r"""The campaign map ("Conquer the World"): art\ctw\dome_tiles in ctw.big, a mosaic of tiles around the map's centre.

  <q>_<i>,<j>          colour (DXT1/RGB32A, 1024 px; 512/256 where only sea)        q = ne | nw | se | sw
  <q>_<i>,<j>_         normal map (1024, or 256/64 under sea)
  <q>_<i>,<j>_b        region borders: white, the line and its glow in the alpha (DXT5 1024)
  <q>_<i>,<j>cuotl_b   the same for the Cuotl campaign
  <q>_<i>,<j>_b_       region mask (256): left alone

Indices count from the centre of the map: i is the row away from the middle line (north tiles upwards, south ones
downwards), j the column away from the middle (west tiles to the left, east ones to the right). Read off the tiles'
edges (scratchpad ctw/adjacency.py, 2026-10-06): sw_1,0 | se_1,0, nw_0,3 over sw_0,3, se_2,0 | se_2,1 continue one
another to within DXT noise. A tile upscaled on its own makes up different detail on the two sides of a shared
edge, so every tile goes through the upscaler with a frame of its neighbours' pixels (padded()) and is cut out
afterwards. Neighbours of another size are resampled to the tile's density; an edge that does not continue
(EDGE_MAX) or a missing neighbour is mirrored instead.
"""
import os
import re

import numpy as np
from PIL import Image

from common import load_texture

ARCHIVE = 'ctw.big'
PREFIX = '.\\art\\ctw\\dome_tiles\\'
NAME = re.compile(r'(ne|nw|se|sw)_(\d+),(\d+)(_|cuotl_b|_b)?')
EDGE_MAX = 18.0    # mean |a - b| over a shared edge (0..255) above which the tiles are taken as not touching
SEAM_MAX = 4.0     # an HD edge step above this (and over twice the original's) counts as a seam


def parse(entry):
    """(map, quadrant, i, j, variant) for a dome tile entry, or None. map: diffuse | normal | border; variant ''
    or 'cuotl' (borders)."""
    base = os.path.splitext(entry.replace('/', '\\').split('\\')[-1])[0].lower()
    m = NAME.fullmatch(base)
    if not m:
        return None
    q, i, j, suffix = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4) or ''
    kind = {'': 'diffuse', '_': 'normal', '_b': 'border', 'cuotl_b': 'border'}[suffix]
    return kind, q, i, j, ('cuotl' if suffix == 'cuotl_b' else '')


def grid(q, i, j):
    """Position in the mosaic (x to the right, y downwards); the centre is between columns -1|0 and rows -1|0."""
    x = j if q[1] == 'e' else -1 - j
    y = -1 - i if q[0] == 'n' else i
    return x, y


def entry_of(kind, q, i, j, variant=''):
    suffix = {'diffuse': '', 'normal': '_', 'border': 'cuotl_b' if variant else '_b'}[kind]
    return f'{PREFIX}{q}_{i},{j}{suffix}.tga'


_index = None


def index(entries):
    """{(kind, variant, x, y): entry} over the given archive entries (built once)."""
    global _index
    if _index is None:
        _index = {}
        for e in entries:
            p = parse(e) if e.lower().startswith(PREFIX) else None
            if p:
                kind, q, i, j, v = p
                _index[(kind, v, *grid(q, i, j))] = e
    return _index


def _load(entry, size):
    """RGBA of a tile at size x size."""
    img = load_texture(ARCHIVE, entry)
    if img.shape[0] != size or img.shape[1] != size:
        img = np.asarray(Image.fromarray(img).resize((size, size), Image.LANCZOS))
    return img


def _edge(a, b, side):
    """Mean |a - b| (RGB) between a's border and b's facing border: side = (dx, dy) of b relative to a."""
    dx, dy = side
    if dx == 1:
        ea, eb = a[:, -1], b[:, 0]
    elif dx == -1:
        ea, eb = a[:, 0], b[:, -1]
    elif dy == 1:
        ea, eb = a[-1, :], b[0, :]
    else:
        ea, eb = a[0, :], b[-1, :]
    return float(np.abs(ea[:, :3].astype(np.float32) - eb[:, :3].astype(np.float32)).mean())


def padded(entry, pad, entries):
    """The tile's RGBA framed by 'pad' texels of its neighbours (mirrored where none continues it); the tile itself
    is at [pad:pad + h, pad:pad + w]."""
    kind, q, i, j, v = parse(entry)
    idx = index(entries)
    x, y = grid(q, i, j)
    tile = load_texture(ARCHIVE, entry)
    n = tile.shape[0]
    out = np.pad(tile, ((pad, pad), (pad, pad), (0, 0)), mode='reflect')

    def neighbour(dx, dy):
        nb = idx.get((kind, v, x + dx, y + dy))
        if nb is None and kind == 'border' and v:   # a Cuotl border tile next to a plain one
            nb = idx.get((kind, '', x + dx, y + dy))
        return nb

    def place(nb, dx, dy):
        other = _load(nb, n)
        ys = slice(0, pad) if dy == -1 else (slice(pad, pad + n) if dy == 0 else slice(pad + n, pad + n + pad))
        xs = slice(0, pad) if dx == -1 else (slice(pad, pad + n) if dx == 0 else slice(pad + n, pad + n + pad))
        sy = slice(n - pad, n) if dy == -1 else (slice(0, n) if dy == 0 else slice(0, pad))
        sx = slice(n - pad, n) if dx == -1 else (slice(0, n) if dx == 0 else slice(0, pad))
        out[ys, xs] = other[sy, sx]

    sides = {}
    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):   # a side neighbour must continue the shared edge
        nb = neighbour(dx, dy)
        if nb is not None and _edge(tile, _load(nb, n), (dx, dy)) <= EDGE_MAX:
            place(nb, dx, dy)
            sides[(dx, dy)] = nb
    used = list(sides.values())
    for dx, dy in ((-1, -1), (1, -1), (-1, 1), (1, 1)):   # a corner one when both sides next to it continue
        nb = neighbour(dx, dy)
        if nb is not None and (dx, 0) in sides and (0, dy) in sides:
            place(nb, dx, dy)
            used.append(nb)
    return out, used


def seams(entries, hd_path, decode, kind='diffuse'):
    """Seam check of the upscaled tiles: for every pair of touching tiles of 'kind' that both have an HD texture
    (hd_path(entry) -> path), the mean |difference| across the shared edge (0..255, RGB). Returns
    [(entry, neighbour, side, hd_cross, original_cross)] and the edges whose HD step is a seam: more than SEAM_MAX
    levels and over twice the original's step (the painting itself and DXT noise are not perfectly continuous)."""
    idx = index(entries)
    rows, bad = [], []
    for (k, v, x, y), e in sorted(idx.items()):
        if k != kind:
            continue
        for dx, dy in ((1, 0), (0, 1)):
            nb = idx.get((k, v, x + dx, y + dy))
            if nb is None or not os.path.exists(hd_path(e)) or not os.path.exists(hd_path(nb)):
                continue
            oa, ob = load_texture(ARCHIVE, e), load_texture(ARCHIVE, nb)
            if oa.shape != ob.shape or _edge(oa, ob, (dx, dy)) > EDGE_MAX:
                continue   # not continuous in the original either
            ha, hb = decode(hd_path(e)), decode(hd_path(nb))
            if ha.shape != hb.shape:
                continue
            row = (e, nb, 'right' if dx else 'below', _edge(ha, hb, (dx, dy)), _edge(oa, ob, (dx, dy)))
            rows.append(row)
            if row[3] > SEAM_MAX and row[3] > 2 * row[4]:
                bad.append(row)
    return rows, bad
