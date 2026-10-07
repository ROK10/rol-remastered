r"""Every shader of every effect, disassembled without the game: fxobjects.big (and patches\patch7's) unpacked, the
shader bytecode found inside each compiled effect (.fxo, fx_2_0) and disassembled by D3DX.

  py -3.14 modkit/tools/fxdump.py [out dir, default modkit/work/fxdump] [--filter water]

An fx_2_0 binary keeps each shader as a DWORD byte count followed by the bytecode, which starts with its version token
(vs FFFE/ps FFFF + major.minor) and ends with the end token 0000FFFF - so a shader is taken only where the count
before a version token lands exactly on an end token. The disassembly is D3DXDisassembleShader of the 64-bit
C:\Windows\System32\d3dx9_27.dll (the same D3DX as the game's 32-bit copy), constant table included: the parameters'
names in their registers. Out: <effect>.<n>.<vs|ps>_<ver>.txt, n in the order found. Game-derived: stays in work/.
"""
import argparse
import ctypes
import os
import struct
import sys

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.dirname(KIT)
sys.path.insert(0, os.path.join(KIT, 'tools'))
import bigtool  # noqa: E402

VERSIONS = {0xFFFE0101: 'vs_1_1', 0xFFFE0200: 'vs_2_0', 0xFFFE0201: 'vs_2_x', 0xFFFE0300: 'vs_3_0',
            0xFFFF0101: 'ps_1_1', 0xFFFF0102: 'ps_1_2', 0xFFFF0103: 'ps_1_3', 0xFFFF0104: 'ps_1_4',
            0xFFFF0200: 'ps_2_0', 0xFFFF0201: 'ps_2_x', 0xFFFF0300: 'ps_3_0'}


def d3dx():
    dll = ctypes.WinDLL(r'C:\Windows\System32\d3dx9_27.dll')
    fn = dll.D3DXDisassembleShader
    fn.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    fn.restype = ctypes.c_long
    return fn


def buffer_text(buf):
    vt = ctypes.cast(ctypes.cast(buf, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    proto = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)
    size_proto = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    ptr = proto(vt[3])(buf)
    size = size_proto(vt[4])(buf)
    text = ctypes.string_at(ptr, size).split(b'\0', 1)[0].decode('latin-1')
    release(buf)
    return text


def shaders_in(blob):
    """[(offset, bytes, version name)] of the shaders in an fx_2_0 binary."""
    out = []
    for off in range(4, len(blob) - 8, 4):
        tok, = struct.unpack_from('<I', blob, off)
        if tok not in VERSIONS:
            continue
        size, = struct.unpack_from('<I', blob, off - 4)
        if size < 8 or size % 4 or off + size > len(blob):
            continue
        if struct.unpack_from('<I', blob, off + size - 4)[0] != 0x0000FFFF:
            continue
        out.append((off, blob[off:off + size], VERSIONS[tok]))
    return out


def entries(archive):
    a = bigtool.BigArchive.load(archive)
    for e in a.entries:
        name = e['name'].replace('\\', '/').split('/')[-1]
        if name.endswith('.fxo'):
            yield name[:-4], a.data_of(e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?', default=os.path.join(KIT, 'work', 'fxdump'))
    ap.add_argument('--filter', default='')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    dis = d3dx()
    n_fx = n_sh = 0
    seen = set()
    for arch in (os.path.join(GAME, 'BIGS', 'fxobjects.big'), os.path.join(GAME, 'BIGS', 'patches', 'patch7', 'fxobjects.big')):
        if not os.path.isfile(arch):
            continue
        tag = '' if 'patches' not in arch else 'patch7.'
        for name, blob in entries(arch):
            if a.filter and a.filter not in name:
                continue
            n_fx += 1
            for i, (off, code, ver) in enumerate(shaders_in(blob)):
                key = (name, code)
                if key in seen:
                    continue
                seen.add(key)
                cbuf = ctypes.create_string_buffer(code, len(code))
                out = ctypes.c_void_p()
                if dis(ctypes.addressof(cbuf), 0, None, ctypes.byref(out)) < 0 or not out:
                    continue
                with open(os.path.join(a.out, f'{tag}{name}.{i:02d}.{ver}.txt'), 'w', encoding='utf-8') as f:
                    f.write(buffer_text(out))
                n_sh += 1
    print(f'{n_fx} effects, {n_sh} shaders disassembled into {a.out}')


if __name__ == '__main__':
    main()
