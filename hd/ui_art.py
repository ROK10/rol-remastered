"""Interface art x2: atlases upscaled cell by cell.

The interface textures are atlases: pictures (icons, button pieces, frame slices) separated by 1-px magenta lines
(and transparent gaps). A model run over the whole atlas mixes the separators into the pictures' edges - Real-ESRGAN
x4plus turned the grey frame of every unit icon green (magenta's complement), PBRify SPAN pink - and the edge texels
of a cell are exactly what the game draws. So:
  * separators: opaque magenta texels (#FF00FF, a little tolerance); they come back as pure magenta, nearest x2;
  * cells: the connected pieces between them; each is cut out with its bounding box, texels outside the piece filled
    with the piece's nearest texel, padded by GUTTER texels of its own edge, and packed with the others into one
    mosaic - one model run per atlas, no cell sees another;
  * transparent texels get the colour of the nearest opaque one first (no dark or magenta fringes along silhouettes);
  * colour through the chosen model (x4), alpha through Real-ESRGAN x4plus as a grey picture (clean edges; a constant
    alpha stays constant), both to x2 with Lanczos; colour locked to the original per cell (colorlock).
Models: 'E' Real-ESRGAN x4plus (ncnn runner) - icons, portraits, glyphs: clean, faithful shapes; 'P' PBRify SPAN
Neutral (spandrel, ComfyUI python) - materials (metal, wood, leather, paper): keeps their grain. Chosen on the
comparison of 2026-10-06 (uitest sheets: x4plus-anime flattens and over-saturates, animevideov3 rings).
"""
import os
import subprocess
import tempfile

import numpy as np
from PIL import Image
from scipy import ndimage

ESRGAN_EXE = r'X:\VN_upscaler\runners\realesrgan-ncnn\realesrgan-ncnn-vulkan.exe'
GUTTER = 12          # source texels of a cell's own edge around it in the mosaic
MOSAIC_W = 2048      # source texels per mosaic row
MIN_CELL = 3         # smaller pieces (stray texels) are only resized


def esrgan_x4(rgb, model='realesrgan-x4plus'):
    """RGB x4 through the Real-ESRGAN ncnn runner (its own process)."""
    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, 'a.png'), os.path.join(tmp, 'b.png')
        Image.fromarray(np.ascontiguousarray(rgb)).save(a)
        subprocess.run([ESRGAN_EXE, '-i', a, '-o', b, '-n', model, '-s', '4', '-f', 'png'],
                       cwd=os.path.dirname(ESRGAN_EXE), check=True, capture_output=True)
        return np.asarray(Image.open(b).convert('RGB'))


def pbrify_x4(rgb):
    """RGB x4 through PBRify SPAN Neutral (spandrel; the ComfyUI python only)."""
    import gan_upscale
    import pack
    return gan_upscale.upscale_rgb(np.ascontiguousarray(rgb), pack.model_file('4x-PBRify_UpscalerSPAN_Neutral*'),
                                   tileable=False)


MODELS = {'E': esrgan_x4, 'P': pbrify_x4}


def separators(rgba):
    r, g, b, a = (rgba[..., i].astype(np.int16) for i in range(4))
    return (r >= 235) & (g <= 25) & (b >= 235) & (a >= 128)


def nearest_fill(arr, keep):
    """arr with the texels outside 'keep' set to their nearest texel inside it (all channels)."""
    if keep.all() or not keep.any():
        return arr
    idx = ndimage.distance_transform_edt(~keep, return_distances=False, return_indices=True)
    return arr[idx[0], idx[1]]


def resize(arr, size):
    if arr.ndim == 2:
        return np.asarray(Image.fromarray(arr).resize(size, Image.LANCZOS))
    return np.dstack([np.asarray(Image.fromarray(np.ascontiguousarray(arr[..., c])).resize(size, Image.LANCZOS))
                      for c in range(arr.shape[2])])


def cells(sep):
    """Connected pieces between the separators: [(y0, y1, x0, x1, mask)] (mask over the bounding box)."""
    lab, n = ndimage.label(~sep)
    out = []
    for i, sl in enumerate(ndimage.find_objects(lab), 1):
        if sl is None:
            continue
        mask = lab[sl] == i
        out.append((sl[0].start, sl[0].stop, sl[1].start, sl[1].stop, mask))
    return out


def pack_tiles(tiles):
    """Shelf packing of padded tiles (h, w): [(y, x)], mosaic height and width."""
    x = y = shelf = 0
    width = MOSAIC_W
    pos = []
    for h, w in tiles:
        if x and x + w > width:
            y += shelf
            x = shelf = 0
        pos.append((y, x))
        x += w
        shelf = max(shelf, h)
        width = max(width, w)
    return pos, y + shelf, width


def upscale_atlas(rgba, method, color_lock=True):
    """rgba: HxWx4 uint8 original -> 2Hx2Wx4 uint8."""
    import colorlock
    h, w = rgba.shape[:2]
    sep = separators(rgba)
    opaque = rgba[..., 3] > 0
    work = rgba.copy()
    if opaque.any() and not opaque.all():   # transparent texels take the nearest visible colour
        work[..., :3] = nearest_fill(rgba[..., :3], opaque & ~sep)
    pieces = [c for c in cells(sep) if c[1] - c[0] >= MIN_CELL and c[3] - c[2] >= MIN_CELL]
    tiles = []
    for y0, y1, x0, x1, mask in pieces:
        crop = nearest_fill(work[y0:y1, x0:x1], mask)
        tiles.append(np.pad(crop, ((GUTTER, GUTTER), (GUTTER, GUTTER), (0, 0)), mode='edge'))
    out = resize(work, (w * 2, h * 2))   # stray texels and anything not in a piece
    if tiles:
        pos, mh, mw = pack_tiles([t.shape[:2] for t in tiles])
        mosaic = np.zeros((mh, mw, 4), np.uint8)
        for (ty, tx), t in zip(pos, tiles):
            mosaic[ty:ty + t.shape[0], tx:tx + t.shape[1]] = t
        rgb4 = MODELS[method](mosaic[..., :3])
        alpha = mosaic[..., 3]
        a4 = None if alpha.min() == alpha.max() else esrgan_x4(np.repeat(alpha[..., None], 3, axis=2)).mean(axis=2)
        for (ty, tx), t, (y0, y1, x0, x1, mask) in zip(pos, tiles, pieces):
            ch, cw = y1 - y0, x1 - x0
            sy, sx = (ty + GUTTER) * 4, (tx + GUTTER) * 4
            rgb2 = resize(rgb4[sy:sy + ch * 4, sx:sx + cw * 4], (cw * 2, ch * 2))
            if color_lock and ch >= 4 and cw >= 4:
                rgb2 = colorlock.color_lock(rgb2, np.ascontiguousarray(work[y0:y1, x0:x1, :3]))
            if a4 is None:
                a2 = np.full((ch * 2, cw * 2), alpha[0, 0], np.uint8)
            else:
                a2 = resize(np.clip(a4[sy:sy + ch * 4, sx:sx + cw * 4] + 0.5, 0, 255).astype(np.uint8),
                            (cw * 2, ch * 2))
                if rgba[y0:y1, x0:x1, 3][mask].min() == 0:
                    a2 = np.where(a2 < 2, 0, a2).astype(np.uint8)
            m2 = np.repeat(np.repeat(mask, 2, axis=0), 2, axis=1)
            region = out[y0 * 2:y1 * 2, x0 * 2:x1 * 2]
            region[m2] = np.dstack([rgb2, a2])[m2]
    if sep.any():
        s2 = np.repeat(np.repeat(sep, 2, axis=0), 2, axis=1)
        out[s2] = np.repeat(np.repeat(rgba, 2, axis=0), 2, axis=1)[s2]
    return out
