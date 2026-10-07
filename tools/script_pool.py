"""The constant pools of compiled game scripts (scripts.big) and where the bytecode pushes a constant - for naming the
push sites of a script's pixel constants (modpack/ui_scale/layout.py SCRIPT_SITES: the int and the strings the
bytecode pushes nearest before and after it).

  py -3.14 modkit/tools/script_pool.py <string> [context, default 8]       every compiled script holding the string
  py -3.14 modkit/tools/script_pool.py --entry <entry name> [from] [to]     one entry's pool, by index
  py -3.14 modkit/tools/script_pool.py --pushes <int> [entry name part]     every push of an int, with its neighbours
"""
import os
import struct
import sys

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(KIT, 'tools'))
sys.path.insert(0, os.path.join(KIT, 'modpack', 'ui_scale'))
import layout  # noqa: E402
import modmgr  # noqa: E402


def entries():
    arc = modmgr.pristine_archive(layout.SCRIPT_ARCHIVE)
    for e in arc.entries:
        yield e['name'], arc.data_of(e)


def show(name, pool, lo, hi):
    print(f'== {name}: {len(pool)} constants')
    for i in range(max(0, lo), min(len(pool), hi)):
        kind, off, v = pool[i]
        print(f'  {i:5d}  {kind:4s}  {v!r}')


def pushes(name, data, value):
    try:
        pool = layout.pool(data)
    except (ValueError, StopIteration):
        return
    end = layout.code_end(data)
    strings, i = [], 0
    while (i := data.find(b'\x28', i, end)) >= 0:
        k = struct.unpack_from('<i', data, i + 1)[0]
        if 0 <= k < len(pool) and pool[k][0] != 'int':
            strings.append((i + 1, pool[k][2]))
        i += 1
    for k, (kind, _, v) in enumerate(pool):
        if kind != 'int' or v != value:
            continue
        for site in layout.const_refs(data, k):
            before = [s for o, s in strings if o < site][-2:]
            after = [s for o, s in strings if o > site][:2]
            print(f'{name} #{k} push at {site:#x}: before {before} after {after}')


def main():
    a = sys.argv[1:]
    if a and a[0] == '--entry':
        for name, data in entries():
            if name.lower() == a[1].lower():
                p = layout.pool(data)
                show(name, p, int(a[2]) if len(a) > 2 else 0, int(a[3]) if len(a) > 3 else len(p))
        return
    if a and a[0] == '--pushes':
        part = a[2].lower() if len(a) > 2 else ''
        for name, data in entries():
            if part in name.lower():
                pushes(name, data, int(a[1]))
        return
    text, ctx = a[0], int(a[1]) if len(a) > 1 else 8
    for name, data in entries():
        if text.encode('utf-16-le') not in data:
            continue
        p = layout.pool(data)
        for i, (kind, off, v) in enumerate(p):
            if kind != 'int' and v == text:
                show(name, p, i - ctx, i + ctx + 1)


if __name__ == '__main__':
    main()
