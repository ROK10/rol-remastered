r"""Checks a release package the way a new player gets it, on a copy of the game - the game itself is left alone.

  py -3.14 modkit/tools/release_check.py [--copy X:\RoL_cleanroom] [--hd]

1. A vanilla copy: the game folder without modkit copied to --copy, then put back to its original state by its own
   modmgr (this install's state, backups and tools copied along; uninstall-all): the files the mods added removed,
   the archives and legends.exe restored; then its modkit removed. The sha256 of every file is kept.
2. The release unpacked into it (dist/RoL_Remastered_<version>.zip; with --hd also the HD pack's zips) and Install.bat
   run as a player runs it (its pause answered).
3. The copy's modmgr lists the release's mods as enabled (hd_textures only with --hd), and what the mods built - the
   archives, legends.exe, the ui_scale variants, ddraw.dll, rolhook.ini, rolhook.d - has the sha256 of this
   install's: the same mods on the same originals give the same bytes (ddraw.dll: the players' build in
   build\native\player, which the release ships; this install runs the full build). With --hd, the unpacked HD pack has every
   texture of this install's modkit\mods\hd, byte for byte, and nothing else.
4. Uninstall.bat run: every game file back to its vanilla sha256 and nothing left over but modkit and the package's
   loose files (Install.bat, Uninstall.bat, the READMEs), which the player deletes.
A passed check deletes the copy (--keep keeps it, for a test game); a failed one keeps it to look into. Repeatable
for every release.
"""
import argparse
import configparser
import hashlib
import os
import shutil
import stat
import subprocess
import sys
import zipfile

from release import HD_PARTS, HD_SKIP

KIT =os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.dirname(KIT)
LOOSE = {'Install.bat', 'Uninstall.bat', 'Remastered_README_EN.txt', 'Remastered_README_RU.txt'}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def game_files(root):
    """{relative path: sha256} of the game's files, modkit and the package's loose files aside."""
    out = {}
    for base, dirs, files in os.walk(root):
        rel_base = os.path.relpath(base, root)
        if rel_base == '.':
            dirs[:] = [d for d in dirs if d.lower() != 'modkit']
        for n in files:
            rel = os.path.normpath(os.path.join(rel_base, n))
            if rel in LOOSE:
                continue
            out[rel] = sha256(os.path.join(base, n))
    return out


def rmtree(path):
    """shutil.rmtree that also takes the game's read-only files."""
    def onexc(func, p, exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onexc=onexc)


def run(cmd, cwd, stdin=None):
    r = subprocess.run(cmd, cwd=cwd, input=stdin, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        raise SystemExit(f'{cmd} failed (exit {r.returncode}):\n{r.stdout}{r.stderr}')
    return r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--copy', default=os.path.join(os.path.splitdrive(GAME)[0] + os.sep, 'RoL_cleanroom'))
    ap.add_argument('--hd', action='store_true', help="also unpack and check the HD pack's zips")
    ap.add_argument('--keep', action='store_true', help='keep the copy after a passed check')
    args = ap.parse_args()
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(os.path.join(KIT, 'modpack', 'release.ini'), encoding='utf-8')
    version = cfg['release']['version']
    mods = [m.strip() for m in cfg['release']['mods'].split(',') if m.strip()]
    dist = os.path.join(KIT, 'dist')
    zips = [os.path.join(dist, f'RoL_Remastered_{version}.zip')]
    if args.hd:
        zips += [os.path.join(dist, f'RoL_Remastered_{version}_{tag}.zip') for tag, _ in HD_PARTS]
    for z in zips:
        if not os.path.isfile(z):
            raise SystemExit(f'{z} missing: build it first (tools/release.py)')
    copy = os.path.abspath(args.copy)
    if os.path.normcase(copy).startswith(os.path.normcase(GAME)):
        raise SystemExit('the copy must be outside the game folder')

    # 1. vanilla copy
    if os.path.isdir(copy):
        rmtree(copy)
    print(f'1. copying the game (without modkit) to {copy} ...', flush=True)
    shutil.copytree(GAME, copy, ignore=lambda d, names: ['modkit'] if os.path.samefile(d, GAME) else [])
    for sub in ('state', 'backup', 'tools'):
        shutil.copytree(os.path.join(KIT, sub), os.path.join(copy, 'modkit', sub),
                        ignore=shutil.ignore_patterns('__pycache__'))
    run([sys.executable, os.path.join(copy, 'modkit', 'tools', 'modmgr.py'), 'uninstall-all'], copy)
    rmtree(os.path.join(copy, 'modkit'))
    vanilla = game_files(copy)
    print(f'   vanilla: {len(vanilla)} files', flush=True)

    # 2. the release as a player installs it
    for z in zips:
        with zipfile.ZipFile(z) as zf:
            zf.extractall(copy)
    print('2. Install.bat ...', flush=True)
    # by its full path: a cmd started with NoDefaultCurrentDirectoryInExePath (as some shells do) does not look in
    # the current folder; Explorer always gives the full path
    out = run(['cmd', '/c', os.path.join(copy, 'Install.bat')], copy, stdin='\n')
    tail = [l for l in out.splitlines() if l.strip()][-3:]
    print('   ' + '\n   '.join(tail))

    # 3. what it built
    listing = run([os.path.join(copy, 'modkit', 'python', 'python.exe'),
                   os.path.join(copy, 'modkit', 'tools', 'modmgr.py'), 'mods'], copy)
    enabled = {l.split()[1] for l in listing.splitlines() if l.strip().startswith('[x]')}
    want = {m for m in mods if args.hd or m != 'hd_textures'}
    bad = []
    if enabled != want:
        bad.append(f'enabled mods differ: missing {sorted(want - enabled)}, extra {sorted(enabled - want)}')
    compare = ['legends.exe', 'ddraw.dll', 'rolhook.ini', os.path.join('BIGS', 'data.big'),
               os.path.join('BIGS', 'multiplayer_data.big')]
    for n in os.listdir(os.path.join(GAME, 'rolhook.d')):
        if n != 'zz_options.ini' and (args.hd or n != 'hd_textures.ini'):
            compare.append(os.path.join('rolhook.d', n))
    variants = os.path.join('modkit', 'mods', 'ui_scale')
    for base, dirs, files in os.walk(os.path.join(GAME, variants)):
        for n in files:
            compare.append(os.path.relpath(os.path.join(base, n), GAME))
    same = 0
    for rel in compare:
        a, b = os.path.join(GAME, rel), os.path.join(copy, rel)
        if rel == 'ddraw.dll':   # the release ships the players' build; this install runs the full one (tests)
            a = os.path.join(KIT, 'build', 'native', 'player', 'ddraw.dll')
        if not os.path.isfile(b):
            bad.append(f'missing in the copy: {rel}')
        elif sha256(a) != sha256(b):
            bad.append(f'differs from this install: {rel}')
        else:
            same += 1
    print(f'3. {len(enabled)} mods enabled; {same} of {len(compare)} built files byte for byte as in this install')
    if args.hd:
        mine, theirs = os.path.join(KIT, 'mods', 'hd'), os.path.join(copy, 'modkit', 'mods', 'hd')
        want_hd = {}
        for base, dirs, files in os.walk(mine):
            for n in files:
                rel = os.path.relpath(os.path.join(base, n), mine)
                if rel.replace(os.sep, '/') not in HD_SKIP:
                    want_hd[rel] = sha256(os.path.join(base, n))
        got_hd = {}
        for base, dirs, files in os.walk(theirs):
            for n in files:
                got_hd[os.path.relpath(os.path.join(base, n), theirs)] = sha256(os.path.join(base, n))
        off = sorted(r for r in want_hd if got_hd.get(r) != want_hd[r])
        extra_hd = sorted(set(got_hd) - set(want_hd))
        if off or extra_hd:
            bad.append(f'HD pack: {len(off)} files missing or different (e.g. {off[:3]}), {len(extra_hd)} extra')
        print(f'   HD pack: {len(want_hd) - len(off)} of {len(want_hd)} textures as in this install, '
              f'{len(extra_hd)} extra')

    # 4. uninstall
    print('4. Uninstall.bat ...', flush=True)
    run(['cmd', '/c', os.path.join(copy, 'Uninstall.bat')], copy, stdin='\n')
    after = game_files(copy)
    changed = sorted(r for r in vanilla if after.get(r) != vanilla[r])
    extra = sorted(set(after) - set(vanilla))
    if changed:
        bad.append(f'not back to vanilla: {changed[:10]}')
    if extra:
        bad.append(f'left over in the game folder: {extra[:10]}')
    print(f'   {len(vanilla) - len(changed)} of {len(vanilla)} game files back to vanilla, {len(extra)} left over')
    if bad:
        print('FAIL:\n  ' + '\n  '.join(bad) + f'\n  (the copy is kept: {copy})')
        return 1
    if not args.keep:
        rmtree(copy)
    print(f'ok: the release installs, builds what this install has, and uninstalls to vanilla '
          f'({"copy kept: " + copy if args.keep else "copy deleted"})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
