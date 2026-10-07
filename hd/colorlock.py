"""Colour lock: keep the original texture's low-frequency colour, take only new fine detail from the HD result.

Super-resolution and diffusion both drift the palette (Real-ESRGAN x4plus on vinci_base_2_diffuse: red -5.5,
chroma -13%). In CIELAB we replace the HD image's blurred colour with the blurred original (upscaled to the same
size), so everything larger than ~sigma keeps the artist's colours while the high-pass detail stays.
"""
import cv2
import numpy as np


def color_lock(hd_rgb, orig_rgb, sigma=None, lock_luma=0.6, tileable=False):
    """hd_rgb: HxWx3 uint8 (target size); orig_rgb: hxwx3 uint8 (source size).
    sigma: blur radius in HD pixels (default ~2 source pixels). lock_luma: 0..1 share of low-frequency
    lightness taken from the original (chroma is always locked fully)."""
    H, W = hd_rgb.shape[:2]
    scale = W / orig_rgb.shape[1]
    sigma = sigma or 2.0 * scale
    up = cv2.resize(orig_rgb, (W, H), interpolation=cv2.INTER_CUBIC)
    pad = int(3 * sigma + 1)

    def blur(x):
        # OpenCV filters have no wrap border: pad tileable textures with their opposite edges by hand
        if tileable:
            xp = np.pad(x, ((pad, pad), (pad, pad), (0, 0)), mode='wrap')
            return cv2.GaussianBlur(xp, (0, 0), sigmaX=sigma, sigmaY=sigma)[pad:-pad, pad:-pad]
        return cv2.GaussianBlur(x, (0, 0), sigmaX=sigma, sigmaY=sigma, borderType=cv2.BORDER_REFLECT)

    hd = cv2.cvtColor(hd_rgb, cv2.COLOR_RGB2LAB).astype(np.float32)
    org = cv2.cvtColor(up, cv2.COLOR_RGB2LAB).astype(np.float32)
    out = hd.copy()
    low_hd, low_org = blur(hd), blur(org)
    out[..., 1:] = hd[..., 1:] - low_hd[..., 1:] + low_org[..., 1:]
    out[..., 0] = hd[..., 0] - lock_luma * (low_hd[..., 0] - low_org[..., 0])
    out = np.clip(out, 0, 255).astype(np.uint8)
    return cv2.cvtColor(out, cv2.COLOR_LAB2RGB)


def drift(a_rgb, b_rgb):
    """Mean colour difference at low frequency (CIE76 delta-E on 8x downsampled LAB) — a QA number per texture."""
    h, w = a_rgb.shape[:2]
    b = cv2.resize(b_rgb, (w, h), interpolation=cv2.INTER_AREA)
    small = (max(1, w // 8), max(1, h // 8))
    la = cv2.cvtColor(cv2.resize(a_rgb, small, interpolation=cv2.INTER_AREA), cv2.COLOR_RGB2LAB).astype(np.float32)
    lb = cv2.cvtColor(cv2.resize(b, small, interpolation=cv2.INTER_AREA), cv2.COLOR_RGB2LAB).astype(np.float32)
    # OpenCV 8-bit LAB: L scaled to 0..255, a/b offset by 128
    la[..., 0] *= 100 / 255
    lb[..., 0] *= 100 / 255
    return float(np.sqrt(((la - lb) ** 2).sum(-1)).mean())
