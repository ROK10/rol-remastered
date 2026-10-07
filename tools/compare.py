r"""Before / after pictures for a mod page: two shots of the same scene side by side in one image, labelled.

  py -3.14 modkit/tools/compare.py <before.png> <after.png> <out.png> [--crop x,y,w,h] [--width N]
                                   [--labels Before,After] [--no-align]
  py -3.14 modkit/tools/compare.py --video <before case dir> <after case dir> <out.gif> --at S --seconds N
                                   [--crop x,y,w,h] [--width N] [--labels Before,After]

--crop     the part of the shot to compare, in the before shot's pixels (default: the whole shot)
--width    the result's width in pixels, scaled down to it (default: the crops' own size, 1:1)
--labels   the two captions (default Before,After)
--split    also each crop on its own, full size, with its caption (<out>_before.jpg / _after.jpg): single pictures for
           a gallery, uploaded in pairs (tools/promo.py numbers them in upload order)
The after shot is lined up with the before one first (phase correlation of the luminance), so a camera a few pixels
off between the two games still gives the same scene in both halves; the offset is printed. Next to out.png comes
out.jpg (quality 92) for sites that limit the upload size.
--video: two autotest cases with 'record' (record.mp4, record.json, core.log in each case folder): the same N seconds
of both, from second S of the test clock, cropped, side by side and labelled, as a GIF at the recording's 50 fps and
as an MP4 (H.264) next to it - for animation, where a still shot shows nothing. The test clock is placed in a
recording from the wall times autotest kept (ffmpeg's start, the game's start) and the match start rolhook logged.
The shots and recordings come from tools/autotest.py cases (work/autotest/...), never from the repository: game
pictures are not committed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

GAP = 6                      # dark line between the halves
FONTS = [r'C:\Windows\Fonts\segoeuib.ttf', r'C:\Windows\Fonts\arialbd.ttf']


def offset(before, after):
    """(dx, dy) to add to a point of the before shot to find the same scene point in the after shot."""
    a = np.asarray(after.convert('L'), dtype=np.float64)
    b = np.asarray(before.convert('L'), dtype=np.float64)
    f = np.fft.fft2(a) * np.conj(np.fft.fft2(b))
    f /= np.abs(f) + 1e-9
    r = np.fft.ifft2(f).real
    y, x = np.unravel_index(np.argmax(r), r.shape)
    h, w = a.shape
    return (x - w if x > w // 2 else x), (y - h if y > h // 2 else y)


def label(img, text):
    d = ImageDraw.Draw(img, 'RGBA')
    size = max(22, img.height // 18)
    font = next((ImageFont.truetype(p, size) for p in FONTS if os.path.isfile(p)), ImageFont.load_default())
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    pad = size // 3
    x0, y0 = size // 2, size // 2
    d.rounded_rectangle((x0, y0, x0 + (r - l) + 2 * pad, y0 + (b - t) + 2 * pad), radius=pad, fill=(0, 0, 0, 150))
    d.text((x0 + pad - l, y0 + pad - t), text, font=font, fill=(255, 255, 255, 255))


def side_by_side(halves, labels):
    for im, text in zip(halves, labels):
        label(im, text.strip())
    out = Image.new('RGB', (2 * halves[0].width + GAP, halves[0].height), (16, 16, 16))
    out.paste(halves[0], (0, 0))
    out.paste(halves[1], (halves[0].width + GAP, 0))
    return out


DLL_DELAY = 0.1   # s from the game's start to rolhook's clock (it loads with the exe as ddraw.dll)


def record_time(case_dir, second):
    """The time in case_dir/record.mp4 of a second of the case's test clock."""
    with open(os.path.join(case_dir, 'record.json'), encoding='utf-8') as f:
        rec = json.load(f)
    with open(os.path.join(case_dir, 'core.log'), encoding='utf-8', errors='replace') as f:
        m = re.search(r'match on screen \(first terrain draw\) at ([\d.]+) s', f.read())
    if not m:
        raise SystemExit(f'{case_dir}: core.log has no match start')
    return rec['game_start'] - rec['ffmpeg_start'] + DLL_DELAY + float(m.group(1)) + second, rec.get('fps', 50)


def frames(case_dir, second, seconds, crop, width, tmp):
    """The cropped frames of N seconds of a recording, as PIL images."""
    t, fps = record_time(case_dir, second)
    vf = []
    if crop:
        x, y, w, h = crop
        vf.append(f'crop={w}:{h}:{x}:{y}')
    if width:
        vf.append(f'scale={width}:-2:flags=lanczos')
    out = os.path.join(tmp, os.path.basename(os.path.normpath(case_dir)))
    os.makedirs(out)
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-ss', f'{t:.3f}', '-i',
                    os.path.join(case_dir, 'record.mp4'), '-t', f'{seconds:.3f}', '-fps_mode', 'passthrough']
                   + (['-vf', ','.join(vf)] if vf else []) + [os.path.join(out, '%05d.png')], check=True)
    names = sorted(os.listdir(out))
    return [Image.open(os.path.join(out, n)).convert('RGB') for n in names], fps


def video(before_dir, after_dir, out, at, seconds, crop=None, width=None, labels=('Before', 'After')):
    """Two autotest recordings -> out (GIF) and out's .mp4, side by side and labelled."""
    half = (width - GAP) // 2 if width else None
    with tempfile.TemporaryDirectory() as tmp:
        fb, fps = frames(before_dir, at, seconds, crop, half, tmp)
        fa, _ = frames(after_dir, at, seconds, crop, half, tmp)
        n = min(len(fb), len(fa))
        if not n:
            raise SystemExit('no frames in that part of the recordings')
        comp = os.path.join(tmp, 'side')
        os.makedirs(comp)
        for i in range(n):
            side_by_side([fb[i], fa[i]], labels).save(os.path.join(comp, f'{i:05d}.png'))
        seq = os.path.join(comp, '%05d.png')
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-framerate', str(fps), '-i', seq,
                        '-vf', 'split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:'
                               'bayer_scale=4:diff_mode=rectangle', out], check=True)
        mp4 = os.path.splitext(out)[0] + '.mp4'
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-framerate', str(fps), '-i', seq,
                        '-c:v', 'libx264', '-crf', '16', '-preset', 'slow', '-pix_fmt', 'yuv420p', mp4], check=True)
        size = Image.open(os.path.join(comp, '00000.png')).size
    print(f'{out}: {size[0]}x{size[1]}, {n} frames at {fps} fps ({n / fps:.1f} s), '
          f'{os.path.getsize(out) / 1e6:.1f} MB; mp4 {os.path.getsize(mp4) / 1e6:.1f} MB')


def still(before_path, after_path, out, crop=None, width=None, labels=('Before', 'After'), align=True, split=False):
    """Two shots -> out (PNG) and out's .jpg side by side, labelled; with split also each crop on its own, full size and
    labelled (<out>_before.jpg, <out>_after.jpg). Returns the single files written."""
    before, after = Image.open(before_path).convert('RGB'), Image.open(after_path).convert('RGB')
    if before.size != after.size:
        raise SystemExit(f'the shots differ in size: {before.size} and {after.size}')
    x, y, w, h = crop if crop else (0, 0, *before.size)
    dx, dy = offset(before, after) if align else (0, 0)
    if not (0 <= x + dx and x + dx + w <= after.width and 0 <= y + dy and y + dy + h <= after.height):
        raise SystemExit(f'the crop moved by the alignment ({dx}, {dy}) leaves the after shot')
    halves = [before.crop((x, y, x + w, y + h)), after.crop((x + dx, y + dy, x + dx + w, y + dy + h))]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    singles = []
    if split:   # each crop on its own, with its caption: a gallery of single pictures, or a site's own slider
        stem = os.path.splitext(out)[0]
        for im, part, text in zip(halves, ('before', 'after'), labels):
            single = im.copy()
            label(single, text.strip())
            single.save(f'{stem}_{part}.jpg', quality=92)
            singles.append(f'{stem}_{part}.jpg')
    if width:
        hw = (width - GAP) // 2
        halves = [im.resize((hw, round(h * hw / w)), Image.LANCZOS) for im in halves]
    pair = side_by_side(halves, labels)
    pair.save(out)
    jpg = os.path.splitext(out)[0] + '.jpg'
    pair.save(jpg, quality=92)
    print(f'{out}: {pair.width}x{pair.height}, after shot offset ({dx}, {dy}); {os.path.getsize(jpg) // 1024} KB jpg')
    return singles


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('before')
    ap.add_argument('after')
    ap.add_argument('out')
    ap.add_argument('--crop', help='x,y,w,h in the before shot')
    ap.add_argument('--width', type=int, help='scale the result down to this width')
    ap.add_argument('--labels', default='Before,After')
    ap.add_argument('--no-align', action='store_true', help='crop both shots at the same pixels')
    ap.add_argument('--split', action='store_true',
                    help='also write each crop on its own, labelled (<out>_before.jpg, <out>_after.jpg)')
    ap.add_argument('--video', action='store_true', help='before and after are autotest case folders with recordings')
    ap.add_argument('--at', type=float, default=5.0, help='--video: from this second of the test clock')
    ap.add_argument('--seconds', type=float, default=3.0, help='--video: this long')
    args = ap.parse_args()
    crop = tuple(map(int, args.crop.split(','))) if args.crop else None
    labels = tuple(args.labels.split(','))
    if args.video:
        if not shutil.which('ffmpeg'):
            raise SystemExit('ffmpeg is not on PATH')
        return video(args.before, args.after, args.out, args.at, args.seconds, crop, args.width, labels)
    still(args.before, args.after, args.out, crop, args.width, labels, not args.no_align, args.split)


if __name__ == '__main__':
    main()
