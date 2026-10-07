r"""Binary XML ("bxml", the data.big entries) — read, view as XML, edit values, write back.

Layout (little-endian; vectors as in .big archives: u32 count, then if count > 0: u32 capacity, u16 0xFFFF, u8 0):
  u32 version (1)
  vector nodes, 12 bytes each: u16 parent, u16 first_child, u16 next_sibling (0xFFFF = none),
                               u16 item_count, u32 item_start
  vector items, 8 bytes each:  u32 packed = name (12 bits) | value << 12 (16 bits, index into values) | type << 28,
                               u32 data (int, or float bits; 0 for strings)
       a node's items: [0] its tag, then its attributes in order. The tag item's value is the element's text
       (values[0] is always '' = no text). Types (data in brackets): 0 no value / plain tag, 1 bool ("true" 1),
       2 hex ("0xc0ffffff"), 3 negative int ("-1" 0xFFFFFFFF), 4 non-negative int, 5 float (bits; source text
       kept: "1000.0f", ".30000001"), 6 string (0). A tag with text has the text's type
       (<KEYMAPVERSION>121</KEYMAPVERSION>: type 4, data 121)
  vector names  of wstr (u32 length + UTF-16): tag and attribute names, lower-cased
  vector values of wstr: attribute values as written in the source XML (".26666665", "Linear"), de-duplicated
  trailer: whatever follows (4 zero bytes in the files seen), kept as is
The engine compares names through the case-insensitive hash, so lower-cased names are what it sees.

  py -3.14 bxml.py show data.big .\data\lod.xml          print an entry as XML
  py -3.14 bxml.py verify data.big                       round-trip every bxml entry of an archive
"""
import os
import struct
import sys
from xml.sax.saxutils import escape, quoteattr

T_TAG, T_BOOL, T_HEX, T_NEG_INT, T_INT, T_FLOAT, T_STRING = 0, 1, 2, 3, 4, 5, 6
NONE = 0xFFFF


class Node:
    __slots__ = ('parent', 'first_child', 'next_sibling', 'item_count', 'item_start')

    def __init__(self, *v):
        self.parent, self.first_child, self.next_sibling, self.item_count, self.item_start = v


class Item:
    __slots__ = ('name', 'value', 'type', 'data')

    def __init__(self, name, value, type_, data):
        self.name, self.value, self.type, self.data = name, value, type_, data   # value: index into values


class BXml:
    def __init__(self):
        self.version = 1
        self.nodes, self.items, self.names, self.values = [], [], [], []
        self.caps = {}       # vector capacities as read (written back unchanged)
        self.trailer = b''

    # ---------------------------------------------------------------- read / write
    @classmethod
    def parse(cls, b):
        x, p = cls(), 0

        def u32():
            nonlocal p
            v, = struct.unpack_from('<I', b, p)
            p += 4
            return v

        def vec(key):
            nonlocal p
            n = u32()
            if n:
                x.caps[key] = u32()
                if b[p:p + 3] != b'\xff\xff\x00':
                    raise ValueError(f'bad vector marker at {p:#x}')
                p += 3
            return n

        def wstr():
            nonlocal p
            n = u32()
            s = b[p:p + 2 * n].decode('utf-16-le')
            p += 2 * n
            return s

        x.version = u32()
        for _ in range(vec('nodes')):
            x.nodes.append(Node(*struct.unpack_from('<4HI', b, p)))
            p += 12
        for _ in range(vec('items')):
            packed, data = struct.unpack_from('<II', b, p)
            p += 8
            x.items.append(Item(packed & 0xFFF, (packed >> 12) & 0xFFFF, packed >> 28, data))
        x.names = [wstr() for _ in range(vec('names'))]
        x.values = [wstr() for _ in range(vec('values'))]
        x.trailer = bytes(b[p:])
        return x

    def to_bytes(self):
        out = bytearray(struct.pack('<I', self.version))

        def vec(key, n):
            out.extend(struct.pack('<I', n))
            if n:
                out.extend(struct.pack('<I', max(self.caps.get(key, n), n)) + b'\xff\xff\x00')

        def wstr(s):
            out.extend(struct.pack('<I', len(s)) + s.encode('utf-16-le'))

        vec('nodes', len(self.nodes))
        for n in self.nodes:
            out.extend(struct.pack('<4HI', n.parent, n.first_child, n.next_sibling, n.item_count, n.item_start))
        vec('items', len(self.items))
        for it in self.items:
            if it.name > 0xFFF or it.value > 0xFFFF or it.type > 0xF:
                raise ValueError('item field out of range')
            out.extend(struct.pack('<II', it.name | (it.value << 12) | (it.type << 28), it.data & 0xFFFFFFFF))
        vec('names', len(self.names))
        for s in self.names:
            wstr(s)
        vec('values', len(self.values))
        for s in self.values:
            wstr(s)
        out.extend(self.trailer)
        return bytes(out)

    # ---------------------------------------------------------------- tree access
    def tag(self, i):
        n = self.nodes[i]
        return self.names[self.items[n.item_start].name]

    def attrs(self, i):
        """[(name, item index)] of node i."""
        n = self.nodes[i]
        return [(self.names[self.items[k].name], k) for k in range(n.item_start + 1, n.item_start + n.item_count)]

    def children(self, i):
        c = self.nodes[i].first_child
        while c != NONE:
            yield c
            c = self.nodes[c].next_sibling

    def value_text(self, k):
        it = self.items[k]
        return self.values[it.value] if it.value < len(self.values) else ''

    def text(self, i):
        """Element text of node i ('' if none)."""
        return self.value_text(self.nodes[i].item_start)

    def find(self, *path, **attr_match):
        """Nodes whose tag path from the root matches 'path' (lower-case tags; '*' = any) and whose attributes
        include attr_match (values compared as text, case-insensitive)."""
        out = []

        def walk(i, depth):
            if path[depth] not in ('*', self.tag(i)):
                return
            if depth == len(path) - 1:
                a = {n: self.value_text(k).lower() for n, k in self.attrs(i)}
                if all(a.get(key.lower()) == str(v).lower() for key, v in attr_match.items()):
                    out.append(i)
                return
            for c in self.children(i):
                walk(c, depth + 1)

        walk(0, 0)
        return out

    def get(self, node, attr):
        for n, k in self.attrs(node):
            if n == attr.lower():
                return self.value_text(k)
        raise KeyError(f'<{self.tag(node)}> has no attribute {attr!r}')

    def set(self, node, attr, value):
        """Set an existing attribute; its type stays (int / float / string). The source text is updated too:
        a new value string is appended (strings are shared between attributes)."""
        for n, k in self.attrs(node):
            if n != attr.lower():
                continue
            it = self.items[k]
            text = str(value)
            if it.type in (T_INT, T_NEG_INT):
                v = int(value)
                it.type, it.data, text = (T_NEG_INT if v < 0 else T_INT), v & 0xFFFFFFFF, str(v)
            elif it.type == T_HEX:
                it.data = int(str(value), 0) & 0xFFFFFFFF if isinstance(value, str) else int(value) & 0xFFFFFFFF
                text = f'0x{it.data:08x}'
            elif it.type == T_BOOL:
                b = value if isinstance(value, bool) else str(value).strip().lower() in ('1', 'true', 'yes')
                it.data, text = int(b), 'true' if b else 'false'
            elif it.type == T_FLOAT:
                it.data = struct.unpack('<I', struct.pack('<f', float(value)))[0]
            if text in self.values:
                it.value = self.values.index(text)
            else:
                self.values.append(text)
                it.value = len(self.values) - 1
            return
        raise KeyError(f'<{self.tag(node)}> has no attribute {attr!r}')

    # ---------------------------------------------------------------- view
    def to_xml(self, i=0, depth=0):
        pad = '  ' * depth
        a = ''.join(f' {n}={quoteattr(self.value_text(k))}' for n, k in self.attrs(i))
        kids, text, tag = list(self.children(i)), self.text(i), self.tag(i).upper()
        if not kids:
            return f'{pad}<{tag}{a}>{escape(text)}</{tag}>\n' if text else f'{pad}<{tag}{a}/>\n'
        inner = ''.join(self.to_xml(c, depth + 1) for c in kids)
        return f'{pad}<{tag}{a}>{escape(text)}\n{inner}{pad}</{tag}>\n'


def _cli():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from bigtool import BigArchive
    game = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] in ('show', 'verify'):
        arch = BigArchive.load(os.path.join(game, 'BIGS', a[1]))
    if len(a) == 3 and a[0] == 'show':
        sys.stdout.reconfigure(encoding='utf-8')
        print(BXml.parse(arch.data_of(arch.find(a[2]))).to_xml(), end='')
    elif len(a) == 2 and a[0] == 'verify':
        bad = 0
        for e in arch.entries:
            if e['type'] != 'bxml':
                continue
            raw = arch.data_of(e)
            x = BXml.parse(raw)
            ok = x.to_bytes() == raw
            bad += not ok
            print(f"{'ok  ' if ok else 'DIFF'} {e['name']}: {len(x.nodes)} nodes, {len(x.items)} items, "
                  f"{len(x.names)} names, {len(x.values)} values, trailer {x.trailer.hex() or '-'}")
        print('round trip:', 'all identical' if not bad else f'{bad} differ')
    else:
        print(__doc__)


if __name__ == '__main__':
    _cli()
