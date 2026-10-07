"""Interface textures: the ones the game loaded (textures.log of every session) under the interface folders, with
size, format, archive and how many layout TEX rectangles refer to them; and the TEXLOAD files of the layouts the game
was never seen loading. --archives: every interface texture in the archives instead, per folder (DDS headers).
  py -3.14 modkit/tools/ui_textures.py [--all] [--archives]"""
import collections
import glob
import os
import re
import struct
import sys

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.dirname(KIT)
sys.path.insert(0, os.path.join(KIT, 'tools'))
sys.path.insert(0, os.path.join(KIT, 'modpack', 'ui_scale'))
from bigtool import BigArchive  # noqa: E402
import bxml  # noqa: E402
import layout  # noqa: E402

LOADED = re.compile(r'\| (HD|memEx|fileEx)\s+(\S+) :: (\S+) \| src (\S+) (\d+)x(\d+) mips (\d+)')
UI_DIRS = ('art\\interface\\', 'ui\\', 'art\\ctw\\ctw_', 'art\\ctw\\profile_', 'art\\ctw\\territory_',
           'art\\ctw\\scenario_images\\')


def is_ui(path):
    p = path.lower()
    return any(p.startswith(d) for d in UI_DIRS)


def layouts():
    """(source, text) of every layout the ui_scale mod scales, plus the campaign files' interfaces."""
    out = []
    archives = {}
    for big, entry in layout.FILES:
        if big not in archives:
            archives[big] = BigArchive.load(os.path.join(GAME, 'BIGS', big))
        e = archives[big].find(entry)
        d = BigArchive.data_of(e)
        t = d.decode('utf-8-sig') if d[:1] in (b'<', b'\xef') else bxml.BXml.parse(d).to_xml()
        out.append((entry, t))
    for rel in layout.CAMPAIGN_FILES:
        t = open(os.path.join(GAME, rel), encoding='utf-8').read()
        m = layout.INTERFACE.search(t)
        out.append((rel, m.group(0)))
    return out


def dds_info(d):
    """(width, height, format, mip count) from a DDS header, or None."""
    if d[:4] != b'DDS ':
        return None
    h, w = struct.unpack_from('<II', d, 12)
    flags = struct.unpack_from('<I', d, 80)[0]
    fmt = d[84:88].decode('ascii', 'replace') if flags & 4 else f'RGB{struct.unpack_from("<I", d, 88)[0]}'
    return w, h, fmt, struct.unpack_from('<I', d, 28)[0]


def category(n):
    """The interface HD plan's class of a texture (hd/ui_art.py: E for CG renders and glossy shapes, P for materials)."""
    b = os.path.basename(n)
    if n.startswith('ui\\cursors\\'):
        return 'cursor'
    if n.startswith('art\\ctw\\scenario_images\\'):
        return 'illustration'
    if n.startswith('art\\interface\\icons\\') or b.startswith(('profile_', 'territory_')) or b == 'numbers.tga':
        return 'icon'
    if n.startswith('art\\interface\\buttons\\') or b.startswith('main_ui'):
        return 'button'
    return 'material'


def archives_inventory():
    rows = []
    for big in sorted(glob.glob(os.path.join(GAME, 'BIGS', '*.big'))):
        arch = BigArchive.load(big)
        for e in arch.entries:
            n = (e['name'] or '').lstrip('.\\').lower()
            if n.endswith(('.tga', '.dds')) and is_ui(n):
                rows.append((os.path.basename(big), n, dds_info(BigArchive.data_of(e))))
    by = collections.defaultdict(list)
    for big, n, info in rows:
        by[(big, os.path.dirname(n))].append((n, info))
    total_px = 0
    for (big, d), items in sorted(by.items()):
        ok = [i for _, i in items if i]
        px = sum(w * h for w, h, _, _ in ok)
        total_px += px
        fmts = collections.Counter(f for _, _, f, _ in ok)
        sizes = collections.Counter(f'{w}x{h}' for w, h, _, _ in ok)
        print(f'{big:22} {d:34} {len(items):4} tex {px / 1e6:6.2f} Mpx  {dict(fmts)}  {dict(sizes.most_common(4))}')
        if '--all' in sys.argv:
            for n, i in items:
                print(f'    {os.path.basename(n):44} {i}')
    print(f'{len(rows)} interface textures in the archives, {total_px / 1e6:.1f} Mpx')
    # x2 budget per class: uncompressed sources stay A8R8G8B8 (4 bytes), DXT ones keep their family; mips +1/3
    cats = collections.defaultdict(lambda: [0, 0, 0.0])
    for big, n, info in rows:
        if not info:
            continue
        w, h, fmt, _ = info
        c = cats[category(n)]
        c[0] += 1
        c[1] += w * h
        bpp = 4 if fmt.startswith('RGB') else (0.5 if fmt == 'DXT1' else 1)
        c[2] += w * h * 4 * bpp * 4 / 3
    print('x2 budget:')
    for k, (cnt, px, size) in sorted(cats.items()):
        print(f'  {k:13} {cnt:4} textures  {px / 1e6:6.1f} Mpx -> {px * 4 / 1e6:6.1f} Mpx  {size / 2**20:7.0f} MB')


def main():
    if '--archives' in sys.argv:
        archives_inventory()
        return
    loaded = {}
    for log in glob.glob(os.path.join(KIT, 'logs', 'sessions', '*', 'textures.log')):
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = LOADED.search(line)
                if m and is_ui(m.group(3)):
                    loaded[m.group(3).lower()] = (m.group(2), m.group(4), int(m.group(5)), int(m.group(6)))
    # TEXLOAD id -> file, and TEX rectangles per texture id, over all layouts
    files_by_id = collections.defaultdict(set)
    rects = collections.Counter()
    for src, t in layouts():
        for m in re.finditer(r'<TEXLOAD\b[^>]*\bid="([^"]+)"[^>]*\bfile="([^"]+)"', t):
            files_by_id[m.group(1)].add(m.group(2).lstrip('.\\').lower())
        for m in re.finditer(r'<TEX\b[^>]*\btexture="([^"]+)"', t):
            rects[m.group(1)] += 1
    rects_by_file = collections.Counter()
    for tid, files in files_by_id.items():
        for f in files:
            rects_by_file[f] += rects[tid]
    by_dir = collections.defaultdict(list)
    for path, (arch, fmt, w, h) in sorted(loaded.items()):
        by_dir[os.path.dirname(path)].append((path, arch, fmt, w, h))
    total = 0
    for d, items in sorted(by_dir.items()):
        px = sum(w * h for _, _, _, w, h in items)
        total += len(items)
        print(f'{d:42} {len(items):4} textures, {px / 1e6:6.2f} Mpx')
        if '--all' in sys.argv:
            for path, arch, fmt, w, h in items:
                print(f'    {os.path.basename(path):44} {w:5}x{h:<5} {fmt:7} {arch:18} TEX refs {rects_by_file.get(path, 0)}')
    print(f'{total} interface textures loaded in the sessions')
    never = sorted(f for files in files_by_id.values() for f in files if f not in loaded)
    print(f'{len(never)} TEXLOAD files of the layouts never seen loaded: {", ".join(never[:20])}{" ..." if len(never) > 20 else ""}')
    print('most referenced atlases:', ', '.join(f'{os.path.basename(f)} {n}' for f, n in rects_by_file.most_common(15)))


if __name__ == '__main__':
    main()
