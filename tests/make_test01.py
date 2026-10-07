"""Test 01: does the engine (a) load an extra .big from BIGS, (b) which copy wins on a name clash,
(c) accept 2048x2048 instead of 512x512, (d) use mip levels stored in the DDS?

Builds two archives with the same three entries (the main city wall texture of each nation):
  00_rolmodtest.big : whole texture tinted ORANGE (all mips)            -> visible if "first loaded wins"
  zz_rolmodtest.big : 2048x2048, mip0 has a WHITE GRID every 128 texels,
                      each lower mip tinted its own colour               -> visible if "last loaded wins"
Mip colours (zz): 1 red, 2 green, 3 blue, 4 yellow, 5 magenta, 6 cyan, 7+ white.
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
GAME = os.path.dirname(KIT)
sys.path.insert(0, os.path.join(KIT, 'tools'))
from bigtool import BigArchive  # noqa: E402
import dds  # noqa: E402

TARGETS = [
    ('vinci_buildings.big', '.\\art\\buildings\\vinc\\textures\\vinci_base_2_diffuse.tga'),
    ('alim_buildings.big', '.\\art\\buildings\\alim\\textures\\alim_base_diffuse.tga'),
    ('cuotl_buildings.big', '.\\art\\buildings\\cuot\\textures\\cuotl_base_texture_diffuse.tga'),
]
MIP_TINTS = [None, (255, 0, 0), (0, 255, 0), (0, 64, 255), (255, 255, 0), (255, 0, 255), (0, 255, 255)]
ORANGE = (255, 128, 0)
OUT = os.path.join(KIT, 'build', 'test01')


def resize(rgba, size):
    """Resize RGB and alpha independently (alpha may be a mask, not coverage)."""
    rgb = Image.fromarray(rgba[..., :3]).resize(size, Image.LANCZOS)
    a = Image.fromarray(rgba[..., 3]).resize(size, Image.LANCZOS)
    out = np.empty((size[1], size[0], 4), np.uint8)
    out[..., :3] = np.asarray(rgb)
    out[..., 3] = np.asarray(a)
    return out


def mip_chain(top):
    levels = [top]
    while max(levels[-1].shape[:2]) > 1:
        h, w = levels[-1].shape[:2]
        levels.append(resize(levels[-1], (max(1, w // 2), max(1, h // 2))))
    return levels


def tint(level, color, amount):
    out = level.copy()
    out[..., :3] = (level[..., :3].astype(np.float32) * (1 - amount) + np.array(color, np.float32) * amount).astype(np.uint8)
    return out


def grid(level, step=128, width=3):
    out = level.copy()
    for k in range(0, level.shape[0], step):
        out[k:k + width, :, :3] = 255
    for k in range(0, level.shape[1], step):
        out[:, k:k + width, :3] = 255
    return out


def find_entry(arch, name):
    e = arch.find(name)
    if e is None:
        cands = [x['name'] for x in arch.entries if x['name'].lower().endswith(name.lower().split('\\')[-1])]
        raise KeyError(f'{name} not found; candidates: {cands}')
    return e


def main():
    os.makedirs(OUT, exist_ok=True)
    early, late = BigArchive(), BigArchive()
    for big, name in TARGETS:
        src = BigArchive.load(os.path.join(GAME, 'BIGS', big))
        e = find_entry(src, name)
        data = BigArchive.data_of(e)
        meta = dds.info(data)
        top = dds.decode_top(data)
        a = top[..., 3]
        print(f"{e['name']}: {meta['fmt']} {meta['width']}x{meta['height']} alpha min/max/mean "
              f"{a.min()}/{a.max()}/{a.mean():.1f}")
        up = resize(top, (meta['width'] * 4, meta['height'] * 4))
        levels = mip_chain(up)
        late_levels = [grid(levels[0])] + [
            tint(lv, MIP_TINTS[i] if i < len(MIP_TINTS) else (255, 255, 255), 0.55) for i, lv in enumerate(levels) if i > 0]
        early_levels = [tint(lv, ORANGE, 0.65) for lv in levels]
        late.set_data(e['name'], dds.write_argb8888(late_levels), ts=e['ts'])
        early.set_data(e['name'], dds.write_argb8888(early_levels), ts=e['ts'])
        stem = os.path.splitext(os.path.basename(e['name']))[0]
        Image.fromarray(late_levels[0]).resize((1024, 1024)).save(os.path.join(OUT, f'{stem}_zz_mip0.png'))
        strip = np.concatenate([np.asarray(Image.fromarray(lv).resize((128, 128), Image.NEAREST)) for lv in late_levels[1:8]], axis=1)
        Image.fromarray(strip).save(os.path.join(OUT, f'{stem}_zz_mips1-7.png'))
        Image.fromarray(early_levels[0]).resize((512, 512)).save(os.path.join(OUT, f'{stem}_00_mip0.png'))
        Image.fromarray(top).save(os.path.join(OUT, f'{stem}_orig.png'))
    for arch, fname in ((early, '00_rolmodtest.big'), (late, 'zz_rolmodtest.big')):
        size = arch.save(os.path.join(OUT, fname))
        print(f'{fname}: {len(arch.entries)} entries, {size / 2**20:.1f} MiB')


if __name__ == '__main__':
    main()
