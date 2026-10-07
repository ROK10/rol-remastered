"""Static-analysis helpers for legends.exe (pristine copy in modkit/backup); needs pefile + capstone.

The engine refers to identifiers (feature names, UI id_strings, file names) through 20-byte string handles
{u32 string*, u16, u16 offset, u16, u8 flags (2 = hashes valid), u32 h1, u32 h2}. string_hash() is the engine's
hash (0x00D8C9A0): over the UTF-16 characters from last to first, h += T[i % 50] * c + T[c % 50] * (i + 1) with
the 50-entry table at 0x010E6490; h1 uses the characters as they are, h2 lower-cased (case-insensitive compares
use h2). BIGS/nonloc_strings.big (nonloc_table_lookup.xml: u32 size, u32 1, u32 count, u32 ?, u32 0,
count x {u16 index, u32 h1, u32 h2}, then count x {u32 length, UTF-16 text}; record k = k-th text, 0-based)
lists the strings the code prints or looks up by text. string_refs(name) finds code that builds a handle.

  py -3.14 modkit/tools/re_exe.py advanced_perf scroll_speed     hashes and code references
"""
import os
import struct
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.dirname(KIT)
EXE = os.path.join(KIT, 'backup', 'legends.exe')

pe = pefile.PE(EXE, fast_load=True)
img = pe.get_memory_mapped_image()
BASE = pe.OPTIONAL_HEADER.ImageBase
SECT = {s.Name.rstrip(b'\0').decode(): (s.VirtualAddress, s.Misc_VirtualSize) for s in pe.sections}
TEXT_VA, TEXT_SZ = SECT['.text']
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

_strings = None
_recs = None


def nonloc():
    """(strings list, {index: (h1, h2)}) from the non-localised string table."""
    global _strings, _recs
    if _strings is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from bigtool import BigArchive
        arch = BigArchive.load(os.path.join(GAME, 'BIGS', 'nonloc_strings.big'))
        d = arch.data_of(arch.entries[0])
        count, = struct.unpack_from('<I', d, 8)
        _recs = {}
        for i in range(count):
            k, a, b = struct.unpack_from('<HII', d, 22 + 10 * i)
            _recs[k] = (a, b)
        _strings, p = [], 22 + 10 * count + 1
        for _ in range(count):
            n, = struct.unpack_from('<I', d, p)
            _strings.append(d[p + 4:p + 4 + 2 * n].decode('utf-16-le', 'replace'))
            p += 4 + 2 * n
    return _strings, _recs


_T = None


def string_hash(s):
    """(h1, h2) exactly as the engine computes them for a string handle."""
    global _T
    if _T is None:
        _T = struct.unpack_from('<50i', img, 0x10E6490 - BASE)
    h1 = h2 = 0
    for i in range(len(s) - 1, -1, -1):
        c = ord(s[i])
        lc = ord(s[i].lower()) if len(s[i].lower()) == 1 else c
        h1 = (h1 + _T[i % 50] * c + _T[c % 50] * (i + 1)) & 0xFFFFFFFF
        h2 = (h2 + _T[i % 50] * lc + _T[lc % 50] * (i + 1)) & 0xFFFFFFFF
    return h1, h2


def hashes_of(name):
    return string_hash(name)


def find_u32(val, lo=None, hi=None):
    lo = TEXT_VA if lo is None else lo
    hi = TEXT_VA + TEXT_SZ if hi is None else hi
    pat = struct.pack('<I', val)
    out, i = [], img.find(pat, lo, hi)
    while i >= 0:
        out.append(BASE + i)
        i = img.find(pat, i + 1, hi)
    return out


def string_refs(name):
    """Code addresses that build a string handle for `name` (both hashes stored nearby)."""
    h = hashes_of(name)
    if not h:
        return []
    a = set(find_u32(h[0]))
    b = set(find_u32(h[1])) if h[1] != h[0] else a
    return sorted(x for x in a if any(abs(x - y) <= 16 for y in b))


def func_start(va, limit=0x4000):
    """Walk back to the byte after a run of int3/nop padding (MSVC aligns functions to 16)."""
    rva = va - BASE
    for p in range(rva, rva - limit, -1):
        if p % 16 == 0 and img[p - 1] in (0xCC, 0x90) and img[p] not in (0xCC,):
            # require at least one pad byte before an aligned start
            return BASE + p
    return None


def disasm(va, size=0x200):
    rva = va - BASE
    return list(md.disasm(img[rva:rva + size], va))


def func(va, max_size=0x3000):
    """Instructions of the function starting at va, up to the next int3 padding after a ret."""
    out = []
    for ins in disasm(va, max_size):
        out.append(ins)
        if ins.mnemonic == 'ret' or ins.mnemonic == 'int3':
            nxt = ins.address + ins.size - BASE
            if img[nxt] == 0xCC:
                break
    return out


def calls_to(target):
    """Addresses of E8 rel32 calls to target."""
    out = []
    data = img[TEXT_VA:TEXT_VA + TEXT_SZ]
    i = data.find(b'\xE8')
    while i >= 0:
        if i + 5 <= len(data):
            rel, = struct.unpack_from('<i', data, i + 1)
            if BASE + TEXT_VA + i + 5 + rel == target:
                out.append(BASE + TEXT_VA + i)
        i = data.find(b'\xE8', i + 1)
    return out


def ptr_refs(target, sect='.rdata'):
    va, sz = SECT[sect]
    return find_u32(target, va, va + sz)


if __name__ == '__main__':
    for n in sys.argv[1:]:
        print(n, hashes_of(n), [hex(x) for x in string_refs(n)])
