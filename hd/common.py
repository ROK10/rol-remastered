"""Shared helpers for the HD texture pipeline (run with the ComfyUI portable Python: torch + CUDA + spandrel)."""
import os
import sys

import numpy as np
from PIL import Image

HD = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HD)
GAME = os.path.dirname(KIT)
AI = r'X:\AI'
COMFY = os.path.join(AI, 'ComfyUI_windows_portable')
MODELS = os.path.join(COMFY, 'ComfyUI', 'models')
TEXCONV = os.path.join(AI, 'tools', 'texconv.exe')
sys.path.insert(0, os.path.join(KIT, 'tools'))

from bigtool import BigArchive  # noqa: E402
import dds  # noqa: E402

_archives = {}


def load_texture(big, entry):
    """RGBA uint8 array of mip 0 of a texture entry inside BIGS/<big>."""
    if big not in _archives:
        _archives[big] = BigArchive.load(os.path.join(GAME, 'BIGS', big))
    arch = _archives[big]
    e = arch.find(entry)
    if e is None:
        raise KeyError(f'{entry} not in {big}')
    return dds.decode_top(BigArchive.data_of(e))


def save_png(path, arr):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    Image.fromarray(np.ascontiguousarray(arr)).save(path)


def load_png(path, mode=None):
    im = Image.open(path)
    if mode:
        im = im.convert(mode)
    return np.asarray(im)


def resize(arr, size, method=Image.LANCZOS):
    """size = (w, h). Works for RGB(A) and single-channel arrays."""
    return np.asarray(Image.fromarray(arr).resize(size, method))


def wrap_pad(arr, pad):
    """Pad a tileable texture with its own opposite edges so filters see continuous content."""
    return np.pad(arr, ((pad, pad), (pad, pad)) + ((0, 0),) * (arr.ndim - 2), mode='wrap')


def tiled_preview(arr, n=2):
    """n x n repetition of a tileable texture, to eyeball seams."""
    return np.tile(arr, (n, n) + (1,) * (arr.ndim - 2))
