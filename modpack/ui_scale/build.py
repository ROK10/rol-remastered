r"""ui_scale: builds the scaled variants rolhook serves at the game's start ([ui] layout_scale, Mods tab):

  mods/ui_scale/<pct>/BIGS/ui.big, multiplayer_data.big   the installed archives' content (every enabled mod's
                                                            patches, as modmgr builds them) with layout.FILES scaled
  mods/ui_scale/<pct>/BIGS/scripts.big                    the campaign scripts' pixel constants scaled
  mods/ui_scale/<pct>/campaigns/<nation>/<nation>.campaign  the campaign files with their interface scaled

for pct in VARIANTS. rolhook reads BIGS\ui.big as mods\ui_scale\150\BIGS\ui.big and so on when the scale is 150;
at 100 nothing is redirected, the game reads its own files. Fonts need no files: rolhook rasterizes them at the
scale once the scaled layouts are in use. modmgr runs this after every archive rebuild (mod.ini post_rebuild), so
the variants always carry the other mods' patches too.

  py -3.14 modkit/modpack/ui_scale/build.py
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(KIT, 'tools'))
import layout  # noqa: E402

VARIANTS = (125, 150, 175, 200)
OUT = os.path.join(KIT, 'mods', 'ui_scale')


def mod_steps(archive):
    """Patch steps of the enabled mods for one archive, in modmgr's order (patch_order, then enable order)."""
    import modmgr
    st = modmgr.load_state()
    steps = []
    for i, mid in enumerate(st.get('mods', {})):
        if mid == 'ui_scale':
            continue
        m = modmgr.load_mod(mid)
        for a, entry, kind, path in m['entries']:
            if a.lower() == archive.lower():
                steps.append((m['patch_order'], i, (mid, entry, kind, path)))
    return [s for _, _, s in sorted(steps, key=lambda x: (x[0], x[1]))]


def build_archive(archive, pct, dst):
    import modmgr
    arch = modmgr.pristine_archive(archive)
    modmgr.apply_entry_steps(arch, archive, mod_steps(archive))
    for a, entry in layout.FILES:
        if a.lower() != archive.lower():
            continue
        e = arch.find(entry)
        if e is None:
            raise SystemExit(f'ui_scale: {entry} not found in {archive}')
        arch.set_data(entry, layout.scale_layout(arch.data_of(e).decode('utf-8'), pct).encode('utf-8'))
    if archive.lower() == layout.SCRIPT_ARCHIVE:
        for entry, sites in layout.SCRIPT_SITES.items():
            e = arch.find(entry)
            if e is None:
                raise SystemExit(f'ui_scale: {entry} not found in {archive}')
            try:
                arch.set_data(entry, layout.scale_script(arch.data_of(e), sites, pct))
            except ValueError as err:
                raise SystemExit(f'ui_scale: {entry}: {err}')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp'
    arch.save(tmp)
    os.replace(tmp, dst)


def build_campaign(rel, pct, dst):
    """A campaign file (game folder, no mod patches them) with its interface scaled, bytes kept outside it."""
    with open(os.path.join(os.path.dirname(KIT), rel), 'rb') as f:
        text = f.read().decode('utf-8')
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(layout.scale_campaign(text, pct).encode('utf-8'))
    os.replace(tmp, dst)


def main():
    archives = sorted({a for a, _ in layout.FILES} | {layout.SCRIPT_ARCHIVE})
    for name in os.listdir(OUT) if os.path.isdir(OUT) else []:
        if name.isdigit() and int(name) not in VARIANTS:
            shutil.rmtree(os.path.join(OUT, name))   # a variant dropped from the list
    if os.path.isdir(os.path.join(OUT, 'loc')):
        shutil.rmtree(os.path.join(OUT, 'loc'))      # 0.4 layout: one scale installed over the game's files
    for pct in VARIANTS:
        root = os.path.join(OUT, str(pct))
        if os.path.isdir(os.path.join(root, 'loc')):
            shutil.rmtree(os.path.join(root, 'loc'))   # 0.5: scaled fonts.xml (rolhook scales the fonts now)
        for archive in archives:
            build_archive(archive, pct, os.path.join(root, 'BIGS', archive))
        for rel in layout.CAMPAIGN_FILES:
            build_campaign(rel, pct, os.path.join(root, rel))
    print(f'ui_scale: variants {", ".join(f"{p}%" for p in VARIANTS)} in {OUT} ({", ".join(archives)}, '
          f'{len(layout.CAMPAIGN_FILES)} campaign files)')


if __name__ == '__main__':
    main()
