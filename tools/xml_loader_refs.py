"""Where legends.exe loads XML through MSXML 4.0 (c:\\rts2\\main\\basic\\helpxml.cpp): code that refers to the
helper's strings and to the DOMDocument40 CLSID, and the calls to CoCreateInstance, with the instructions around them.
  py -3.14 modkit/tools/xml_loader_refs.py"""
import struct
import uuid

import re_exe as R

STRINGS = ['MSXML 4.0 is not properly installed.', 'XML Error', 'UTF-8']


def find_all(pat):
    out, i = [], R.img.find(pat)
    while i >= 0:
        out.append(R.BASE + i)
        i = R.img.find(pat, i + 1)
    return out


def show(va, before=24, after=48):
    for ins in R.disasm(va - before, before + after + 16):
        if va - before <= ins.address <= va + after:
            print(f'      {ins.address:#x}  {ins.mnemonic} {ins.op_str}')


for s in STRINGS:
    for enc in ('utf-16-le', 'ascii'):
        for va in find_all(s.encode(enc) + (b'\x00\x00' if enc == 'utf-16-le' else b'\x00')):
            refs = R.find_u32(va)
            print(f'{s!r} ({enc}) at {va:#x}: code refs {[hex(r) for r in refs]}')
            for r in refs[:3]:
                show(r)

clsid = uuid.UUID('88D969C0-F192-11D4-A65F-0040963251E5').bytes_le
for va in find_all(clsid):
    refs = R.find_u32(va)
    print(f'CLSID_DOMDocument40 at {va:#x}: code refs {[hex(r) for r in refs]}')
    for r in refs[:4]:
        show(r, 40, 80)
iid = uuid.UUID('2933BF81-7B36-11D2-B20E-00C04F983E60').bytes_le
for va in find_all(iid):
    print(f'IID_IXMLDOMDocument at {va:#x}: code refs {[hex(r) for r in R.find_u32(va)]}')

# CoCreateInstance through the import table: call dword ptr [IAT slot]
import pefile  # noqa: E402
R.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
for imp in R.pe.DIRECTORY_ENTRY_IMPORT:
    for i in imp.imports:
        if i.name == b'CoCreateInstance':
            slot = i.address
            sites = [x - 2 for x in R.find_u32(slot) if R.img[x - 2 - R.BASE:x - R.BASE] == b'\xff\x15']
            print(f'CoCreateInstance IAT slot {slot:#x}: {len(sites)} call sites {[hex(x) for x in sites]}')
