"""Safe detour lengths for legends.exe functions: the shortest run of whole instructions >= 5 bytes from each address
with no relative jump or call in it (what rolhook's detour() moves into a trampoline). Prints the list in the form
[debug] count_calls takes (address/length, comma-separated).

  py -3.14 modkit/tools/prologue.py 0xEE43A0 0xEE4DC0 ...
"""
import sys

import re_exe as R

RELATIVE = ('j', 'call', 'loop')


def prologue(va):
    n = 0
    for ins in R.disasm(va, 32):
        if ins.mnemonic.startswith(RELATIVE) or 'rip' in ins.op_str:
            return None, f'{ins.mnemonic} {ins.op_str} at +{ins.address - va}'
        n = ins.address + ins.size - va
        if n >= 5:
            return n, ''
    return None, 'too short'


def main():
    out, notes = [], []
    for a in sys.argv[1:]:
        va = int(a, 16)
        n, why = prologue(va)
        if n:
            out.append(f'{va:#010x}/{n}')
            b = bytes(R.img[va - R.BASE:va - R.BASE + n]).hex(' ')
            notes.append(f'  {va:#010x}: {n} bytes ({b}): ' + '; '.join(f'{i.mnemonic} {i.op_str}'
                                                                      for i in R.disasm(va, n) if i.address < va + n))
        else:
            notes.append(f'  {va:#010x}: no safe prologue ({why})')
    print(', '.join(out))
    print('\n'.join(notes))


if __name__ == '__main__':
    main()
