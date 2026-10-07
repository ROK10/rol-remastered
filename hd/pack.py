r"""HD pack builder: turns upscaled RGB results into game-ready DDS files that rolhook serves instead of the originals.

For every texture:
  * RGB  — the chosen chain's result (colour-locked);
  * A    — the original alpha upscaled on its own: hard cut-out masks (wall ornaments, holes) get crisp anti-aliased
           edges, soft masks (propeller discs) a smooth Lanczos; values the engine's alpha test would let through as
           faint noise (< 8, see renderstates.xml: alpha_func Greater 0 / GreaterEqual 8) are clamped to 0;
  * normal maps — N3: low frequencies of the original normal map (the artist's depth) + high frequencies of a
           PBRify NormalV3 map computed from the new diffuse; the original's alpha channel (specular/detail) is
           carried over, upscaled;
  * encoding — texconv into the original's family (DXT1 -> BC1, DXT5 -> BC3) with a full mip chain; the result is
           decoded back and compared with the source image.

Output: <out>/<entry path without the leading .\ and with .dds>, the layout rolhook's [textures] override expects.
Run with the ComfyUI portable Python (torch + spandrel for NormalV3, opencv, texture2ddecoder).
"""
import glob
import json
import os
import subprocess
import tempfile

import cv2
import numpy as np
from PIL import Image

from common import MODELS, TEXCONV, load_texture, resize, BigArchive, dds, GAME  # noqa: F401
import colorlock

ALPHA_FLOOR = 8          # alpha below this is treated as fully transparent
ALPHA_CEIL = 247         # and above this as fully opaque


def model_file(pattern):
    hits = sorted(glob.glob(os.path.join(MODELS, 'upscale_models', pattern)))
    if not hits:
        raise SystemExit(f'model not found: {pattern} in {MODELS}\\upscale_models')
    return hits[0]


def source_format(big, entry):
    import common
    if big not in common._archives:   # the same cache load_texture uses: an archive is read from disk once
        common._archives[big] = BigArchive.load(os.path.join(GAME, 'BIGS', big))
    arch = common._archives[big]
    return dds.info(BigArchive.data_of(arch.find(entry)))


# ---------------------------------------------------------------- alpha
def is_binary_mask(a):
    return np.isin(a, (0, 255)).mean() > 0.97


def upscale_alpha(a, size):
    """a: hxw uint8 -> size (w, h). Hard masks: smooth upscale, then an edge curve ~1.5 output px wide.
    A constant alpha is data and stays exactly as it is: the terrain tiles carry 3, and the terrain's base pass
    alpha-tests "> 0" (0 drops the whole layer). Soft alpha is only upscaled; the snapping of near-0 / near-255
    values belongs to hard masks alone."""
    w, h = size
    if a.min() == a.max():
        return np.full((h, w), a.min(), np.uint8)
    up = cv2.resize(a.astype(np.float32), (w, h), interpolation=cv2.INTER_CUBIC)
    if is_binary_mask(a):
        scale = w / a.shape[1]
        # the cubic ramp across an edge spans ~2 source px = 2*scale output px; squeeze it to ~1.5 px
        k = max(1.0, (2.0 * scale) / 1.5)
        up = np.clip((up - 127.5) * k + 127.5, 0, 255)
        up[up < ALPHA_FLOOR] = 0
        up[up > ALPHA_CEIL] = 255
    up = np.clip(up, 0, 255)
    return (up + 0.5).astype(np.uint8)


def alpha_was_damaged(a):
    """True when the old upscale_alpha (near-0 / near-255 snapping on every alpha, constant alpha included) changed
    what this source's alpha upscales to: a non-binary alpha with values below ALPHA_FLOOR or above ALPHA_CEIL."""
    if a.min() == 255 or is_binary_mask(a):
        return False
    return int(a.min()) < ALPHA_FLOOR or int(a.max()) > ALPHA_CEIL


# ---------------------------------------------------------------- normals
def _unpack(n):
    v = n.astype(np.float32) / 127.5 - 1.0
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)


def normal_n3(orig_normal_rgb, diffuse_hd, tileable, sigma=6.0, edge_fade=0):
    """Original normal map's low frequencies + PBRify NormalV3 (from the HD diffuse) high frequencies.
    edge_fade: over this many pixels from the border the result fades to the original (resized) normals, so that
    tiles of a mosaic whose normal maps continue one another keep doing so even where their colours do not."""
    import gan_upscale
    H, W = diffuse_hd.shape[:2]
    n1 = resize(orig_normal_rgb, (W, H))
    n2 = gan_upscale.upscale_rgb(diffuse_hd, model_file('1x-PBRify_NormalV3*'), tileable=tileable)
    v1, v2 = _unpack(n1), _unpack(n2)

    def blur(x):
        if tileable:
            p = int(3 * sigma + 1)
            return cv2.GaussianBlur(np.pad(x, ((p, p), (p, p), (0, 0)), mode='wrap'), (0, 0), sigma)[p:-p, p:-p]
        return cv2.GaussianBlur(x, (0, 0), sigma, borderType=cv2.BORDER_REFLECT)

    v3 = blur(v1) + (v2 - blur(v2))
    if edge_fade > 0:
        d = np.minimum(np.minimum(np.arange(H)[:, None], np.arange(H)[::-1, None]),
                       np.minimum(np.arange(W)[None, :], np.arange(W)[None, ::-1])).astype(np.float32)
        w = np.clip(d / float(edge_fade), 0.0, 1.0)[..., None]
        v3 = v1 * (1.0 - w) + v3 * w
    v3 /= np.maximum(np.linalg.norm(v3, axis=-1, keepdims=True), 1e-6)
    return ((v3 + 1.0) * 127.5).clip(0, 255).astype(np.uint8)


# ---------------------------------------------------------------- encoding
def bc_format(src_fmt, alpha=None):
    """DXT1/3/5 keep their family. Uncompressed sources (RGB32A/RGB24) get BC3 when the alpha carries something,
    else BC1: four times the pixels uncompressed would cost the 32-bit game too much address space."""
    fixed = {'DXT1': 'BC1_UNORM', 'DXT3': 'BC2_UNORM', 'DXT5': 'BC3_UNORM'}
    if src_fmt in fixed:
        return fixed[src_fmt]
    return 'BC3_UNORM' if alpha is not None and alpha.min() < 255 else 'BC1_UNORM'


def encode(rgba, fmt, out_path):
    """Writes a DDS with a full mip chain through texconv; returns the decoded mip 0 for QA."""
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        png = os.path.join(tmp, 'tex.png')
        Image.fromarray(np.ascontiguousarray(rgba), 'RGBA').save(png)
        args = [TEXCONV, '-nologo', '-y', '-f', fmt, '-m', '0', '-sepalpha', '-o', tmp, png]
        r = subprocess.run(args, capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f'texconv failed ({r.returncode}): {r.stdout} {r.stderr}')
        data = open(os.path.join(tmp, 'tex.dds'), 'rb').read()
    with open(out_path, 'wb') as f:
        f.write(data)
    return data


def entry_out_path(out_dir, entry):
    rel = entry.lstrip('.\\/').replace('\\', os.sep)
    return os.path.join(out_dir, os.path.splitext(rel)[0] + '.dds')


def qa(src_rgba, data):
    """Decoded-vs-source check: format, size, mips, PSNR of RGB and A, low-frequency colour drift."""
    info = dds.info(data)
    dec = dds.decode_top(data)
    a = src_rgba.astype(np.float32)
    b = dec.astype(np.float32)

    def psnr(x, y):
        mse = float(((x - y) ** 2).mean())
        return 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)

    return {'fmt': info['fmt'], 'size': [info['width'], info['height']], 'mips': info['mips'],
            'bytes': len(data), 'psnr_rgb': round(psnr(a[..., :3], b[..., :3]), 2),
            'psnr_a': round(psnr(a[..., 3], b[..., 3]), 2),
            'drift_dE': round(colorlock.drift(src_rgba[..., :3], dec[..., :3]), 3)}


def build_texture(big, entry, rgb_hd, out_dir, normal_of=None, tileable=False, alpha_hd=None, normal_fade=0):
    """rgb_hd: final RGB at target size. normal_of: (diffuse_hd) when this entry is a normal map (normal_fade: see
    normal_n3's edge_fade). alpha_hd: an upscaled alpha to use instead of upscale_alpha (effects: their shape lives
    in the alpha); a constant source alpha is data and stays constant whatever is passed."""
    info = source_format(big, entry)
    orig = load_texture(big, entry)
    H, W = rgb_hd.shape[:2]
    if normal_of is not None:
        rgb_hd = normal_n3(orig[..., :3], normal_of, tileable, edge_fade=normal_fade)
        H, W = rgb_hd.shape[:2]
    if info['fmt'] == 'DXT1':
        alpha = np.full((H, W), 255, np.uint8)
    elif alpha_hd is not None and orig[..., 3].min() != orig[..., 3].max():
        alpha = alpha_hd if alpha_hd.shape[:2] == (H, W) else np.asarray(
            Image.fromarray(alpha_hd).resize((W, H), Image.LANCZOS))
    else:
        alpha = upscale_alpha(orig[..., 3], (W, H))
    rgba = np.dstack([rgb_hd, alpha])
    fmt = bc_format(info['fmt'], orig[..., 3])
    path = entry_out_path(out_dir, entry)
    data = encode(rgba, fmt, path)
    rep = qa(rgba, data)
    rep.update({'entry': entry, 'archive': big, 'src': f"{info['fmt']} {info['width']}x{info['height']}",
                'alpha': 'none' if fmt == 'BC1_UNORM' else ('cutout' if is_binary_mask(orig[..., 3]) else 'soft'),
                'file': os.path.relpath(path, out_dir)})
    return rep


def write_report(out_dir, reports, extra=None):
    with open(os.path.join(out_dir, 'pack.json'), 'w', encoding='utf-8') as f:
        json.dump({'textures': reports, **(extra or {})}, f, indent=1, ensure_ascii=False)
