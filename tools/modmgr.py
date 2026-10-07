"""Rise of Legends mod manager: backups, LAA flag, tracked install/uninstall with rollback.

Every file we put into the game folder is recorded in modkit/state/installed.json with its
sha256, so uninstall removes exactly our files (and refuses to delete a file that changed).
Original files we overwrite are copied to modkit/backup/ first and restored on uninstall.

Mods of the modpack live in modkit/modpack/<id>/mod.ini ([mod] id/version/requires/name_ru/description_ru,
[files] dest=src relative to the mod folder, [patch] laa=1, [entries] "archive.big::entry" = replacement file
or "patch:script.py" whose patch(text) edits the entry, see entrypatch.py). Enabling a mod installs its files
(tracked) and applies its patches; dependencies are enabled first; a mod required by an enabled mod cannot be
disabled. [mod] patch_order (default 0) puts a mod's patches after those of lower order; post_rebuild=script.py
runs the script's main() after every enable / disable (ui_scale rebuilds its scaled variants of the archives).

Usage (from the game folder):
  py -3.14 modkit/tools/modmgr.py status
  py -3.14 modkit/tools/modmgr.py mods
  py -3.14 modkit/tools/modmgr.py enable <mod-id>
  py -3.14 modkit/tools/modmgr.py disable <mod-id>
  py -3.14 modkit/tools/modmgr.py diff <mod-id>       what the mod's [entries] change in pristine archives
  py -3.14 modkit/tools/modmgr.py laa on|off
  py -3.14 modkit/tools/modmgr.py install <src> <dest-relative-to-game>
  py -3.14 modkit/tools/modmgr.py remove <dest-relative-to-game>
  py -3.14 modkit/tools/modmgr.py uninstall-all
  py -3.14 modkit/tools/modmgr.py setup              the release's mods (modpack/release.ini), as Install.bat does
"""
import configparser
import difflib
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import sys
import time

GAME = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KIT = os.path.join(GAME, 'modkit')
STATE = os.path.join(KIT, 'state', 'installed.json')
BACKUP = os.path.join(KIT, 'backup')
MODPACK = os.path.join(KIT, 'modpack')
EXE = os.path.join(GAME, 'legends.exe')
IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x20


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def load_state():
    if os.path.exists(STATE):
        with open(STATE, encoding='utf-8') as f:
            return json.load(f)
    return {'files': {}, 'log': []}


def save_state(st):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    tmp = STATE + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(st, f, ensure_ascii=False, indent=1)
    os.replace(tmp, STATE)


def log(st, msg):
    st['log'].append(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}")
    print(msg)


def backup_original(rel):
    """Copy an original game file to modkit/backup once (never overwrite an existing backup)."""
    src = os.path.join(GAME, rel)
    dst = os.path.join(BACKUP, rel)
    if os.path.exists(src) and not os.path.exists(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        with open(dst + '.sha256', 'w') as f:
            f.write(sha256(dst))
        return True
    return False


# ---------- LAA ----------
def pe_characteristics_offset(buf):
    pe_off, = struct.unpack_from('<I', buf, 0x3C)
    if buf[pe_off:pe_off + 4] != b'PE\0\0':
        raise ValueError('not a PE file')
    return pe_off + 4 + 18   # IMAGE_FILE_HEADER.Characteristics


def laa_state():
    with open(EXE, 'rb') as f:
        buf = f.read(4096)
    off = pe_characteristics_offset(buf)
    ch, = struct.unpack_from('<H', buf, off)
    return bool(ch & IMAGE_FILE_LARGE_ADDRESS_AWARE), off, ch


def cmd_laa(on):
    st = load_state()
    enabled, off, ch = laa_state()
    if enabled == on:
        print(f"LAA already {'on' if on else 'off'}")
        return
    if backup_original('legends.exe'):
        log(st, 'backup: legends.exe -> modkit/backup/legends.exe')
    new = ch | IMAGE_FILE_LARGE_ADDRESS_AWARE if on else ch & ~IMAGE_FILE_LARGE_ADDRESS_AWARE
    with open(EXE, 'r+b') as f:
        f.seek(off)
        f.write(struct.pack('<H', new))
    st['laa'] = on
    log(st, f'LAA {"enabled" if on else "disabled"} (Characteristics {ch:#06x} -> {new:#06x})')
    save_state(st)


# ---------- tracked files ----------
def cmd_install(src, rel):
    st = load_state()
    dst = os.path.join(GAME, rel)
    if os.path.exists(dst) and rel.lower() not in {k.lower() for k in st['files']}:
        if backup_original(rel):
            log(st, f'backup: {rel}')
    os.makedirs(os.path.dirname(dst) or GAME, exist_ok=True)
    shutil.copy2(src, dst)
    st['files'][rel] = {'sha256': sha256(dst), 'src': os.path.abspath(src),
                        'replaced_original': os.path.exists(os.path.join(BACKUP, rel))}
    log(st, f'installed {rel} ({os.path.getsize(dst)} bytes)')
    save_state(st)


def remove_one(st, rel):
    info = st['files'].get(rel)
    dst = os.path.join(GAME, rel)
    if info is None:
        print(f'not tracked: {rel}')
        return
    if os.path.exists(dst):
        if sha256(dst) != info['sha256']:
            print(f'REFUSING to delete {rel}: content changed since install')
            return
        os.remove(dst)
    bak = os.path.join(BACKUP, rel)
    if info.get('replaced_original') and os.path.exists(bak):
        shutil.copy2(bak, dst)
        log(st, f'restored original {rel}')
    del st['files'][rel]
    log(st, f'removed {rel}')


def cmd_remove(rel):
    st = load_state()
    remove_one(st, rel)
    save_state(st)


def cmd_uninstall_all():
    st = load_state()
    for rel in list(st['files']):
        remove_one(st, rel)
    for info in st.get('mods', {}).values():
        for rel in info.get('user_files', []):
            p = os.path.join(GAME, rel)
            if os.path.exists(p):
                os.remove(p)
                log(st, f'removed user settings {rel}')
    st.pop('mods', None)
    st.pop('patched_archives', None)
    if laa_state()[0] and st.get('laa'):
        save_state(st)
        cmd_laa(False)
        st = load_state()
    save_state(st)
    print('uninstall complete')


# ---------- modpack ----------
def is_true(v):
    return str(v).strip().lower() in ('1', 'true', 'yes', 'on')


def all_mods():
    if not os.path.isdir(MODPACK):
        return []
    return sorted(d for d in os.listdir(MODPACK) if os.path.isfile(os.path.join(MODPACK, d, 'mod.ini')))


def load_mod(mod_id):
    path = os.path.join(MODPACK, mod_id, 'mod.ini')
    if not os.path.isfile(path):
        raise SystemExit(f'unknown mod: {mod_id} (known: {", ".join(all_mods())})')
    # '=' only: [entries] keys look like "multiplayer_data.big::.\data\gamecamera.xml"
    cfg = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=None, delimiters=('=',))
    cfg.optionxform = str
    cfg.read(path, encoding='utf-8')
    m = dict(cfg['mod']) if cfg.has_section('mod') else {}
    m['id'] = mod_id
    m['dir'] = os.path.dirname(path)
    m['files'] = dict(cfg['files']) if cfg.has_section('files') else {}
    m['patch'] = dict(cfg['patch']) if cfg.has_section('patch') else {}
    m['entries'] = []   # (archive, entry, kind 'file'|'patch', path)
    if cfg.has_section('entries'):
        for key, src in cfg['entries'].items():
            archive, sep, entry = key.partition('::')
            if not sep:
                raise SystemExit(f'{mod_id}: [entries] key must be "archive.big::entry", got {key!r}')
            src = src.strip()
            kind = 'patch' if src.startswith('patch:') else 'file'
            if kind == 'patch':
                src = src[len('patch:'):].strip()
            m['entries'].append((archive.strip(), entry.strip(), kind, os.path.normpath(os.path.join(m['dir'], src))))
    m['requires'] = [r.strip() for r in m.get('requires', '').split(',') if r.strip()]
    # patches of the same archive run by patch_order, then enable order (ui_scale scales what other mods inserted)
    m['patch_order'] = int(m.get('patch_order', '0') or 0)
    # files the game or rolhook writes for this mod (user settings): kept on disable, removed by uninstall-all
    m['user_files'] = [f.strip().replace('\\', '/') for f in m.get('user_files', '').split(',') if f.strip()]
    return m


def pristine_archive(archive):
    """The original archive: the backup if we ever patched it, else the game's own file."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from bigtool import BigArchive
    rel = f'BIGS/{archive}'
    bak = os.path.join(BACKUP, rel)
    return BigArchive.load(bak if os.path.exists(bak) else os.path.join(GAME, rel))


def load_patch_module(mid, path):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # patch scripts import entrypatch / bxml
    spec = importlib.util.spec_from_file_location(f'modpatch_{mid}_{abs(hash(path))}', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_patch_script(mid, path, entry, text):
    module = load_patch_module(mid, path)
    from entrypatch import PatchError
    try:
        out = module.patch(text)
    except PatchError as e:
        raise SystemExit(f'{mid}: patch {os.path.basename(path)} failed on {entry}: {e}')
    if not isinstance(out, str):
        raise SystemExit(f'{mid}: {os.path.basename(path)}: patch() must return str')
    return out


def run_bxml_patch(mid, path, entry, raw):
    """bxml entries (data.big): the script's patch_bxml(bx) edits a bxml.BXml in place."""
    module = load_patch_module(mid, path)
    if not hasattr(module, 'patch_bxml'):
        raise SystemExit(f'{mid}: {entry} is bxml; {os.path.basename(path)} needs patch_bxml(bx)')
    from bxml import BXml
    bx = BXml.parse(raw)
    try:
        module.patch_bxml(bx)
    except (KeyError, IndexError, ValueError) as e:
        raise SystemExit(f'{mid}: patch {os.path.basename(path)} failed on {entry}: {e}')
    return bx.to_bytes()


def apply_entry_steps(arch, archive, steps):
    """Apply (mod_id, entry, kind, path) steps to an archive in order. Patches chain on top of the current
    text of the entry; a replacement file discards what earlier mods did to that entry (with a warning)."""
    touched = {}
    for mid, entry, kind, path in steps:
        e = arch.find(entry)
        if e is None:
            raise SystemExit(f'{mid}: entry {entry} not found in {archive}')
        key = entry.lower()
        if kind == 'file':
            if key in touched:
                print(f'warning: {entry} in {archive}: replacement from {mid} discards changes by '
                      f'{", ".join(touched[key])}')
            with open(path, 'rb') as f:
                arch.set_data(entry, f.read())
        elif e['type'] == 'bxml':
            arch.set_data(entry, run_bxml_patch(mid, path, entry, arch.data_of(e)))
        else:
            raw = arch.data_of(e)
            try:
                text = raw.decode('utf-8')
            except UnicodeDecodeError:
                raise SystemExit(f'{mid}: {entry} is not UTF-8 text; text patches need plain XML')
            arch.set_data(entry, run_patch_script(mid, path, entry, text).encode('utf-8'))
        touched.setdefault(key, []).append(mid)


def rebuild_archives():
    """Rebuild every .big touched by enabled mods' [entries] from its pristine backup.

    All enabled mods are applied by patch_order, then in enable order (patches chain; a replacement file wins over
    earlier mods); an archive no mod patches any more is restored from the backup."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from bigtool import BigArchive

    st = load_state()
    plan = {}
    for i, mid in enumerate(st.get('mods', {})):
        m = load_mod(mid)
        for archive, entry, kind, path in m['entries']:
            plan.setdefault(archive.lower(), (archive, []))[1].append((m['patch_order'], i, (mid, entry, kind, path)))
    plan = {k: (a, [s for _, _, s in sorted(steps, key=lambda x: (x[0], x[1]))]) for k, (a, steps) in plan.items()}
    previously = set(st.get('patched_archives', []))
    for key in sorted(previously | set(plan)):
        if key not in plan:
            remove_one(st, f'BIGS/{key}')
            log(st, f'archive restored: BIGS/{key}')
            continue
        archive, patches = plan[key]
        rel = f'BIGS/{archive}'
        backup_original(rel)
        arch = BigArchive.load(os.path.join(BACKUP, rel))
        apply_entry_steps(arch, archive, patches)
        dst = os.path.join(GAME, rel)
        tmp = dst + '.modmgr.tmp'
        arch.save(tmp)
        os.replace(tmp, dst)
        st['files'][rel] = {'sha256': sha256(dst), 'src': f'archive patch ({len(patches)} entries)',
                            'replaced_original': True}
        log(st, f'archive rebuilt: {rel} with {len(patches)} patched entr{"y" if len(patches) == 1 else "ies"}')
    st['patched_archives'] = sorted(plan)
    save_state(st)


def mod_entry_results(m):
    """Apply one mod's [entries] to pristine archives in memory: {(archive, entry): (before, after)}."""
    by_archive = {}
    for archive, entry, kind, path in m['entries']:
        by_archive.setdefault(archive.lower(), (archive, []))[1].append((m['id'], entry, kind, path))
    out = {}
    for archive, steps in by_archive.values():
        arch = pristine_archive(archive)
        before = {}
        for _, entry, _, _ in steps:
            e = arch.find(entry)
            if e is not None:
                before.setdefault(entry.lower(), arch.data_of(e))
        apply_entry_steps(arch, archive, steps)
        for _, entry, _, _ in steps:
            e = arch.find(entry)
            a, b = before[entry.lower()], arch.data_of(e)
            if e['type'] == 'bxml':   # compare the XML views
                from bxml import BXml
                a, b = BXml.parse(a).to_xml().encode('utf-8'), BXml.parse(b).to_xml().encode('utf-8')
            out[(archive, entry)] = (a, b)
    return out


def dry_run(m):
    mod_entry_results(m)


def cmd_diff(mid):
    m = load_mod(mid)
    if not m['entries']:
        print(f'{mid}: no [entries]')
        return
    for (archive, entry), (before, after) in mod_entry_results(m).items():
        print(f'=== {archive}::{entry}: {len(before)} -> {len(after)} bytes')
        if before == after:
            print('(unchanged)')
            continue
        try:
            a, b = before.decode('utf-8').splitlines(), after.decode('utf-8').splitlines()
        except UnicodeDecodeError:
            print('(binary entry, no text diff)')
            continue
        for line in difflib.unified_diff(a, b, 'pristine', mid, lineterm='', n=1):
            print(line)


def cmd_mods():
    enabled = load_state().get('mods', {})
    for mid in all_mods():
        m = load_mod(mid)
        mark = 'x' if mid in enabled else ' '
        req = f"  (needs {', '.join(m['requires'])})" if m['requires'] else ''
        print(f"  [{mark}] {mid:18s} v{m.get('version', '?'):7s} {m.get('name_ru', m.get('name', ''))}{req}")


def cmd_enable(mid, chain=(), rebuild=True):
    """rebuild=False: the archives are rebuilt and the post-rebuild scripts run by the caller, once for many mods."""
    if mid in chain:
        raise SystemExit('dependency cycle: ' + ' -> '.join(chain + (mid,)))
    m = load_mod(mid)
    for dep in m['requires']:
        if dep not in load_state().get('mods', {}):
            cmd_enable(dep, chain + (mid,), rebuild)
    files = []
    for dest, src in m['files'].items():
        src_path = os.path.normpath(os.path.join(m['dir'], src))
        if not os.path.isfile(src_path):
            raise SystemExit(f'{mid}: source file missing: {src_path} (build it first)')
        cmd_install(src_path, dest.replace('\\', '/'))
        files.append(dest.replace('\\', '/'))
    for archive, entry, kind, path in m['entries']:
        if not os.path.isfile(path):
            raise SystemExit(f'{mid}: {"patch script" if kind == "patch" else "replacement file"} missing: {path}')
    if m['entries']:
        dry_run(m)   # a broken patch must fail before anything is recorded as enabled
    if is_true(m['patch'].get('laa', '0')):
        cmd_laa(True)
    st = load_state()
    st.setdefault('mods', {})[mid] = {'version': m.get('version', ''), 'files': files, 'patch': m['patch'],
                                      'entries': [f'{a}::{e}' for a, e, _, _ in m['entries']],
                                      'user_files': m['user_files']}
    log(st, f"mod enabled: {mid} v{m.get('version', '')}")
    save_state(st)
    if m['entries'] and rebuild:
        rebuild_archives()
    if not chain and rebuild:   # once, after the dependencies too
        run_post_rebuild()


def hd_pack_present():
    hd = os.path.join(KIT, 'mods', 'hd')
    return os.path.isdir(hd) and any(files for _, _, files in os.walk(hd))


def cmd_setup():
    """The release's mods (modpack/release.ini [release] mods = ..., in that order) in one go, as Install.bat runs it:
    each mod's files installed and its patches tried, then the archives rebuilt once and the post-rebuild scripts run
    once. hd_textures only with the HD pack unpacked into modkit/mods/hd."""
    cfg = configparser.ConfigParser(interpolation=None)
    path = os.path.join(MODPACK, 'release.ini')
    if not cfg.read(path, encoding='utf-8') or not cfg.has_section('release'):
        raise SystemExit(f'no [release] section in {path}')
    rel = cfg['release']
    print(f"{rel.get('name', 'modpack')} {rel.get('version', '')}")
    for mid in (x.strip() for x in rel.get('mods', '').split(',')):
        if not mid:
            continue
        if mid == 'hd_textures' and not hd_pack_present():
            print('hd_textures skipped: the HD pack is not unpacked (modkit/mods/hd); unpack it into the game folder '
                  'and run the installer again')
            continue
        cmd_enable(mid, rebuild=False)
    rebuild_archives()
    run_post_rebuild()
    print('setup complete')


def run_post_rebuild():
    """[mod] post_rebuild=<script.py> of every enabled mod: its main(), after the archives changed (ui_scale builds
    its scaled variants from them)."""
    for mid in load_state().get('mods', {}):
        m = load_mod(mid)
        script = m.get('post_rebuild', '').strip()
        if not script:
            continue
        path = os.path.normpath(os.path.join(m['dir'], script))
        if not os.path.isfile(path):
            raise SystemExit(f'{mid}: post_rebuild script missing: {path}')
        load_patch_module(mid, path).main()


def cmd_disable(mid):
    st = load_state()
    enabled = st.get('mods', {})
    if mid not in enabled:
        print(f'not enabled: {mid}')
        return
    dependents = [o for o in enabled if o != mid and mid in load_mod(o)['requires']]
    if dependents:
        raise SystemExit(f'cannot disable {mid}: required by {", ".join(dependents)}')
    info = enabled[mid]
    for rel in info.get('files', []):
        remove_one(st, rel)
    st['mods'].pop(mid, None)
    save_state(st)
    if info.get('entries'):
        rebuild_archives()
    if is_true(info.get('patch', {}).get('laa', '0')):
        cmd_laa(False)
    st = load_state()
    log(st, f'mod disabled: {mid}')
    for rel in info.get('user_files', []):
        if os.path.exists(os.path.join(GAME, rel)):
            print(f'kept user settings {rel} (delete it to drop them; uninstall-all removes it)')
    save_state(st)
    run_post_rebuild()


def cmd_status():
    st = load_state()
    enabled, _, ch = laa_state()
    print(f'game: {GAME}')
    print(f'legends.exe LAA: {"ON" if enabled else "off"} (Characteristics {ch:#06x}); backup: '
          f'{"yes" if os.path.exists(os.path.join(BACKUP, "legends.exe")) else "no"}')
    print(f'enabled mods: {", ".join(st.get("mods", {})) or "-"}')
    print(f'patched archives: {", ".join(st.get("patched_archives", [])) or "-"}')
    print(f'tracked files: {len(st["files"])}')
    for rel, info in st['files'].items():
        dst = os.path.join(GAME, rel)
        ok = os.path.exists(dst) and sha256(dst) == info['sha256']
        print(f'  {"ok " if ok else "CHANGED/MISSING"} {rel}')


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding='utf-8', errors='replace')
        except (AttributeError, ValueError):
            pass
    a = sys.argv[1:]
    if not a or a[0] == 'status':
        cmd_status()
    elif a[0] == 'mods':
        cmd_mods()
    elif a[0] == 'enable' and len(a) == 2:
        cmd_enable(a[1])
    elif a[0] == 'disable' and len(a) == 2:
        cmd_disable(a[1])
    elif a[0] == 'diff' and len(a) == 2:
        cmd_diff(a[1])
    elif a[0] == 'laa' and len(a) == 2 and a[1] in ('on', 'off'):
        cmd_laa(a[1] == 'on')
    elif a[0] == 'install' and len(a) == 3:
        cmd_install(a[1], a[2])
    elif a[0] == 'remove' and len(a) == 2:
        cmd_remove(a[1])
    elif a[0] == 'uninstall-all':
        cmd_uninstall_all()
    elif a[0] == 'setup':
        cmd_setup()
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
