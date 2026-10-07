"""Rise of Legends .big ("WAR-BUILDER") archive tool: list / extract / pack / replace / verify.

Format (all little-endian, wstr = u32 char count + UTF-16LE):
  u32 version (=11), u8 flag (=1), wstr builder machine name ("WAR-BUILDER"; some patch archives
  carry "DESKTOP-MATT" / "PLAYTEST-10" — the engine does not check it)
  vector deps:    u32 count; if count: u32 capacity, u16 0xFFFF, u8 0; then {wstr path, u32 unix_ts}
  vector entries: u32 count; if count: u32 capacity, u16 0xFFFF, u8 0; then entries:
      wstr name, u32 0, u32 0, u32 data_offset, u32 unpacked_size, u32 0, u32 unix_ts (0xFFFFFFFF = generated),
      wstr type ("", "bxml", "fxo", "bhs", "string_lookup", ...), u8 flags, u8 has_flags
  data (contiguous, in entry order): at data_offset -> u32 packed_size + zlib stream

Entry flag bits mirror bigmanifest.xml attributes: 1 = check_timestamp_against_orig,
4 = binary (generated), 8 = check_timestamp_against_orig_single_only. Textures use 0/0.
Note: the original BHG builder truncated the zlib stream to unpacked_size when compression
expanded the data (one broken entry: multiplayer_data.big/.\\data\\texture_cache.xml).
This tool always writes complete streams.
"""
import argparse
import fnmatch
import json
import os
import struct
import sys
import zlib

DEFAULT_BUILDER = 'WAR-BUILDER'


class Reader:
    def __init__(self, buf, pos=0):
        self.buf, self.pos = buf, pos

    def u8(self):
        v = self.buf[self.pos]
        self.pos += 1
        return v

    def u16(self):
        v, = struct.unpack_from('<H', self.buf, self.pos)
        self.pos += 2
        return v

    def u32(self):
        v, = struct.unpack_from('<I', self.buf, self.pos)
        self.pos += 4
        return v

    def wstr(self):
        n = self.u32()
        s = self.buf[self.pos:self.pos + 2 * n].decode('utf-16le')
        self.pos += 2 * n
        return s


def _vec_header(r):
    count = r.u32()
    if count == 0:
        return 0, None
    cap, marker, zero = r.u32(), r.u16(), r.u8()
    return count, {'capacity': cap, 'marker': marker, 'zero': zero}


class BigArchive:
    """In-memory model of a .big archive. Entries keep raw packed bytes until modified."""

    def __init__(self):
        self.version = 11
        self.flag = 1
        self.builder = DEFAULT_BUILDER
        self.deps = []            # list of (path, ts)
        self.deps_capacity = 0
        self.entries = []         # list of dict(name, type, ts, flags, has_flags, usize, packed|data, u0, u1, u2)

    # ---------- reading ----------
    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            buf = f.read()
        return cls.from_bytes(buf)

    @classmethod
    def from_bytes(cls, buf):
        a = cls()
        r = Reader(buf)
        a.version, a.flag = r.u32(), r.u8()
        a.builder = r.wstr()
        if a.version != 11 or not a.builder or len(a.builder) > 64:
            raise ValueError(f'not a RoL .big (version {a.version}, builder {a.builder[:40]!r})')
        ndeps, dh = _vec_header(r)
        a.deps_capacity = dh['capacity'] if dh else 0
        for _ in range(ndeps):
            a.deps.append((r.wstr(), r.u32()))
        nents, _ = _vec_header(r)
        for _ in range(nents):
            name = r.wstr()
            u0, u1, off, usize, u2, ts = (r.u32() for _ in range(6))
            typ = r.wstr()
            flags, has_flags = r.u8(), r.u8()
            plen, = struct.unpack_from('<I', buf, off)
            a.entries.append(dict(name=name, type=typ, ts=ts, flags=flags, has_flags=has_flags, usize=usize,
                                  packed=bytes(buf[off + 4:off + 4 + plen]), u0=u0, u1=u1, u2=u2))
        return a

    # ---------- access ----------
    def find(self, name):
        key = name.lower()
        for e in self.entries:
            if e['name'].lower() == key:
                return e
        return None

    @staticmethod
    def data_of(e):
        if 'data' in e:
            return e['data']
        d = zlib.decompressobj()
        out = d.decompress(e['packed'], e['usize'])
        return out

    def set_data(self, name, data, ts=None, typ=None, flags=None, create=True):
        e = self.find(name)
        if e is None:
            if not create:
                raise KeyError(name)
            e = dict(name=name, type=typ or '', ts=0, flags=0, has_flags=0, usize=0, u0=0, u1=0, u2=0)
            self.entries.append(e)
        e['data'] = bytes(data)
        e['usize'] = len(data)
        e.pop('packed', None)
        if ts is not None:
            e['ts'] = ts
        if typ is not None:
            e['type'] = typ
        if flags is not None:
            e['flags'], e['has_flags'] = flags, 1 if flags else 0
        return e

    # ---------- writing ----------
    def to_bytes(self, level=6):
        def wstr(s):
            b = s.encode('utf-16le')
            return struct.pack('<I', len(b) // 2) + b

        packed = []
        for e in self.entries:
            if 'packed' in e and 'data' not in e:
                packed.append(e['packed'])
            else:
                packed.append(zlib.compress(e['data'], level))

        head = bytearray()
        head += struct.pack('<IB', self.version, self.flag) + wstr(self.builder)
        if self.deps:
            head += struct.pack('<IIHB', len(self.deps), max(self.deps_capacity, len(self.deps)), 0xFFFF, 0)
            for p, ts in self.deps:
                head += wstr(p) + struct.pack('<I', ts)
        else:
            head += struct.pack('<I', 0)
        # entry table size is independent of offsets, so measure it first
        table_len = 0
        if self.entries:
            table_len += 4 + 4 + 2 + 1
            for e in self.entries:
                table_len += len(wstr(e['name'])) + 24 + len(wstr(e['type'])) + 2
        else:
            table_len = 4
        off = len(head) + table_len
        table = bytearray()
        if self.entries:
            table += struct.pack('<IIHB', len(self.entries), len(self.entries), 0xFFFF, 0)
            for e, p in zip(self.entries, packed):
                table += wstr(e['name'])
                table += struct.pack('<6I', e.get('u0', 0), e.get('u1', 0), off, e['usize'], e.get('u2', 0), e['ts'])
                table += wstr(e['type']) + struct.pack('<BB', e['flags'], e['has_flags'])
                off += 4 + len(p)
        else:
            table += struct.pack('<I', 0)
        assert len(table) == table_len
        out = bytearray(head + table)
        for p in packed:
            out += struct.pack('<I', len(p)) + p
        return bytes(out)

    def save(self, path, level=6):
        data = self.to_bytes(level)
        tmp = path + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(data)
        os.replace(tmp, path)
        return len(data)


# ---------- helpers for extract/pack ----------
def entry_relpath(e):
    name = e['name'] or e['type']
    rel = name.lstrip('.\\/').replace('\\', os.sep)
    if not rel:
        rel = e['type']
    if e['type'] == 'bxml':
        rel += '.bxml'
    return rel


def cmd_list(args):
    a = BigArchive.load(args.big)
    print(f'{args.big}: version={a.version} deps={len(a.deps)} entries={len(a.entries)}')
    for p, ts in a.deps:
        print(f'  dep {p} ts={ts}')
    for e in a.entries:
        if args.filter and not fnmatch.fnmatch(e['name'].lower(), args.filter.lower()):
            continue
        print(f"  {e['usize']:10d} {len(e.get('packed', b'')):10d} {e['type']:16s} f={e['flags']}/{e['has_flags']} ts={e['ts']:10d} {e['name']}")


def cmd_extract(args):
    a = BigArchive.load(args.big)
    os.makedirs(args.out, exist_ok=True)
    manifest = dict(version=a.version, flag=a.flag, builder=a.builder, deps=a.deps, deps_capacity=a.deps_capacity,
                    entries=[])
    n = 0
    for e in a.entries:
        rel = entry_relpath(e)
        if args.filter and not fnmatch.fnmatch(e['name'].lower(), args.filter.lower()):
            continue
        p = os.path.join(args.out, rel)
        os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
        with open(p, 'wb') as f:
            f.write(BigArchive.data_of(e))
        manifest['entries'].append(dict(name=e['name'], type=e['type'], ts=e['ts'], flags=e['flags'],
                                        has_flags=e['has_flags'], file=rel.replace(os.sep, '/')))
        n += 1
    with open(os.path.join(args.out, '_big_manifest.json'), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    print(f'extracted {n} entries -> {args.out}')


def cmd_pack(args):
    with open(os.path.join(args.dir, '_big_manifest.json'), encoding='utf-8') as f:
        m = json.load(f)
    a = BigArchive()
    a.version, a.flag = m.get('version', 11), m.get('flag', 1)
    a.builder = m.get('builder', DEFAULT_BUILDER)
    a.deps = [tuple(x) for x in m.get('deps', [])]
    a.deps_capacity = m.get('deps_capacity', len(a.deps))
    for me in m['entries']:
        with open(os.path.join(args.dir, me['file'].replace('/', os.sep)), 'rb') as f:
            data = f.read()
        e = dict(name=me['name'], type=me.get('type', ''), ts=me.get('ts', 0), flags=me.get('flags', 0),
                 has_flags=me.get('has_flags', 0), usize=len(data), data=data, u0=0, u1=0, u2=0)
        a.entries.append(e)
    size = a.save(args.out, args.level)
    print(f'packed {len(a.entries)} entries -> {args.out} ({size} bytes)')


def cmd_replace(args):
    a = BigArchive.load(args.big)
    for spec in args.entry:
        name, _, src = spec.partition('=')
        with open(src, 'rb') as f:
            data = f.read()
        existed = a.find(name) is not None
        a.set_data(name, data, ts=int(os.path.getmtime(src)) if args.touch else None)
        print(f"{'replaced' if existed else 'added'} {name} <- {src} ({len(data)} bytes)")
    size = a.save(args.out, args.level)
    print(f'wrote {args.out} ({size} bytes)')


def verify_archive(path, roundtrip=False, level=6):
    """Parse, decompress every entry, optionally rebuild and compare. Returns list of problems."""
    problems = []
    with open(path, 'rb') as f:
        orig = f.read()
    a = BigArchive.from_bytes(orig)
    datas = []
    for e in a.entries:
        d = BigArchive.data_of(e)
        if len(d) != e['usize']:
            problems.append(f"{e['name']}: unpacked {len(d)} != {e['usize']}")
        datas.append(d)
    if roundtrip:
        rebuilt_same_packed = a.to_bytes(level)
        if rebuilt_same_packed != orig:
            problems.append('rebuild with original packed streams is not byte-identical')
        b = BigArchive.from_bytes(rebuilt_same_packed)
        # full recompress path
        for e, d in zip(a.entries, datas):
            e['data'] = d
        c = BigArchive.from_bytes(a.to_bytes(level))
        for e1, e2, d in zip(a.entries, c.entries, datas):
            if (e1['name'], e1['type'], e1['ts'], e1['flags'], e1['has_flags']) != \
               (e2['name'], e2['type'], e2['ts'], e2['flags'], e2['has_flags']):
                problems.append(f"{e1['name']}: metadata mismatch after recompress")
            if BigArchive.data_of(e2) != d:
                problems.append(f"{e1['name']}: payload mismatch after recompress")
        if len(c.deps) != len(a.deps):
            problems.append('deps mismatch')
    return a, problems


def cmd_verify(args):
    bad = 0
    for path in args.big:
        a, probs = verify_archive(path, roundtrip=args.roundtrip, level=args.level)
        status = 'OK' if not probs else f'{len(probs)} problem(s)'
        print(f'{os.path.basename(path):28s} entries={len(a.entries):5d} {status}')
        for p in probs[:10]:
            print('    ', p)
        bad += bool(probs)
    sys.exit(1 if bad else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list'); p.add_argument('big'); p.add_argument('--filter'); p.set_defaults(fn=cmd_list)
    p = sub.add_parser('extract'); p.add_argument('big'); p.add_argument('out'); p.add_argument('--filter'); p.set_defaults(fn=cmd_extract)
    p = sub.add_parser('pack'); p.add_argument('dir'); p.add_argument('out'); p.add_argument('--level', type=int, default=6); p.set_defaults(fn=cmd_pack)
    p = sub.add_parser('replace'); p.add_argument('big'); p.add_argument('out')
    p.add_argument('--entry', action='append', required=True, help='name=path (name as stored, e.g. .\\art\\x.tga)')
    p.add_argument('--touch', action='store_true', help='set entry timestamp to the source file mtime')
    p.add_argument('--level', type=int, default=6); p.set_defaults(fn=cmd_replace)
    p = sub.add_parser('verify'); p.add_argument('big', nargs='+'); p.add_argument('--roundtrip', action='store_true')
    p.add_argument('--level', type=int, default=6); p.set_defaults(fn=cmd_verify)
    args = ap.parse_args()
    args.fn(args)


if __name__ == '__main__':
    main()
