"""GAN / transformer super-resolution through spandrel (ESRGAN, DAT2, HAT, SPAN... auto-detected).

Tiled inference with overlap and feathered blending, so big inputs fit in VRAM. Tileable textures are wrap-padded
before inference and cropped after, which keeps their edges continuous.
"""
import functools

import numpy as np
import torch
from spandrel import ModelLoader


@functools.lru_cache(maxsize=4)
def load_model(path):
    model = ModelLoader().load_from_file(path)
    model.cuda().eval()
    half = model.supports_half
    if half:
        model.half()
    return model, half


def release():
    """Free the cached models and CUDA memory (before handing the GPU to ComfyUI)."""
    load_model.cache_clear()
    torch.cuda.empty_cache()


def _run(model, half, tile):
    t = torch.from_numpy(tile).permute(2, 0, 1).unsqueeze(0).float().div(255.0).cuda()
    if half:
        t = t.half()
    with torch.inference_mode():
        out = model(t)
    return out.squeeze(0).float().clamp(0, 1).permute(1, 2, 0).cpu().numpy()


def upscale_rgb(rgb, model_path, tile=384, overlap=48, tileable=False):
    """rgb: HxWx3 uint8 -> (H*s)x(W*s)x3 uint8."""
    model, half = load_model(model_path)
    pad = overlap if tileable else 0
    src = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode='wrap') if pad else rgb
    h, w = src.shape[:2]
    scale = model.scale
    out = np.zeros((h * scale, w * scale, 3), np.float32)
    weight = np.zeros((h * scale, w * scale, 1), np.float32)
    step = tile - overlap
    ys = list(range(0, max(h - overlap, 1), step)) or [0]
    xs = list(range(0, max(w - overlap, 1), step)) or [0]
    for y in ys:
        for x in xs:
            y0, x0 = min(y, max(h - tile, 0)), min(x, max(w - tile, 0))
            y1, x1 = min(y0 + tile, h), min(x0 + tile, w)
            res = _run(model, half, src[y0:y1, x0:x1])
            th, tw = res.shape[:2]
            ramp_y = np.minimum(np.arange(th) + 1, np.arange(th)[::-1] + 1).astype(np.float32)
            ramp_x = np.minimum(np.arange(tw) + 1, np.arange(tw)[::-1] + 1).astype(np.float32)
            feather = np.minimum(np.minimum(ramp_y[:, None], ramp_x[None, :]), overlap * scale)[..., None]
            out[y0 * scale:y0 * scale + th, x0 * scale:x0 * scale + tw] += res * feather
            weight[y0 * scale:y0 * scale + th, x0 * scale:x0 * scale + tw] += feather
    out /= np.maximum(weight, 1e-6)
    if pad:
        p = pad * scale
        out = out[p:-p, p:-p]
    return (out * 255.0 + 0.5).astype(np.uint8)
