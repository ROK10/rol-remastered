r"""The mod page's before / after pictures for a release, from the autotest runs: one command rebuilds them all.

  py -3.14 modkit/tools/promo.py [--version 1.0]

Each entry of STILLS takes a shot of two autotest cases (the newest run in work/autotest that has both) and makes,
through tools/compare.py, the labelled side-by-side picture (<name>.png / .jpg) and each half on its own with its
caption (<name>_before.jpg, <name>_after.jpg). VIDEOS make GIF + MP4 from two recordings. Everything lands in
work/promo/<version>; singles/ gets the halves and GIFs again, numbered in upload order (01 before, 02 after, ...),
for a gallery that keeps the order of the upload. The runs come from:
  autotest.py promo_before promo_after --lang en
  autotest.py land_desert_before land_desert_after ... land_forest_after --lang en
  autotest.py phoenix_free_before phoenix_free_after --lang en     (the camera on the scout by hand)
Crops keep clear of the interface at 150 % on 3440 x 1440: the bottom-left panel reaches x 830 from y 1085, the
resource bar x 550 along the top.
"""
import argparse
import glob
import os
import shutil

from compare import still, video

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(KIT, 'work', 'autotest')
UI = ('Before · UI 100% at 1440p', 'After · UI 150% at 1440p')
# name, before case, after case, shot, crop (x, y, w, h), captions, align
STILLS = [
    ('land_desert_city', 'land_desert_before', 'land_desert_after', 'shot_1_6s', (500, 60, 1920, 880)),
    ('land_desert_territory', 'land_desert_before', 'land_desert_after', 'shot_2_11s', (840, 330, 1920, 1080)),
    ('land_crater_city', 'land_crater_before', 'land_crater_after', 'shot_1_6s', (760, 0, 1920, 1080)),
    ('land_crater_territory', 'land_crater_before', 'land_crater_after', 'shot_2_11s', (840, 200, 1920, 1080)),
    ('land_snow_city', 'land_snow_before', 'land_snow_after', 'shot_1_6s', (760, 0, 1920, 1080)),
    ('land_snow_territory', 'land_snow_before', 'land_snow_after', 'shot_2_11s', (840, 330, 1920, 1080)),
    ('land_forest_city', 'land_forest_before', 'land_forest_after', 'shot_1_6s', (760, 0, 1920, 1080)),
    ('land_forest_territory', 'land_forest_before', 'land_forest_after', 'shot_2_11s', (560, 0, 1920, 1080)),
    ('shadows_far_left', 'promo_before', 'promo_after', 'shot_3_16s', (80, 130, 1040, 950)),
    ('tree_shadows_far_right', 'promo_before', 'promo_after', 'shot_3_16s', (2560, 380, 880, 720)),
    ('ui_scale_bottom_left', 'promo_before', 'promo_after', 'shot_2_11s', (0, 900, 1000, 540), UI, False),
    ('ui_scale_top_left', 'promo_before', 'promo_after', 'shot_2_11s', (0, 0, 800, 360), UI, False),
    ('ui_scale_top_right', 'promo_before', 'promo_after', 'shot_2_11s', (2540, 0, 900, 260), UI, False),
]
# name, before case, after case, second of the test clock, seconds, crop, width
VIDEOS = [
    ('phoenix_smooth_animation', 'phoenix_free_before', 'phoenix_free_after', 15, 3, (1320, 345, 800, 600), 1006),
]


def newest(before, after, needs):
    """The case folders of the newest run that has both cases with 'needs' in them."""
    for run in sorted(glob.glob(os.path.join(RUNS, '20*')), reverse=True):
        b, a = os.path.join(run, before), os.path.join(run, after)
        if all(os.path.exists(os.path.join(d, needs)) for d in (b, a)):
            return b, a
    raise SystemExit(f'no run in {RUNS} has {before} and {after} with {needs}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--version', default='1.0')
    args = ap.parse_args()
    out = os.path.join(KIT, 'work', 'promo', args.version)
    singles_dir = os.path.join(out, 'singles')
    if os.path.isdir(singles_dir):
        shutil.rmtree(singles_dir)
    os.makedirs(singles_dir)
    n = 0
    for entry in STILLS:
        name, before, after, shot, crop = entry[:5]
        labels = entry[5] if len(entry) > 5 else ('Before', 'After')
        align = entry[6] if len(entry) > 6 else True
        b, a = newest(before, after, shot + '.png')
        for f in still(os.path.join(b, shot + '.png'), os.path.join(a, shot + '.png'), os.path.join(out, name + '.png'),
                       crop, None, labels, align, split=True):
            n += 1
            shutil.copy2(f, os.path.join(singles_dir, f'{n:02d}_{os.path.basename(f)}'))
    for name, before, after, at, seconds, crop, width in VIDEOS:
        b, a = newest(before, after, 'record.mp4')
        gif = os.path.join(out, name + '.gif')
        video(b, a, gif, at, seconds, crop, width)
        n += 1
        shutil.copy2(gif, os.path.join(singles_dir, f'{n:02d}_{name}.gif'))
    print(f'{n} files for the gallery in {singles_dir}, in upload order')


if __name__ == '__main__':
    main()
