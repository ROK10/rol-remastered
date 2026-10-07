"""Which interface pictures the scale leaves small: the sprite logs ([debug] log_sprites, autotest cases ui_sprites
and ui_sprites_100) of two games at different interface scales, lined up by texture and texel rectangle.
A picture laid out by the scaled layouts grows by the scale; one placed by code keeps its screen size. The logs
since rolhook 0.9.9 also name the code that drew a picture (the return address of its sprite call, and the next
one up the stack - the caller of a drawing helper) and where on screen it went (the sprite's x, y plus UIRenderMgr's
offset).
With one session (a game at the scale with no counterpart at 100 %, e.g. the campaign map), the pictures drawn at
exactly their texel size, by caller - at the scale those are the suspects.
  py -3.14 modkit/tools/ui_sprites.py <session at 100 %> <session at the scale> [scale, default 1.5] [--all]
  py -3.14 modkit/tools/ui_sprites.py <session at the scale>"""
import collections
import os
import re
import sys

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSIONS = os.path.join(KIT, 'logs', 'sessions')
LINE = re.compile(r'sprite (.+?) \| texels (-?\d+),(-?\d+) (-?\d+)x(-?\d+) \| screen (-?\d+)x(-?\d+) at (-?\d+),(-?\d+)'
                  r'(?: \(offset (-?\d+),(-?\d+)\) from (0x[0-9A-Fa-f]+)(?: < (0x[0-9A-Fa-f]+))?)?')


def load(session):
    """{(texture, texel rect): [(w, h, x, y, offset x, offset y, caller)]}"""
    out = collections.defaultdict(list)
    path = session if os.path.isfile(session) else os.path.join(SESSIONS, session, 'ui.log')
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE.search(line)
            if m:
                key = (m.group(1), tuple(int(m.group(i)) for i in range(2, 6)))
                where = tuple(int(m.group(i)) for i in range(6, 10))
                caller = m.group(12) + (' < ' + m.group(13) if m.group(13) else '') if m.group(12) else None
                extra = (int(m.group(10)), int(m.group(11)), caller) if m.group(12) else (None, None, None)
                out[key].append(where + extra)
    return out


def describe(item):
    w, h, x, y, ox, oy, caller = item
    if caller is None:
        return f'at {x},{y}'
    return f'at {x + ox},{y + oy} on screen (sprite {x},{y} + offset {ox},{oy}), from {caller}'


def natural(session):
    pics = load(session)
    by_caller = collections.defaultdict(list)
    total = 0
    for (tex, (tl, tt, tw, th)), items in pics.items():
        for it in items:
            if (it[0], it[1]) == (abs(tw), abs(th)) and abs(tw) > 2 and abs(th) > 2:
                by_caller[it[6] or '?'].append((tex, tl, tt, tw, th, it))
                total += 1
    print(f'{len(pics)} pictures; drawn at their texel size: {total}')
    for caller, items in sorted(by_caller.items(), key=lambda kv: -len(kv[1])):
        print(f'  from {caller}: {len(items)}')
        for tex, tl, tt, tw, th, it in items[:8]:
            print(f'      {tex} texels {tl},{tt} {tw}x{th} {describe(it)}')


def main():
    files = [x for x in sys.argv[1:] if not x.startswith('--')]
    if len(files) == 1:
        natural(files[0])
        return
    a, b = load(sys.argv[1]), load(sys.argv[2])
    nums = [x for x in sys.argv[3:] if not x.startswith('--')]
    scale = float(nums[0]) if nums else 1.5
    common = [k for k in a if k in b]
    kept = collections.defaultdict(list)
    grown = 0
    for k in common:
        wa = max(it[0] for it in a[k])
        wb = max(it[0] for it in b[k])
        if wa <= 2:
            continue
        r = wb / wa
        if r > scale - 0.15:
            grown += 1
        elif r < 1.15:
            kept[k[0]].append((k[1], wa, wb, b[k][0]))
    print(f'{len(a)} pictures at 100 %, {len(b)} at the scale, {len(common)} in both; grown by ~{scale}: {grown}; '
          f'kept their size: {sum(len(v) for v in kept.values())}')
    every = '--all' in sys.argv
    for tex, items in sorted(kept.items(), key=lambda kv: -len(kv[1])):
        print(f'  {tex}: {len(items)}')
        for (tl, tt, tw, th), wa, wb, item in items[:None if every else 6]:
            pa = sorted({describe(it) for it in a[(tex, (tl, tt, tw, th))]})[:2]
            print(f'      texels {tl},{tt} {tw}x{th}: screen width {wa} -> {wb}, {describe(item)}; at 100 %: '
                  f'{"; ".join(pa)}')
    only_b = collections.Counter(k[0] for k in b if k not in a)
    print('only at the scale (new pictures or different texels):', ', '.join(f'{t} {n}' for t, n in only_b.most_common(8)))
    callers = collections.Counter(it[6] for v in b.values() for it in v if it[6])
    if callers:
        print('pictures by caller at the scale:', ', '.join(f'{c} {n}' for c, n in callers.most_common(12)))


if __name__ == '__main__':
    main()
