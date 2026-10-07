"""The script engine's native functions in legends.exe: every script function registered by name
(mov eax, native / mov ecx, L"name" / ... / call 0x00C7E010), with its native address, its argument count (stdcall:
the 'ret N' it ends with, N / 4) and the shortest prefix of whole instructions >= 5 bytes without relative jumps or
calls (what rolhook's detour() moves into a trampoline).

  py -3.14 modkit/tools/script_natives.py [name_part ...]          list (all, or names containing a part)
  py -3.14 modkit/tools/script_natives.py --cpp name [name ...]     C++ table rows for rolhook (scriptui.cpp)

Needs pefile + capstone (re_exe.py)."""
import sys

import re_exe as R

REGISTER = 0x00C7E010
RELATIVE = ('j', 'call', 'loop')


def registered():
    """{script name: native address} from the registration sites."""
    out = {}
    for site in R.calls_to(REGISTER):
        fn = name = None
        for i in R.disasm(site - 64, 72):
            if i.address >= site:
                break
            if i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x'):
                fn = int(i.op_str.split(', ')[1], 16)
            if i.mnemonic == 'mov' and i.op_str.startswith('ecx, 0x'):
                name = int(i.op_str.split(', ')[1], 16)
        if not fn or not name or not (R.TEXT_VA <= fn - R.BASE < R.TEXT_VA + R.TEXT_SZ):
            continue
        rva = name - R.BASE
        end = rva
        while R.img[end:end + 2] != b'\x00\x00':
            end += 2
        try:
            s = R.img[rva:end].decode('utf-16-le')
        except UnicodeDecodeError:
            continue
        if s and s.isprintable():
            out[s] = fn
    return out


def arity(fn):
    """Argument count, or None when the function's returns pop different amounts (or none was found)."""
    rets = {int(i.op_str, 16) if i.op_str else 0 for i in R.func(fn, 0x1000) if i.mnemonic == 'ret'}
    return rets.pop() // 4 if len(rets) == 1 else None


def prefix(fn):
    """(length, bytes) of the first whole instructions >= 5 bytes, or None if a relative jump/call comes first."""
    n = 0
    for i in R.disasm(fn, 32):
        if i.mnemonic.startswith(RELATIVE):
            return None
        n += i.size
        if n >= 5:
            return n, R.img[fn - R.BASE:fn - R.BASE + n]
    return None


def main():
    a = sys.argv[1:]
    cpp = '--cpp' in a
    a = [x for x in a if x != '--cpp']
    reg = registered()
    names = [n for n in a if n in reg] if cpp else sorted(n for n in reg if not a or any(p in n for p in a))
    for n in names:
        fn = reg[n]
        k, p = arity(fn), prefix(fn)
        if cpp:
            if k is None or p is None:
                print(f'    // {n}: {"ambiguous ret" if k is None else "no clean prefix"}, skipped')
                continue
            print(f'    {{"{n}", 0x{fn:08X}, {k}, {p[0]}, {{{", ".join(f"0x{b:02X}" for b in p[1])}}}}},')
        else:
            print(f'{n:36} {fn:#010x}  args {k if k is not None else "?"}  '
                  f'prefix {p[0] if p else "-"} {p[1].hex() if p else ""}')
    if cpp:
        missing = [n for n in a if n not in reg]
        if missing:
            print(f'    // not registered: {", ".join(missing)}')
    else:
        print(f'{len(names)} of {len(reg)} script functions')


if __name__ == '__main__':
    main()
