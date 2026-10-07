"""Battle 01 — which chain makes the best HD textures for Rise of Legends?

Chains (all x4):  A Real-ESRGAN x4plus (baseline) | B PBRify UpscalerV4 | C B + Flux refine (light)
                  | D B + Flux refine (strong).  Every chain also gets a colour-locked variant (*_cl).
Writes work/battle01/out/<id>/<chain>.png, 2x2 tiled previews for terrain, crops and index.json.
Run with the ComfyUI portable Python (torch + CUDA + spandrel + opencv).
"""
import glob
import json
import os
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import KIT, MODELS, load_png, save_png, resize, tiled_preview  # noqa: E402
from battle01_set import SET, NORMALS  # noqa: E402
import colorlock  # noqa: E402
import comfy_client  # noqa: E402
import gan_upscale  # noqa: E402

W = os.path.join(KIT, 'work', 'battle01')
SRC, OUT = os.path.join(W, 'src'), os.path.join(W, 'out')
ESRGAN_EXE = r'X:\VN_upscaler\runners\realesrgan-ncnn\realesrgan-ncnn-vulkan.exe'
FLUX = {'C_flux_light': dict(denoise=0.30, strength=0.65), 'D_flux_strong': dict(denoise=0.45, strength=0.55)}


def model_file(pattern):
    hits = sorted(glob.glob(os.path.join(MODELS, 'upscale_models', pattern)))
    if not hits:
        raise SystemExit(f'model not found: {pattern} in {MODELS}\\upscale_models')
    return hits[0]


def esrgan(rgb):
    with tempfile.TemporaryDirectory() as tmp:
        a, b = os.path.join(tmp, 'a.png'), os.path.join(tmp, 'b.png')
        Image.fromarray(rgb).save(a)
        subprocess.run([ESRGAN_EXE, '-i', a, '-o', b, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png'],
                       cwd=os.path.dirname(ESRGAN_EXE), check=True, capture_output=True)
        return np.asarray(Image.open(b).convert('RGB'))


def flux(rgb_hd, rgb_src, prompt, tileable, tag, **kw):
    """Flux refine at >= 1024 px: small images are refined at 2x and brought back down."""
    h, w = rgb_hd.shape[:2]
    work = rgb_hd if min(h, w) >= 768 else resize(rgb_hd, (w * 2, h * 2))
    ctrl = resize(rgb_src, (work.shape[1], work.shape[0]), Image.BICUBIC)
    tile = 1408 if max(work.shape[:2]) <= 1280 else 1152
    res = comfy_client.refine(work, ctrl, prompt, tile=tile, overlap=256, tileable=tileable, tag=tag, **kw)
    return res if res.shape[:2] == (h, w) else resize(res, (w, h))


def crop_box(tid, size):
    """A 1/4-side crop with a lot of detail (chosen per texture by eye on the contact sheet)."""
    w, h = size
    picks = {'vinci_wall': (0.0, 0.5), 'alim_wall': (0.0, 0.5), 'cuotl_wall': (0.0, 0.5),
             'steamflyer': (0.0, 0.0), 'musketeer': (0.25, 0.0)}
    fx, fy = picks.get(tid, (0.375, 0.375))
    cw, ch = w // 4, h // 4
    return int(fx * w), int(fy * h), cw, ch


def main():
    os.makedirs(OUT, exist_ok=True)
    pbrify = model_file('4x-PBRify*UpscalerV4*')
    print('PBRify model:', os.path.basename(pbrify), flush=True)
    srcs = {tid: np.ascontiguousarray(load_png(os.path.join(SRC, tid + '.png'), 'RGBA')[..., :3])
            for tid, *_ in SET}
    res = {tid: {} for tid in srcs}
    secs = {tid: {} for tid in srcs}

    # phase 1: super-resolution (GPU used by this process only)
    for tid, big, entry, kind, prompt in SET:
        rgb, tileable = srcs[tid], kind == 'tile'
        t = time.time(); res[tid]['A_esrgan'] = esrgan(rgb); secs[tid]['A_esrgan'] = time.time() - t
        t = time.time(); res[tid]['B_pbrify'] = gan_upscale.upscale_rgb(rgb, pbrify, tileable=tileable)
        secs[tid]['B_pbrify'] = time.time() - t
        print(f'{tid:11s} SR done: esrgan {secs[tid]["A_esrgan"]:.1f}s pbrify {secs[tid]["B_pbrify"]:.1f}s', flush=True)
    gan_upscale.release()

    # phase 2: Flux refine through ComfyUI (it gets the whole GPU)
    proc = comfy_client.start(os.path.join(W, 'comfyui.log'))
    try:
        for tid, big, entry, kind, prompt in SET:
            for name, kw in FLUX.items():
                t = time.time()
                res[tid][name] = flux(res[tid]['B_pbrify'], srcs[tid], prompt, kind == 'tile', f'{tid}_{name}', **kw)
                secs[tid][name] = time.time() - t
                print(f'{tid:11s} {name} {secs[tid][name]:.1f}s', flush=True)
    finally:
        if proc:
            proc.terminate()
            proc.wait(timeout=60)

    # phase 3: colour lock, previews, crops, metrics
    index = {}
    for tid, big, entry, kind, prompt in SET:
        rgb, tileable = srcs[tid], kind == 'tile'
        d = os.path.join(OUT, tid)
        os.makedirs(d, exist_ok=True)
        for name in list(res[tid]):
            res[tid][name + '_cl'] = colorlock.color_lock(res[tid][name], rgb, tileable=tileable)
        entry_info = {'kind': kind, 'entry': entry, 'src_size': [rgb.shape[1], rgb.shape[0]], 'chains': {}}
        for name, img in res[tid].items():
            save_png(os.path.join(d, name + '.png'), img)
            if tileable:
                save_png(os.path.join(d, name + '_tiled.png'), resize(tiled_preview(img), (1024, 1024)))
            x, y, cw, ch = crop_box(tid, (img.shape[1], img.shape[0]))
            save_png(os.path.join(d, name + '_crop.png'), img[y:y + ch, x:x + cw])
            entry_info['chains'][name] = {'drift_dE': round(colorlock.drift(rgb, img), 2),
                                          'seconds': round(secs[tid].get(name.replace('_cl', ''), 0.0), 1)}
        hd_size = (res[tid]['B_pbrify'].shape[1], res[tid]['B_pbrify'].shape[0])
        x, y, cw, ch = crop_box(tid, hd_size)
        near = resize(rgb, hd_size, Image.NEAREST)
        save_png(os.path.join(d, 'orig_crop.png'), near[y:y + ch, x:x + cw])
        if tileable:
            save_png(os.path.join(d, 'orig_tiled.png'), resize(tiled_preview(near), (1024, 1024)))
        index[tid] = entry_info
        print(f'{tid:11s} drift dE: ' + ' '.join(f"{k}={v['drift_dE']}" for k, v in entry_info['chains'].items()),
              flush=True)
    with open(os.path.join(W, 'index.json'), 'w') as f:
        json.dump(index, f, indent=1)

    # phase 4: normal maps
    normals(index)


def normals(index):
    """Normal maps: N1 original Lanczos x4, N2 PBRify Normal from the best HD diffuse, N3 N1 low + N2 high."""
    try:
        nmodel = model_file('1x-PBRify_NormalV3*')
    except SystemExit as e:
        print(e)
        return
    import cv2
    for nid, big, entry, did in NORMALS:
        src = load_png(os.path.join(SRC, nid + '.png'), 'RGBA')[..., :3]
        diff = load_png(os.path.join(OUT, did, 'C_flux_light_cl.png'), 'RGB')
        H, W_ = diff.shape[:2]
        n1 = resize(src, (W_, H))
        n2 = gan_upscale.upscale_rgb(diff, nmodel, tileable=(did in ('grass',)))

        def unpack(n):
            v = n.astype(np.float32) / 127.5 - 1.0
            return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)

        v1, v2 = unpack(n1), unpack(n2)
        sigma = 6.0
        low1 = cv2.GaussianBlur(v1, (0, 0), sigma)
        high2 = v2 - cv2.GaussianBlur(v2, (0, 0), sigma)
        v3 = low1 + high2
        v3 /= np.maximum(np.linalg.norm(v3, axis=-1, keepdims=True), 1e-6)
        n3 = ((v3 + 1.0) * 127.5).clip(0, 255).astype(np.uint8)
        d = os.path.join(OUT, nid)
        for name, img in (('N1_lanczos', n1), ('N2_pbrify', n2), ('N3_blend', n3)):
            save_png(os.path.join(d, name + '.png'), img)
            v = unpack(img)
            light = np.array([0.5, 0.6, 0.62], np.float32)
            shade = (np.clip((v * light).sum(-1), 0, 1) * 255).astype(np.uint8)
            save_png(os.path.join(d, name + '_lit.png'), shade)
        index.setdefault('_normals', {})[nid] = ['N1_lanczos', 'N2_pbrify', 'N3_blend']
        print(f'{nid}: normal variants written', flush=True)
    with open(os.path.join(W, 'index.json'), 'w') as f:
        json.dump(index, f, indent=1)


if __name__ == '__main__':
    main()
