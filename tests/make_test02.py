"""Test 02: HD texture override through rolhook (no archive changes).

Writes modkit/mods/test02/<entry path>.dds:
  vinci_base_2_diffuse        draft x4 AI upscale (Real-ESRGAN x4plus) 512 -> 2048, full mip chain, A8R8G8B8
  alim_base_diffuse,
  cuotl_base_texture_diffuse  2048 probes: white grid on mip 0, each lower mip tinted (shows which mip is drawn)
The textures channel of rolhook confirms each replacement ("HD ... gpu 2048x2048 L12").
"""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
GAME = os.path.dirname(KIT)
sys.path.insert(0, os.path.join(KIT, 'tools'))
sys.path.insert(0, HERE)
from bigtool import BigArchive  # noqa: E402
import dds  # noqa: E402
from make_test01 import find_entry, grid, mip_chain, resize, tint, MIP_TINTS  # noqa: E402

ESRGAN = r'X:\VN_upscaler\runners\realesrgan-ncnn\realesrgan-ncnn-vulkan.exe'
OUT = os.path.join(KIT, 'mods', 'test02')
PREVIEW = os.path.join(KIT, 'build', 'test02')

UPSCALE = [('vinci_buildings.big', '.\\art\\buildings\\vinc\\textures\\vinci_base_2_diffuse.tga')]
PROBES = [('alim_buildings.big', '.\\art\\buildings\\alim\\textures\\alim_base_diffuse.tga'),
          ('cuotl_buildings.big', '.\\art\\buildings\\cuot\\textures\\cuotl_base_texture_diffuse.tga')]


def load(big, name):
    arch = BigArchive.load(os.path.join(GAME, 'BIGS', big))
    e = find_entry(arch, name)
    data = BigArchive.data_of(e)
    return e['name'], dds.info(data), dds.decode_top(data)


def out_path(entry_name):
    rel = entry_name.lstrip('.\\/').replace('\\', os.sep)
    return os.path.join(OUT, os.path.splitext(rel)[0] + '.dds')


def esrgan_x4(rgb):
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = os.path.join(tmp, 'in.png'), os.path.join(tmp, 'out.png')
        Image.fromarray(rgb).save(src)
        subprocess.run([ESRGAN, '-i', src, '-o', dst, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png'],
                       cwd=os.path.dirname(ESRGAN), check=True, capture_output=True)
        return np.asarray(Image.open(dst).convert('RGB'))


def write(entry_name, levels):
    p = out_path(entry_name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(dds.write_argb8888(levels))
    print(f'  {os.path.relpath(p, KIT)}: {levels[0].shape[1]}x{levels[0].shape[0]}, {len(levels)} levels')


def main():
    os.makedirs(PREVIEW, exist_ok=True)
    for big, name in UPSCALE:
        ename, meta, top = load(big, name)
        h, w = top.shape[:2]
        rgb = esrgan_x4(top[..., :3])
        alpha = np.asarray(Image.fromarray(top[..., 3]).resize((w * 4, h * 4), Image.LANCZOS))
        up = np.dstack([rgb, alpha]).astype(np.uint8)
        write(ename, mip_chain(up))
        stem = os.path.splitext(os.path.basename(ename))[0]
        crop = (slice(0, h // 2), slice(0, w // 2))           # top-left quarter: riveted plates
        before = Image.fromarray(top[crop[0], crop[1], :3]).resize((1024, 1024), Image.NEAREST)
        after = Image.fromarray(up[crop[0].start * 4:crop[0].stop * 4, crop[1].start * 4:crop[1].stop * 4, :3]).resize((1024, 1024), Image.LANCZOS)
        side = Image.new('RGB', (2048, 1024))
        side.paste(before, (0, 0))
        side.paste(after, (1024, 0))
        side.save(os.path.join(PREVIEW, f'{stem}_before_after.png'))
    for big, name in PROBES:
        ename, meta, top = load(big, name)
        h, w = top.shape[:2]
        levels = mip_chain(resize(top, (w * 4, h * 4)))
        levels = [grid(levels[0])] + [tint(lv, MIP_TINTS[i] if i < len(MIP_TINTS) else (255, 255, 255), 0.55)
                                      for i, lv in enumerate(levels) if i > 0]
        write(ename, levels)


if __name__ == '__main__':
    main()
