"""Creates the files smoke.exe expects next to it: rolhook.ini (from smoke.ini) and the HD override sample."""
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(KIT, 'build', 'native')
sys.path.insert(0, os.path.join(KIT, 'tools'))
import dds  # noqa: E402

os.makedirs(OUT, exist_ok=True)
shutil.copyfile(os.path.join(HERE, 'smoke.ini'), os.path.join(OUT, 'rolhook.ini'))

levels = []
size = 512
while size >= 1:
    lv = np.zeros((size, size, 4), np.uint8)
    lv[..., 0] = 255 * (len(levels) % 2)       # alternate red / green per mip level
    lv[..., 1] = 255 * (1 - len(levels) % 2)
    lv[..., 3] = 255
    levels.append(lv)
    size //= 2
target = os.path.join(OUT, 'override_smoke', 'art', 'terrain', 'minimap_modifier_background.dds')
os.makedirs(os.path.dirname(target), exist_ok=True)
with open(target, 'wb') as f:
    f.write(dds.write_argb8888(levels))
print(f'wrote {target} ({len(levels)} levels)')
