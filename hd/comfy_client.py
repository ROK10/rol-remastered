"""Minimal client for the portable ComfyUI: start the server headless, queue API-format workflows, collect images.

Flux refine = img2img over a super-resolved image, steered by the jasperai Flux.1-dev ControlNet Upscaler
(a tile-style ControlNet). Large images are processed in overlapping tiles and feather-blended.
"""
import json
import os
import shutil
import subprocess
import time
import urllib.request
import uuid

import numpy as np
from PIL import Image

from common import COMFY

URL = 'http://127.0.0.1:8188'
INPUT = os.path.join(COMFY, 'ComfyUI', 'input')
OUTPUT = os.path.join(COMFY, 'ComfyUI', 'output')
CHECKPOINT = 'flux1-dev-fp8.safetensors'
CONTROLNET = 'flux1-dev-controlnet-upscaler-jasperai.safetensors'


def alive():
    try:
        urllib.request.urlopen(URL + '/system_stats', timeout=2)
        return True
    except Exception:
        return False


def start(log_path):
    """Start ComfyUI unless it already runs; returns the Popen (or None when reused)."""
    if alive():
        return None
    py = os.path.join(COMFY, 'python_embeded', 'python.exe')
    log = open(log_path, 'w', encoding='utf-8', errors='replace')
    # no console window of its own (the HD pass may run detached, without a console to share)
    proc = subprocess.Popen([py, '-s', os.path.join('ComfyUI', 'main.py'), '--windows-standalone-build',
                             '--listen', '127.0.0.1', '--port', '8188', '--disable-auto-launch'],
                            cwd=COMFY, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                            creationflags=0x08000000)   # CREATE_NO_WINDOW
    for _ in range(300):
        if alive():
            return proc
        if proc.poll() is not None:
            raise RuntimeError(f'ComfyUI exited with {proc.returncode}, see {log_path}')
        time.sleep(1)
    raise RuntimeError('ComfyUI did not come up in 5 minutes')


def run(workflow, timeout=3600):
    data = json.dumps({'prompt': workflow, 'client_id': str(uuid.uuid4())}).encode()
    req = urllib.request.Request(URL + '/prompt', data=data, headers={'Content-Type': 'application/json'})
    try:
        pid = json.load(urllib.request.urlopen(req))['prompt_id']
    except urllib.error.HTTPError as e:
        raise RuntimeError(e.read().decode(errors='replace')[:3000])
    t0 = time.time()
    while time.time() - t0 < timeout:
        hist = json.load(urllib.request.urlopen(f'{URL}/history/{pid}'))
        if pid in hist:
            entry = hist[pid]
            if entry.get('status', {}).get('status_str') == 'error':
                raise RuntimeError(json.dumps(entry['status'])[:3000])
            files = [os.path.join(OUTPUT, im.get('subfolder', ''), im['filename'])
                     for node in entry.get('outputs', {}).values() for im in node.get('images', [])]
            if files:
                return files
        time.sleep(0.5)
    raise TimeoutError(pid)


def _put(arr, name):
    os.makedirs(INPUT, exist_ok=True)
    Image.fromarray(arr).save(os.path.join(INPUT, name))
    return name


def refine_workflow(init_name, ctrl_name, prompt, denoise, strength, steps, guidance, seed, prefix):
    return {
        '1': {'class_type': 'CheckpointLoaderSimple', 'inputs': {'ckpt_name': CHECKPOINT}},
        '2': {'class_type': 'CLIPTextEncode', 'inputs': {'text': prompt, 'clip': ['1', 1]}},
        '3': {'class_type': 'FluxGuidance', 'inputs': {'conditioning': ['2', 0], 'guidance': guidance}},
        '4': {'class_type': 'CLIPTextEncode', 'inputs': {'text': '', 'clip': ['1', 1]}},
        '5': {'class_type': 'LoadImage', 'inputs': {'image': init_name}},
        '6': {'class_type': 'LoadImage', 'inputs': {'image': ctrl_name}},
        '7': {'class_type': 'ControlNetLoader', 'inputs': {'control_net_name': CONTROLNET}},
        '8': {'class_type': 'ControlNetApplyAdvanced',
              'inputs': {'positive': ['3', 0], 'negative': ['4', 0], 'control_net': ['7', 0], 'image': ['6', 0],
                         'strength': strength, 'start_percent': 0.0, 'end_percent': 1.0, 'vae': ['1', 2]}},
        '9': {'class_type': 'VAEEncode', 'inputs': {'pixels': ['5', 0], 'vae': ['1', 2]}},
        '10': {'class_type': 'KSampler',
               'inputs': {'model': ['1', 0], 'seed': seed, 'steps': steps, 'cfg': 1.0, 'sampler_name': 'euler',
                          'scheduler': 'simple', 'positive': ['8', 0], 'negative': ['8', 1],
                          'latent_image': ['9', 0], 'denoise': denoise}},
        '11': {'class_type': 'VAEDecode', 'inputs': {'samples': ['10', 0], 'vae': ['1', 2]}},
        '12': {'class_type': 'SaveImage', 'inputs': {'images': ['11', 0], 'filename_prefix': prefix}},
    }


def refine(init_rgb, ctrl_rgb, prompt, denoise=0.35, strength=0.6, steps=24, guidance=3.0, seed=7,
           tile=1152, overlap=256, tileable=False, tag='rol'):
    """Flux refine of an HxW RGB image (init) steered by a same-size control image. Returns uint8 RGB."""
    pad = overlap // 2 if tileable else 0
    if pad:
        init_rgb = np.pad(init_rgb, ((pad, pad), (pad, pad), (0, 0)), mode='wrap')
        ctrl_rgb = np.pad(ctrl_rgb, ((pad, pad), (pad, pad), (0, 0)), mode='wrap')
    h, w = init_rgb.shape[:2]
    out = np.zeros((h, w, 3), np.float32)
    weight = np.zeros((h, w, 1), np.float32)
    step = tile - overlap
    ys = [0] if h <= tile else sorted(set(list(range(0, h - tile, step)) + [h - tile]))
    xs = [0] if w <= tile else sorted(set(list(range(0, w - tile, step)) + [w - tile]))
    k = 0
    for y in ys:
        for x in xs:
            th, tw = min(tile, h), min(tile, w)
            # Flux works on multiples of 16 px
            th16, tw16 = th - th % 16, tw - tw % 16
            a = init_rgb[y:y + th16, x:x + tw16]
            c = ctrl_rgb[y:y + th16, x:x + tw16]
            name = f'{tag}_{uuid.uuid4().hex[:8]}'
            files = run(refine_workflow(_put(a, name + '_i.png'), _put(c, name + '_c.png'), prompt, denoise,
                                        strength, steps, guidance, seed + k, f'rol/{name}'))
            res = np.asarray(Image.open(files[0]).convert('RGB')).astype(np.float32)
            for f in files:
                os.remove(f)
            for f in (name + '_i.png', name + '_c.png'):
                os.remove(os.path.join(INPUT, f))
            ry = np.minimum(np.arange(th16) + 1, np.arange(th16)[::-1] + 1).astype(np.float32)
            rx = np.minimum(np.arange(tw16) + 1, np.arange(tw16)[::-1] + 1).astype(np.float32)
            feather = np.minimum(np.minimum(ry[:, None], rx[None, :]), max(overlap // 2, 1))[..., None]
            out[y:y + th16, x:x + tw16] += res * feather
            weight[y:y + th16, x:x + tw16] += feather
            k += 1
    out = out / np.maximum(weight, 1e-6)
    out = (out + 0.5).clip(0, 255).astype(np.uint8)
    if pad:
        out = out[pad:-pad, pad:-pad]
    return out
