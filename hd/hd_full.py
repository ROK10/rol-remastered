r"""Full HD pass over the game's world textures, split into phases that can be run, stopped and resumed.

Phases (in default order) and chains (user's rule after battle 01, see memory feedback-texture-philosophy):
  terrain    terrain tiles, cliffs                   B: PBRify V4 x4                                   fast
  buildings  building maps                           D: PBRify V4 x4 + Flux.1-dev refine for diffuse    hours
  units      unit maps                               D for *_hi / single diffuse >= 128 px, B for the rest
  trees      tree and plant maps                     B                                                  fast
  prefabs    landscape prefabs (rocks, ruins)        B                                                  fast
  effects    particles, decals, spell circles        E: Real-ESRGAN x4plus on colour and alpha for line art;
                                                     O: soft particles and opaque ones keep the original   fast
  campaign   the campaign map (art\ctw\dome_tiles)    S: PBRify SPAN Neutral x4 with the neighbour tiles as
                                                     context (seamless); region borders E; masks untouched  fast
Inside a phase: diffuse maps by their chain; normal maps -> N3 (original's low frequencies + PBRify NormalV3 of
the chosen diffuse; unpaired ones -> Lanczos); team colour and other masks -> Lanczos, alpha-aware. Every RGB
result is colour-locked to the original; alpha is upscaled on its own (pack.py; chain E through Real-ESRGAN too).
The review page (review_page.py) can send Flux textures back to B or C ('hd fallback'); masks never go to Flux.

Target sizes (integer multiples by on-screen importance; originals are at most 512, a few 1024):
  terrain x4 (max 2048) | buildings x4 (max 2048) | units x4 up to 1024 | trees x2 (max 1024) | prefabs x4 (max 2048)
  | effects x4 up to 1024 | campaign map colour x4 (max 4096), its normal maps and borders x2 (max 2048)
Not touched: UI, cursors, credits, campaign map sea and region masks, sky, storyboards.

Each finished texture goes to modkit/mods/hd/<entry>.dds at once (atomic replace), so the pack grows while a phase
runs and a stopped run loses at most the texture in progress. State lives in work/hd_full: jobs.json (the plan),
done.json, failed.json, run.log, sr/ and flux/ (cached intermediate images), RUNNING (lock with the PIDs).

  hd plan                      rebuild the job list, show phases
  hd status                    progress per phase, failures, remaining-time estimate, running job
  hd start [phase ...]         the same as run, detached in the background without a window (survives closing
                               windows, VS Code, the Claude session); output in work/hd_full/console.log
  hd run [phase ...] [--limit N] [--ids a,b]   run / resume the given phases (default: all, in order); --limit:
                               only the next N textures of each phase; --ids: only these jobs (redo after reset)
  hd stop                      stop after the current texture (also cleans up a ComfyUI left by a killed run)
  hd reset <phase> [--cache]   forget a phase and remove its textures from the pack (--cache: also its sr/flux)
  hd fallback B:id C:id O:id   choices from the review pages: B = PBRify only, C = light Flux, O = keep the original,
                               E = Real-ESRGAN line art (effects only); kept in overrides.json, redone (with their
                               normal maps) by the next 'hd run'
  hd fix-alpha [phase ...] [--dry]   re-encode finished textures whose alpha the old upscale damaged (constant
                               terrain alpha 3 became 0), from the cached results
  hd applied [session]         does the game show the pack: the textures loaded in the sessions since the pack last
                               changed (or since the given logs/sessions folder) against the plan, per phase
'hd' is hd.bat next to this script (the ComfyUI portable Python).
"""
import collections
import ctypes
import json
import os
import re
import subprocess
import sys
import time
import traceback

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import GAME, KIT, BigArchive, dds, load_texture, resize  # noqa: E402
import campaign  # noqa: E402
import colorlock  # noqa: E402
import pack  # noqa: E402

W = os.path.join(KIT, 'work', 'hd_full')
SR_DIR, FLUX_DIR, TMP_DIR = os.path.join(W, 'sr'), os.path.join(W, 'flux'), os.path.join(W, 'tmp')
OUT = os.path.join(KIT, 'mods', 'hd')
JOBS, DONE, FAILED = os.path.join(W, 'jobs.json'), os.path.join(W, 'done.json'), os.path.join(W, 'failed.json')
OVERRIDES = os.path.join(W, 'overrides.json')   # {id: 'B' | 'C'} chosen on the review page, beats the plan
STOP, LOCK, LOG = os.path.join(W, 'STOP'), os.path.join(W, 'RUNNING'), os.path.join(W, 'run.log')
BS = '\\'

PHASES = ['terrain', 'buildings', 'units', 'trees', 'prefabs', 'effects', 'campaign']
KIND = {'terrain': 'terrain', 'buildings': 'building', 'units': 'unit', 'trees': 'tree', 'prefabs': 'prefab',
        'effects': 'effect', 'campaign': 'campaign'}
# The campaign map is a satellite view: PBRify V4 (chain B) reads it as a close-up and turns forests into grass blades;
# SPAN Neutral keeps tree crowns and rock as seen from above, Real-ESRGAN x4plus smooths it to bicubic (comparison
# work/ctw_compare, 2026-10-06). Chain S = SPAN Neutral, colour-locked like B; tiles go through it framed by their
# neighbours (campaign.padded) so the made-up detail continues across the tile edges. CONTEXT texels of frame.
SPAN_NEUTRAL = '4x-PBRify_UpscalerSPAN_Neutral*'
CONTEXT = 48
# Effects (art\effects: particles, decals, spell circles, explosion frames) carry their shape in the alpha. Line art
# (selection circles, emblems, icons, debris: a mask with thin anti-aliased edges) goes through Real-ESRGAN x4plus on
# colour and alpha alike, colour-locked like B: chain E. Soft particles (smoke, glows, rays, flares, fire: fades over
# many pixels) stay original - chain O: every upscaler hardens those fades into cut-out edges (the 2026-10-04 pass
# turned smoke puffs into rocks and light rays into trapezoids), and a fade gains nothing from more pixels. Opaque
# effect textures (gradients, noise, flat colours) stay original too. The line between them is the share of the
# covered area (alpha > 0.05) that is in between (0.1 .. 0.9): line art <= EFFECT_LINE_ART_MID < soft (fx_kind
# sheets, 2026-10-06). The review of the E results (2026-10-06) sent four back to O in overrides.json, cases the
# alpha rule cannot see: blurry 64 px clumps (whirl_pebbles, _2: the model carved a pattern into them), specks a
# texel or two wide (sparks_2: they shrank to 40 % of their area) and noise under a hard-edged disc (shock_ring_02:
# smoothed into clouds). It sent eleven splats the other way, O -> E, so that a family looks alike on screen: their
# edges are only blurrier than their siblings', not soft (semi-transparent share 0.33 .. 0.49) - the selection
# circles of Alim and Cuotl units (data\underlays.xml: every unit of the nation; Vinci's went E by the rule), the
# Cuotl giant-unit circle, the CTW evil hero rings of levels 2-5 (level 1 went E), the hero centre splats of Cuotl
# and CTW, vinci_hero_04.
ESRGAN_EXE = r'X:\VN_upscaler\runners\realesrgan-ncnn\realesrgan-ncnn-vulkan.exe'
EFFECT_LINE_ART_MID = 0.33


def effect_chain(rgba):
    """'E' for effect line art, 'O' (keep the original) for soft or opaque effect textures."""
    a = rgba[..., 3].astype(np.float32) / 255
    if a.max() - a.min() < 0.02:
        return 'O'
    covered = float((a > 0.05).mean())
    mid = float(((a > 0.1) & (a < 0.9)).mean()) / max(covered, 1e-4)
    return 'E' if mid <= EFFECT_LINE_ART_MID else 'O'
FLUX_D = dict(denoise=0.45, strength=0.55)
FLUX_C = dict(denoise=0.30, strength=0.65)   # the light variant of battle 01, for textures D over-invented
FLUX = {'D': FLUX_D, 'C': FLUX_C}
ALBEDO = 'highly detailed, sharp, flat even lighting, no shadows, albedo texture'


# ---------------------------------------------------------------- plan
def split(entry):
    return entry.lstrip('.' + BS).lower().split(BS)


def classify(entry):
    """(kind, map) or None. kind: terrain | building | unit | tree | prefab | effect | campaign."""
    p = split(entry)
    name = os.path.splitext(p[-1])[0]
    kind = None
    if 'nonship' in p:
        return None   # textures that did not ship (art\trees\*\nonship)
    if p[:3] == ['art', 'ctw', 'dome_tiles']:
        c = campaign.parse(entry)
        return ('campaign', c[0]) if c else None   # diffuse | normal | border; sea, masks, grey map: untouched
    if p[0] == 'art' and len(p) > 2:
        kind = {'buildings': 'building', 'units': 'unit'}.get(p[1])
        if p[1] == 'terrain' and p[2] == 'tilesets':
            kind = 'terrain'   # art\terrain\minimap_modifier_* are UI art addressed by pixel rects: never scale
        if p[1] == 'trees' and 'shadow' not in p[2]:
            kind = 'tree'
        if p[1] == 'effects':
            kind = 'effect'
    elif p[0] in ('landscapes', 'prefabs'):
        kind = 'prefab'
    if kind is None:
        return None
    if re.search(r'(_|^)(dot3|normal|nrm|bump)(_|$)', name):
        m = 'normal'
    elif re.search(r'(_|^)(team|teamcolor|tc)(_|$)', name):
        m = 'team'
    elif kind == 'effect':
        # in art\effects 'glow', 'light', 'shadow', 'alpha' name the effect's picture (glow sprites, light rays,
        # a smoke puff with a hard alpha): only refraction and reflection maps are data there
        m = 'mask' if re.search(r'(_|^)(distortion|env|environment|skybox|mask|msk)(_|$)', name) else 'diffuse'
    elif re.search(r'(_|^)(mask|msk|gloss|glow|spec|specular|env|environment|skybox|reflect|alpha|light|illum|shadow|'
                   r'distortion)(_|$)', name):
        m = 'mask'   # data, not a picture: never through Flux (vinci_roof_metal_msk got Flux squiggles before 'msk')
    else:
        m = 'diffuse'
    return kind, m


def target_size(kind, w, h, m='diffuse'):
    if kind == 'campaign':
        mult, cap = (4, 4096) if m == 'diffuse' else (2, 2048)
    else:
        mult, cap = {'terrain': (4, 2048), 'building': (4, 2048), 'unit': (4, 1024), 'tree': (2, 1024),
                     'prefab': (4, 2048), 'effect': (4, 1024)}[kind]
    s = mult
    while s > 1 and max(w, h) * s > cap:
        s //= 2
    return w * s, h * s


def diffuse_pair(entry, by_name):
    """The diffuse map a normal map belongs to: vinci_base_normal -> vinci_base_diffuse, grass_dot3 -> grass,
    quarry_rock_dot3 -> quarry_rock_diff, timonium_dot3_02 -> timonium_diffuse."""
    p = entry.lower()
    token = r'_(dot3|normal|nrm|bump)'
    for pattern in (token + r'(?=(_|\.))', token + r'_\d+(?=\.)'):
        for repl in ('_diffuse', '_diff', ''):
            c = re.sub(pattern, repl, p)
            if c != p and c in by_name:
                return by_name[c]
    return None


def collect():
    """All world textures of the main archives (an entry present in several archives is taken once)."""
    seen, jobs = set(), []
    for big in sorted(f for f in os.listdir(os.path.join(GAME, 'BIGS')) if f.lower().endswith('.big')):
        arch = BigArchive.load(os.path.join(GAME, 'BIGS', big))
        for e in arch.entries:
            name = e['name']
            c = classify(name) if name.lower().endswith(('.tga', '.dds')) else None
            if not c or name.lower() in seen:
                continue
            data = BigArchive.data_of(e)
            if data[:4] != b'DDS ':
                continue
            info = dds.info(data)
            if min(info['width'], info['height']) < 32:
                continue   # tiny ramps / lookup tables: nothing to gain
            seen.add(name.lower())
            kind, m = c
            jobs.append(dict(id=re.sub(r'[^a-z0-9_]+', '_', name.lower().lstrip('.' + BS)), archive=big, entry=name,
                             kind=kind, map=m, fmt=info['fmt'], src=[info['width'], info['height']],
                             dst=list(target_size(kind, info['width'], info['height'], m))))
            if kind == 'effect' and m == 'diffuse':
                jobs[-1]['fx'] = effect_chain(dds.decode_top(data))
    by_name = {j['entry'].lower(): j for j in jobs if j['map'] == 'diffuse'}
    for j in jobs:
        if j['map'] == 'diffuse':
            # Flux only where drawn detail shows: building/unit maps of 128 px and more that are not the low-LOD
            # twin of a *_hi map (those are drawn far away); everything else gets PBRify alone
            low_twin = re.sub(r'(\.\w+)$', r'_hi\1', j['entry'].lower()) in by_name
            patterned = j['kind'] in ('building', 'unit') and max(j['src']) >= 128 and not low_twin
            j['chain'] = j.pop('fx') if j['kind'] == 'effect' else ('S' if j['kind'] == 'campaign' else
                                                                    ('D' if patterned else 'B'))
        elif j['map'] == 'border':
            j['chain'] = 'E'
        elif j['map'] == 'normal':
            if j['kind'] == 'campaign':
                kind_, q, i, jj, v = campaign.parse(j['entry'])
                d = by_name.get(campaign.entry_of('diffuse', q, i, jj).lower())
            else:
                d = diffuse_pair(j['entry'], by_name)
            j['chain'] = 'N3' if d else 'L'
            if d:
                j['pair'] = d['id']
        else:
            j['chain'] = 'L'
        j['phase'] = next(p for p, k in KIND.items() if k == j['kind'])
    by_id = {j['id']: j for j in jobs}
    for j in jobs:   # a normal map stays original with its diffuse (effects kept as they are)
        if j['chain'] == 'N3' and by_id[j['pair']]['chain'] == 'O':
            j['chain'] = 'O'
            del j['pair']
    jobs.sort(key=lambda j: (PHASES.index(j['phase']), j['entry'].lower()))
    return jobs


def work_size(job):
    """Flux works at the final size (detail at the scale it is seen), at least ~1024 px: small targets are
    refined at 2x and brought down."""
    w, h = job['dst']
    while min(w, h) < 768:
        w, h = w * 2, h * 2
    return w, h


def estimate_secs(job):
    s = 1.5                                                         # colour lock, alpha, texconv
    if job['map'] == 'diffuse':
        s += 6.0 * job['src'][0] * job['src'][1] / (512 * 512) + 1.0   # PBRify x4
    if job['chain'] in FLUX:
        ww, wh = work_size(job)                                     # Flux: ~145 s at 2048^2, ~40 s at 1024^2
        s += 40.0 * max(1.0, ww * wh / (1024 * 1024)) ** 0.93
    if job['chain'] == 'N3':
        s += 2.0
    if job['chain'] == 'E':
        s += 3.0                                                    # Real-ESRGAN on colour and alpha
    return s


# ---------------------------------------------------------------- state
def load_json(path, default):
    try:
        with open(path, encoding='utf-8') as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, obj):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(obj, f, indent=0, ensure_ascii=False)
    os.replace(tmp, path)


def log(msg):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    print(line, flush=True)
    with open(LOG, 'a', encoding='utf-8') as f:
        f.write(line + '\n')


def pid_alive(pid):
    if not pid:
        return False
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
    ctypes.windll.kernel32.CloseHandle(h)
    return code.value == 259   # STILL_ACTIVE


def kill_pid(pid):
    subprocess.run(['taskkill', '/PID', str(pid), '/T', '/F'], capture_output=True)


class Run:
    """Lock + stop flag + the ComfyUI child of the current run."""

    def __init__(self, phases):
        lock = load_json(LOCK, {})
        if pid_alive(lock.get('pid')):
            raise SystemExit(f"a run is already active (pid {lock['pid']}, phases {', '.join(lock.get('phases', []))});"
                             f" 'hd stop' stops it after the current texture")
        if pid_alive(lock.get('comfy_pid')):
            kill_pid(lock['comfy_pid'])   # left behind by a killed run
        if os.path.exists(STOP):
            os.remove(STOP)
        self.lock = {'pid': os.getpid(), 'comfy_pid': None, 'phases': phases, 'started': time.strftime('%Y-%m-%d %H:%M:%S')}
        save_json(LOCK, self.lock)

    def comfy(self, proc):
        self.lock['comfy_pid'] = proc.pid if proc else None
        save_json(LOCK, self.lock)

    @staticmethod
    def stopping():
        return os.path.exists(STOP)

    def close(self):
        if os.path.exists(LOCK):
            os.remove(LOCK)


# ---------------------------------------------------------------- texture steps
def source(job):
    return load_texture(job['archive'], job['entry'])


def tileable(job):
    return job['kind'] == 'terrain' and 'cliff' not in job['entry'].lower()


def sr_path(job):
    return os.path.join(SR_DIR, job['id'] + ('_S' if job['chain'] == 'S' else '') + '.png')


def sr_e_path(job):
    return os.path.join(SR_DIR, job['id'] + '_E.png')   # RGBA: Real-ESRGAN colour and alpha


def esrgan_x4(rgb):
    """Real-ESRGAN x4plus through the ncnn-vulkan runner (its own process, no torch)."""
    import tempfile
    if not os.path.exists(ESRGAN_EXE):
        raise RuntimeError(f'Real-ESRGAN runner not found: {ESRGAN_EXE}')
    os.makedirs(TMP_DIR, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=TMP_DIR) as tmp:
        a, b = os.path.join(tmp, 'a.png'), os.path.join(tmp, 'b.png')
        Image.fromarray(np.ascontiguousarray(rgb)).save(a)
        subprocess.run([ESRGAN_EXE, '-i', a, '-o', b, '-n', 'realesrgan-x4plus', '-s', '4', '-f', 'png'],
                       cwd=os.path.dirname(ESRGAN_EXE), check=True, capture_output=True)
        return np.asarray(Image.open(b).convert('RGB'))


def esrgan_rgba(src):
    """Effect texture x4: colour and alpha through Real-ESRGAN (alpha as a grey picture); a constant alpha is data
    and stays constant; specks of near-0 alpha the model leaves in empty areas are cleared."""
    rgb = esrgan_x4(src[..., :3])
    a = src[..., 3]
    h, w = rgb.shape[:2]
    if a.min() == a.max():
        up = np.full((h, w), a.min(), np.uint8)
    else:
        up = esrgan_x4(np.repeat(a[..., None], 3, axis=2)).mean(axis=2)
        if a.min() == 0:
            up[up < 2] = 0
        up = np.clip(up + 0.5, 0, 255).astype(np.uint8)
    return np.dstack([rgb, up])


def flux_path(job):
    return os.path.join(FLUX_DIR, job['id'] + ('_C' if job['chain'] == 'C' else '') + '.png')


STYLE = {   # by nation, from the folder (Flux follows the image through the ControlNet; the words steer the detail)
    'vinci': 'Renaissance clockwork steampunk in the spirit of Leonardo da Vinci, brass, riveted iron, painted wood, '
             'plaster, stone',
    'alim': 'Arabian desert fantasy, sandstone, glazed tiles, brass, carved geometric ornaments, glass and sand magic',
    'cuotl': 'Aztec Mesoamerican temple style, carved stone glyphs, gold, jade, glowing alien technology',
    'neutral': 'fantasy medieval and renaissance town, stone, timber, plaster, cloth, iron',
}


def nation(job):
    p = split(job['entry'])
    folder = p[2] if len(p) > 2 else ''
    for key, names in (('vinci', ('vinc', 'vinci')), ('alim', ('alim', 'arabian')), ('cuotl', ('cuot', 'cuotl'))):
        if folder in names:
            return key
    return 'neutral'


def prompt_for(job):
    name = os.path.splitext(split(job['entry'])[-1])[0].replace('_', ' ')
    what = {'building': 'painted texture atlas of a building', 'unit': 'painted texture atlas of a unit, armor, '
            'cloth, leather, metal'}[job['kind']]
    return f'{what}, {STYLE[nation(job)]}, {name}, {ALBEDO}'


def final_diffuse(d):
    """The colour-locked diffuse (B, C, D or E) at its target size: what the pack gets and normal maps derive from."""
    orig = source(d)[..., :3]
    if d['chain'] == 'E':
        hd = np.ascontiguousarray(np.asarray(Image.open(sr_e_path(d)).convert('RGBA'))[..., :3])
    else:
        hd = np.asarray(Image.open(flux_path(d) if d['chain'] in FLUX else sr_path(d)).convert('RGB'))
    hd = colorlock.color_lock(hd, orig, tileable=tileable(d))
    return resize(hd, tuple(d['dst'])) if (hd.shape[1], hd.shape[0]) != tuple(d['dst']) else hd


def final_alpha(d):
    """E chain: the Real-ESRGAN alpha at the target size (None for the other chains: pack.upscale_alpha)."""
    if d['chain'] != 'E':
        return None
    a = np.ascontiguousarray(np.asarray(Image.open(sr_e_path(d)).convert('RGBA'))[..., 3])
    tw, th = d['dst']
    return a if (a.shape[1], a.shape[0]) == (tw, th) else np.asarray(Image.fromarray(a).resize((tw, th), Image.LANCZOS))


def write_dds(job, rgb=None, normal_of=None, tileable_=False, alpha=None):
    """RGB (or a normal map built from 'normal_of') at the job's target size -> DDS in the pack, atomically."""
    tw, th = job['dst']
    if rgb is not None and (rgb.shape[1], rgb.shape[0]) != (tw, th):
        rgb = resize(rgb, (tw, th))
    if normal_of is not None and (normal_of.shape[1], normal_of.shape[0]) != (tw, th):
        normal_of = resize(normal_of, (tw, th))
    # campaign normal maps continue one another even where the colour tiles do not (the far south): the detail made
    # from the colour fades out towards the tile's border (1/64 of its side), the original relief carries the edge
    fade = max(1, round(tw / 64)) if job['kind'] == 'campaign' and normal_of is not None else 0
    rep = pack.build_texture(job['archive'], job['entry'], rgb if rgb is not None else normal_of, TMP_DIR,
                             normal_of=normal_of, tileable=tileable_, alpha_hd=alpha, normal_fade=fade)
    final = pack.entry_out_path(OUT, job['entry'])
    os.makedirs(os.path.dirname(final), exist_ok=True)
    os.replace(pack.entry_out_path(TMP_DIR, job['entry']), final)
    return rep


def forget_changed(jobs, done):
    """A texture whose chain changed in a new plan (e.g. a normal map now paired: L -> N3) counts as not done."""
    for j in jobs:
        rec = done.get(j['id'])
        if rec and rec.get('chain') != j['chain']:
            del done[j['id']]


class Phase:
    def __init__(self, name, jobs, done, failed, run, limit=None, ids=None):
        self.name, self.run = name, run
        self.jobs = [j for j in jobs if j['phase'] == name and (not ids or j['id'] in ids)]
        if limit is not None:   # a sample: the first N textures of the phase that are not done yet
            self.jobs = [j for j in self.jobs if j['id'] not in done][:limit]
        self.by_id = {j['id']: j for j in jobs}
        self.done, self.failed = done, failed
        self.count = 0

    def budget_left(self):
        return True

    def finish(self, j, rep, t, step):
        self.done[j['id']] = {'phase': self.name, 'chain': j['chain'], 'dst': j['dst'], 'fmt': rep.get('fmt'),
                              'psnr': rep.get('psnr_rgb'), 'sec': round(time.time() - t, 1)}
        self.failed.pop(j['id'], None)
        save_json(DONE, self.done)
        self.count += 1
        left = sum(1 for x in self.jobs if x['id'] not in self.done)
        log(f"{self.name}: {step} {j['chain']:2s} {j['id']} -> {j['dst'][0]}x{j['dst'][1]} {rep.get('fmt')} "
            f"{time.time() - t:.1f}s ({left} left in phase)")

    def fail(self, j, e, step):
        self.failed[j['id']] = {'phase': self.name, 'step': step, 'error': repr(e)}
        save_json(FAILED, self.failed)
        log(f"{self.name}: FAIL {step} {j['id']}: {e!r}")
        traceback.print_exc()

    def framed(self, j):
        """(RGBA source, frame): campaign tiles come framed by CONTEXT texels of their neighbours, the rest as they
        are. Upscaled x4, the result is cut back by 4 * frame."""
        if j['kind'] != 'campaign':
            return np.ascontiguousarray(source(j)), 0
        entries = [x['entry'] for x in self.by_id.values() if x['kind'] == 'campaign']
        img, used = campaign.padded(j['entry'], CONTEXT, entries)
        return np.ascontiguousarray(img), CONTEXT

    def step_sr(self):
        """PBRify for every diffuse of the phase (cached); B/S diffuse, masks and B/S-paired normals written at once."""
        import gan_upscale
        pbrify = pack.model_file('4x-PBRify*UpscalerV4*')
        for j in self.jobs:
            if self.run.stopping():
                return False
            if not self.budget_left():
                break
            if j['chain'] == 'E' and not os.path.exists(sr_e_path(j)):
                try:
                    t = time.time()
                    src, f = self.framed(j)
                    up = esrgan_rgba(src)
                    if f:
                        up = up[4 * f:up.shape[0] - 4 * f, 4 * f:up.shape[1] - 4 * f]
                    Image.fromarray(np.ascontiguousarray(up), 'RGBA').save(sr_e_path(j))
                    log(f"{self.name}: esrgan {j['id']} {j['src'][0]}x{j['src'][1]} x4 {time.time() - t:.1f}s")
                except Exception as e:
                    self.fail(j, e, 'esrgan')
                    continue
            elif j['map'] == 'diffuse' and j['chain'] not in ('E', 'O') and not os.path.exists(sr_path(j)):
                try:
                    t = time.time()
                    src, f = self.framed(j)
                    model = pack.model_file(SPAN_NEUTRAL) if j['chain'] == 'S' else pbrify
                    up = gan_upscale.upscale_rgb(np.ascontiguousarray(src[..., :3]), model, tileable=tileable(j))
                    if f:
                        up = up[4 * f:up.shape[0] - 4 * f, 4 * f:up.shape[1] - 4 * f]
                    Image.fromarray(np.ascontiguousarray(up)).save(sr_path(j))
                    log(f"{self.name}: sr {j['chain']} {j['id']} {j['src'][0]}x{j['src'][1]} x4 {time.time() - t:.1f}s")
                except Exception as e:
                    self.fail(j, e, 'sr')
                    continue
            if j['id'] in self.done:
                continue
            try:
                t = time.time()
                if j['chain'] in ('B', 'S'):
                    self.finish(j, write_dds(j, final_diffuse(j), tileable_=tileable(j)), t, j['chain'])
                elif j['chain'] == 'E':
                    self.finish(j, write_dds(j, final_diffuse(j), alpha=final_alpha(j)), t, 'E')
                elif j['chain'] == 'O':   # the game's own texture: nothing in the pack (an older HD one is removed)
                    final = pack.entry_out_path(OUT, j['entry'])
                    if os.path.exists(final):
                        os.remove(final)
                    self.finish(j, {'fmt': 'original'}, t, 'O')
                elif j['chain'] == 'L':
                    orig = np.ascontiguousarray(source(j)[..., :3])
                    self.finish(j, write_dds(j, resize(orig, tuple(j['dst']))), t, 'L')
                elif j['chain'] == 'N3' and self.by_id[j['pair']]['chain'] in ('B', 'E', 'S') and j['pair'] in self.done:
                    d = self.by_id[j['pair']]
                    self.finish(j, write_dds(j, normal_of=final_diffuse(d), tileable_=tileable(d)), t, 'N3')
            except Exception as e:
                self.fail(j, e, j['chain'])
        gan_upscale.release()
        return True

    def step_flux(self):
        """Flux refine of the D (and review-chosen C) diffuse maps through ComfyUI (the only GPU user meanwhile)."""
        todo = [j for j in self.jobs if j['chain'] in FLUX and j['id'] not in self.done and os.path.exists(sr_path(j))]
        if not todo or not self.budget_left():
            return True
        import comfy_client
        # renders already in the cache (e.g. 'run --flux-cache', then chosen on a comparison page) need no ComfyUI
        need = any(not os.path.exists(flux_path(j)) for j in todo)
        proc = comfy_client.start(os.path.join(W, 'comfyui.log')) if need else None
        self.run.comfy(proc)
        try:
            for j in todo:
                if self.run.stopping():
                    return False
                if not self.budget_left():
                    break
                try:
                    t = time.time()
                    if not os.path.exists(flux_path(j)):
                        ww, wh = work_size(j)
                        base = np.asarray(Image.open(sr_path(j)).convert('RGB'))
                        work = resize(base, (ww, wh)) if (base.shape[1], base.shape[0]) != (ww, wh) else base
                        ctrl = resize(np.ascontiguousarray(source(j)[..., :3]), (ww, wh), Image.BICUBIC)
                        res = comfy_client.refine(work, ctrl, prompt_for(j), tile=1408 if max(ww, wh) <= 1280 else 1152,
                                                  overlap=256, tileable=False, tag=j['id'], **FLUX[j['chain']])
                        Image.fromarray(res).save(flux_path(j))
                    self.finish(j, write_dds(j, final_diffuse(j)), t, j['chain'])
                except Exception as e:
                    self.fail(j, e, j['chain'])
        finally:
            if proc:
                proc.terminate()
                try:
                    proc.wait(timeout=60)
                except subprocess.TimeoutExpired:
                    kill_pid(proc.pid)
            self.run.comfy(None)
        return True

    def step_n3(self):
        """Normal maps whose diffuse is done (D pairs after Flux, and any B pair the first step missed)."""
        import gan_upscale
        for j in self.jobs:
            if self.run.stopping():
                return False
            if not self.budget_left():
                break
            if j['chain'] != 'N3' or j['id'] in self.done or j['pair'] not in self.done:
                continue
            try:
                t = time.time()
                d = self.by_id[j['pair']]
                self.finish(j, write_dds(j, normal_of=final_diffuse(d), tileable_=tileable(d)), t, 'N3')
            except Exception as e:
                self.fail(j, e, 'N3')
        gan_upscale.release()
        return True

    def execute(self):
        left = [j for j in self.jobs if j['id'] not in self.done]
        if not left:
            log(f'{self.name}: already complete ({len(self.jobs)} textures)')
        else:
            log(f"{self.name}: start, {len(left)} of {len(self.jobs)} textures left, "
                f"~{sum(estimate_secs(j) for j in left) / 3600:.1f} h estimated")
            for step in (self.step_sr, self.step_flux, self.step_n3):
                if not step():
                    return False
            left = sum(1 for j in self.jobs if j['id'] not in self.done)
            log(f"{self.name}: {'complete' if not left else f'{left} textures not done (see hd status)'}")
        if self.name == 'campaign':
            self.seam_report()   # also on a run of a complete phase: 'hd run campaign' re-checks the pack
        return True

    def seam_report(self):
        """The campaign map's tiles must continue one another in the pack as they do in the game's files: the step
        across every shared edge, per map type, into run.log (campaign.seams)."""
        import functools
        entries = [x['entry'] for x in self.by_id.values() if x['kind'] == 'campaign']
        decode = functools.lru_cache(maxsize=6)(lambda p: dds.decode_top(open(p, 'rb').read()))
        for kind in ('diffuse', 'border', 'normal'):
            rows, bad = campaign.seams(entries, lambda e: pack.entry_out_path(OUT, e), decode, kind)
            if not rows:
                continue
            hd_steps, orig_steps = sorted(r[3] for r in rows), sorted(r[4] for r in rows)
            log(f"campaign: {kind} edges {len(rows)}, step across them median {hd_steps[len(rows) // 2]:.1f} "
                f"(original {orig_steps[len(rows) // 2]:.1f}), max {hd_steps[-1]:.1f}; seams: {len(bad)}"
                + ''.join(f"\n    {os.path.basename(a)} {side} {os.path.basename(b)}: {h:.1f} (original {o:.1f})"
                          for a, b, side, h, o in bad))


# ---------------------------------------------------------------- commands
def load_jobs():
    jobs = load_json(JOBS, None)
    if jobs is None:
        jobs = collect()
        save_json(JOBS, jobs)
    over = load_json(OVERRIDES, {})
    for j in jobs:
        if j['id'] in over and j['map'] == 'diffuse':
            j['chain'] = over[j['id']]
    by_id = {j['id']: j for j in jobs}
    for j in jobs:   # a normal map stays original with a diffuse chosen to stay original
        if j['chain'] == 'N3' and by_id[j['pair']]['chain'] == 'O':
            j['chain'] = 'O'
    return jobs


def cmd_fallback(args):
    """'hd fallback B:id1,id2 C:id3 O:id4 E:id5' (the text the review page builds): B = PBRify only, C = light Flux,
    O = keep the game's original, E = Real-ESRGAN line art (effect textures the alpha rule kept original although
    their siblings went to E). The choice is kept in overrides.json; those textures and the normal maps made from
    them are redone by 'hd run'."""
    if pid_alive(load_json(LOCK, {}).get('pid')):
        raise SystemExit("a run is active: 'hd stop' first (or wait for it to finish)")
    text = ' '.join(args)
    pick = {}
    for part in re.split(r'\s+(?=[BCOE]:)', text.strip()):
        m = re.match(r'([BCOE]):\s*(.+)', part.strip())
        if not m:
            raise SystemExit(f'cannot read {part!r}; expected "B: id,id", "C: id,id", "O: id,id" and/or "E: id,id"')
        for i in re.split(r'[,\s]+', m.group(2).strip()):
            if i:
                pick[i] = m.group(1)
    jobs = load_jobs()
    kind = {j['id']: j['kind'] for j in jobs}
    unknown = [i for i in pick if i not in kind]
    if unknown:
        raise SystemExit(f"unknown ids: {', '.join(unknown[:5])}")
    not_fx = [i for i, c in pick.items() if c == 'E' and kind[i] != 'effect']
    if not_fx:
        raise SystemExit(f"E is for effect textures only: {', '.join(not_fx[:5])}")
    over, done = load_json(OVERRIDES, {}), load_json(DONE, {})
    over.update(pick)
    redo = set(pick) | {j['id'] for j in jobs if j.get('pair') in pick}
    for i in redo:
        done.pop(i, None)
    save_json(OVERRIDES, over)
    save_json(DONE, done)
    phases = sorted({j['phase'] for j in jobs if j['id'] in redo}, key=PHASES.index)
    log(f"fallback: {sum(v == 'B' for v in pick.values())} to B, {sum(v == 'C' for v in pick.values())} to C, "
        f"{sum(v == 'E' for v in pick.values())} to E, "
        f"{sum(v == 'O' for v in pick.values())} kept original; {len(redo)} textures to redo with "
        f"'hd run {' '.join(phases)}'")


def cmd_plan():
    save_json(JOBS, collect())
    cmd_status()   # through load_jobs: with the review choices (overrides.json) applied, as a run sees them


def cmd_status(jobs=None):
    jobs = jobs or load_jobs()
    done, failed = load_json(DONE, {}), load_json(FAILED, {})
    forget_changed(jobs, done)   # display only: shows what the next run will redo
    print(f"{'phase':10s} {'done':>11s} {'failed':>6s} {'left, h':>8s}  chains")
    total_left = 0.0
    for p in PHASES:
        pj = [j for j in jobs if j['phase'] == p]
        d = sum(1 for j in pj if j['id'] in done)
        f = sum(1 for j in pj if j['id'] in failed)
        left = sum(estimate_secs(j) for j in pj if j['id'] not in done) / 3600
        total_left += left
        chains = collections.Counter(j['chain'] for j in pj)
        print(f"{p:10s} {d:5d}/{len(pj):<5d} {f:6d} {left:8.1f}  " + ' '.join(f'{k} {v}' for k, v in sorted(chains.items())))
    size = sum(os.path.getsize(os.path.join(r, n)) for r, _, fs in os.walk(OUT) for n in fs if n.endswith('.dds')) \
        if os.path.isdir(OUT) else 0
    print(f"total: {len(done)}/{len(jobs)} done, ~{total_left:.1f} h of GPU left; pack {size / 1048576:.0f} MB in {OUT}")
    lock = load_json(LOCK, {})
    if pid_alive(lock.get('pid')):
        print(f"running since {lock.get('started')}: phases {', '.join(lock.get('phases', []))} (pid {lock['pid']})"
              + ('; stop requested' if os.path.exists(STOP) else ''))
    else:
        print('not running')
    if os.path.exists(LOG):
        with open(LOG, encoding='utf-8') as f:
            tail = f.readlines()[-3:]
        print('last log lines:\n  ' + '  '.join(tail).rstrip())
    if failed:
        print(f'failed ({len(failed)}), retried on the next run:')
        for k, v in list(failed.items())[:10]:
            print(f"  {v['phase']}/{v['step']} {k}: {v['error'][:120]}")


SESSIONS = os.path.join(KIT, 'logs', 'sessions')
LOADED = re.compile(r'INFO  textures\| (HD|memEx)\s+\S+ :: (.+?) \| src ')


def cmd_applied(args):
    """'hd applied [first session]': does the game show the pack? Every planned texture the game loaded in the
    sessions (logs/sessions/<time>/textures.log, rolhook: 'HD' = served from the pack, 'memEx' = the archive's
    original) against the plan: a finished texture of a chain other than O must come from the pack, an O one (or one
    not finished) from the archive. Default: the sessions started after the pack last changed (done.json); a session
    from before that saw the older pack, so a difference there is only listed as stale."""
    changed = time.strftime('%Y-%m-%d_%H-%M-%S', time.localtime(os.path.getmtime(DONE)))
    since = args[0] if args else changed
    sessions = sorted(d for d in os.listdir(SESSIONS) if d >= since and os.path.isdir(os.path.join(SESSIONS, d)))
    if not sessions:
        print(f"no sessions since {since} (the pack last changed then); 'hd applied <session folder>' looks further back")
        return
    jobs, done = load_jobs(), load_json(DONE, {})
    plan = {j['entry'].lstrip('.' + BS).lower(): j for j in jobs}
    seen = collections.defaultdict(set)   # entry -> {(mode, session)}
    for s in sessions:
        path = os.path.join(SESSIONS, s, 'textures.log')
        if os.path.exists(path):
            with open(path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    m = LOADED.search(line)
                    if m and m.group(2).lower() in plan:
                        seen[m.group(2).lower()].add((m.group(1), s))
    stats = collections.defaultdict(collections.Counter)   # phase -> how its textures were served the last time
    wrong, stale = [], 0
    for e, loads in sorted(seen.items()):
        j = plan[e]
        want = 'HD' if j['chain'] != 'O' and j['id'] in done else 'memEx'
        last = max((s, m) for m, s in loads)[1]
        stats[j['phase']]['pack' if last == 'HD' else 'original'] += 1
        bad = sorted((s, m) for m, s in loads if m != want)
        if any(s >= changed for s, _ in bad):
            stats[j['phase']]['wrong'] += 1
            wrong.append((e, j['chain'], want, next((s, m) for s, m in bad if s >= changed)))
        elif bad:
            stale += 1
    fresh = sum(1 for s in sessions if s >= changed)
    print(f"{len(sessions)} sessions since {since}: {sessions[0]} .. {sessions[-1]}; the pack last changed {changed}, "
          f"{fresh} of the sessions saw it as it is")
    print(f"{'phase':10s} {'loaded':>6s} {'from pack':>9s} {'original':>8s} {'wrong':>5s}   (served the last time)")
    for p in PHASES:
        c = stats.get(p)
        if c:
            print(f"{p:10s} {c['pack'] + c['original']:6d} {c['pack']:9d} {c['original']:8d} {c['wrong']:5d}")
    if stale:
        print(f"{stale} were served otherwise only in sessions from before the pack last changed (the older pack)")
    if wrong:
        print('wrong (texture, chain, expected, served as):')
        for e, chain, want, (s, mode) in wrong[:20]:
            print(f"  {e}  {chain}  want {want}, got {mode} in {s}")
    elif fresh:
        print(f"the {fresh} sessions with the current pack got every planned texture from where the plan puts it")
    else:
        print('no session has run with the current pack yet: its textures are checked by the next one')


def flux_cache(phases, run):
    """Flux kept as cached images only (work/hd_full/flux: <id>.png strong D, <id>_C.png light C) for every diffuse
    the plan put on Flux, also those sent back to B: the pack keeps the plain upscale, so the three versions can be
    compared later and nothing has to be restored."""
    todo = []
    for j in load_json(JOBS, []):
        if j['phase'] in phases and j['map'] == 'diffuse' and j['chain'] == 'D' and os.path.exists(sr_path(j)):
            todo += [dict(j, chain='D'), dict(j, chain='C')]
    todo = [j for j in todo if not os.path.exists(flux_path(j))]
    if not todo:
        log('flux cache: nothing to do')
        return True
    import comfy_client
    log(f"flux cache: {len(todo)} renders for {', '.join(phases)} (D and C), the pack stays as it is")
    proc = comfy_client.start(os.path.join(W, 'comfyui.log'))
    run.comfy(proc)
    try:
        for k, j in enumerate(todo):
            if run.stopping():
                log('flux cache: stopped')
                return False
            t = time.time()
            try:
                ww, wh = work_size(j)
                base = np.asarray(Image.open(sr_path(j)).convert('RGB'))
                work = resize(base, (ww, wh)) if (base.shape[1], base.shape[0]) != (ww, wh) else base
                ctrl = resize(np.ascontiguousarray(source(j)[..., :3]), (ww, wh), Image.BICUBIC)
                res = comfy_client.refine(work, ctrl, prompt_for(j), tile=1408 if max(ww, wh) <= 1280 else 1152,
                                          overlap=256, tileable=False, tag=j['id'], **FLUX[j['chain']])
                Image.fromarray(res).save(flux_path(j))
                log(f"flux cache: {j['chain']} {j['id']} {time.time() - t:.0f}s ({len(todo) - k - 1} left)")
            except Exception as e:
                log(f"flux cache: FAIL {j['chain']} {j['id']}: {e!r}")
    finally:
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                kill_pid(proc.pid)
        run.comfy(None)
    log('flux cache: done')
    return True


def cmd_run(args):
    limit, ids, cache = None, None, None
    if '--flux-cache' in args:
        i = args.index('--flux-cache')
        cache = [x.strip() for x in args[i + 1].split(',') if x.strip()]
        args = args[:i] + args[i + 2:]
    if '--limit' in args:
        i = args.index('--limit')
        limit = int(args[i + 1])
        args = args[:i] + args[i + 2:]
    if '--ids' in args:
        i = args.index('--ids')
        ids = set(x.strip() for x in args[i + 1].split(',') if x.strip())
        args = args[:i] + args[i + 2:]
    phases = args or PHASES
    bad = [p for p in phases if p not in PHASES]
    if bad:
        raise SystemExit(f"unknown phase(s) {', '.join(bad)}; phases: {', '.join(PHASES)}")
    jobs = load_jobs()
    run = Run(phases)
    try:
        done, failed = load_json(DONE, {}), load_json(FAILED, {})
        forget_changed(jobs, done)
        log(f"run: phases {', '.join(phases)}{f', limit {limit} per phase' if limit else ''}"
            f"{f', only {len(ids)} ids' if ids else ''}; {len(done)}/{len(jobs)} textures done so far")
        for p in phases:
            if not Phase(p, jobs, done, failed, run, limit, ids).execute():
                log(f"stopped in phase {p}; 'hd run' continues where it stopped")
                return
        log('run finished')
        if cache:
            flux_cache(cache, run)
    finally:
        run.close()


def cmd_start(args):
    """'hd run' as a detached background process without a console window: closing a window, VS Code or the
    Claude session does not stop it; 'hd stop' does. Its output goes to work/hd_full/console.log."""
    lock = load_json(LOCK, {})
    if pid_alive(lock.get('pid')):
        raise SystemExit(f"already running (pid {lock['pid']}); progress: hd status, stop: hd stop")
    out = open(os.path.join(W, 'console.log'), 'a', encoding='utf-8')
    out.write(f"\n===== hd start {' '.join(args)} at {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    out.flush()
    flags = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    p = subprocess.Popen([sys.executable, '-u', os.path.abspath(__file__), 'run', *args],
                         cwd=os.path.dirname(os.path.abspath(__file__)), stdout=out, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, creationflags=flags, close_fds=True)
    print(f"started in the background (pid {p.pid}, no window): progress 'hd status', stop 'hd stop'")


def cmd_stop():
    lock = load_json(LOCK, {})
    if pid_alive(lock.get('pid')):
        open(STOP, 'w').close()
        print('stop requested: the run ends after the texture in progress (Flux: up to a few minutes)')
        return
    if pid_alive(lock.get('comfy_pid')):
        kill_pid(lock['comfy_pid'])
        print('no run active; stopped the ComfyUI it left behind')
    else:
        print('no run active')
    if os.path.exists(LOCK):
        os.remove(LOCK)


def cmd_reset(args):
    if not args or args[0] not in PHASES:
        raise SystemExit(f"usage: hd reset <phase> [--cache]; phases: {', '.join(PHASES)}")
    if pid_alive(load_json(LOCK, {}).get('pid')):
        raise SystemExit("a run is active: 'hd stop' first")
    phase, cache = args[0], '--cache' in args
    jobs = load_jobs()
    done, failed = load_json(DONE, {}), load_json(FAILED, {})
    n = 0
    for j in jobs:
        if j['phase'] != phase:
            continue
        done.pop(j['id'], None)
        failed.pop(j['id'], None)
        p = pack.entry_out_path(OUT, j['entry'])
        if os.path.exists(p):
            os.remove(p)
            n += 1
        if cache:
            for c in (sr_path(j), flux_path(j)):
                if os.path.exists(c):
                    os.remove(c)
    save_json(DONE, done)
    save_json(FAILED, failed)
    log(f"reset {phase}: {n} textures removed from the pack{', caches cleared' if cache else ''}")


def cmd_fix_alpha(args):
    """Re-encodes the finished textures whose alpha the old upscale_alpha damaged (pack.alpha_was_damaged), from the
    cached RGB results: no new upscaling; normal maps rerun only the quick NormalV3 step. --dry lists them."""
    if pid_alive(load_json(LOCK, {}).get('pid')):
        raise SystemExit('the pass is running: hd stop first')
    dry = '--dry' in args
    phases = [p for p in args if p in PHASES] or PHASES
    jobs = load_jobs()
    by_id = {j['id']: j for j in jobs}
    done = load_json(DONE, {})
    todo = []
    for j in jobs:
        if j['phase'] not in phases or j['id'] not in done:
            continue
        info = pack.source_format(j['archive'], j['entry'])
        if info['fmt'] == 'DXT1':
            continue
        a = source(j)[..., 3]
        if pack.alpha_was_damaged(a):
            todo.append((j, int(a.min()), int(a.max())))
    print(f'{len(todo)} textures with a damaged alpha in {", ".join(phases)}')
    for j, lo, hi in todo[:20]:
        print(f"  {j['phase']:9s} {j['chain']:3s} alpha {lo}..{hi}  {j['id']}")
    if dry or not todo:
        return
    log(f'fix-alpha: {len(todo)} textures')
    for j, lo, hi in todo:
        t = time.time()
        try:
            if j['chain'] == 'N3':
                d = by_id[j['pair']]
                rep = write_dds(j, normal_of=final_diffuse(d), tileable_=tileable(d))
            elif j['chain'] == 'L':
                rep = write_dds(j, resize(np.ascontiguousarray(source(j)[..., :3]), tuple(j['dst'])))
            else:
                rep = write_dds(j, final_diffuse(j), tileable_=tileable(j))
            log(f"fix-alpha: {j['chain']:2s} {j['id']} alpha {lo}..{hi} {time.time() - t:.1f}s")
        except Exception as e:
            log(f"fix-alpha: FAIL {j['id']}: {e!r}")
    try:
        import gan_upscale
        gan_upscale.release()
    except Exception:
        pass
    log('fix-alpha: done')


def main():
    for d in (W, SR_DIR, FLUX_DIR, TMP_DIR):
        os.makedirs(d, exist_ok=True)
    a = sys.argv[1:]
    cmd = a[0] if a else 'status'
    if cmd == 'plan':
        cmd_plan()
    elif cmd == 'status':
        cmd_status()
    elif cmd == 'run':
        cmd_run(a[1:])
    elif cmd == 'start':
        cmd_start(a[1:])
    elif cmd == 'stop':
        cmd_stop()
    elif cmd == 'reset':
        cmd_reset(a[1:])
    elif cmd == 'fallback':
        cmd_fallback(a[1:])
    elif cmd == 'fix-alpha':
        cmd_fix_alpha(a[1:])
    elif cmd == 'applied':
        cmd_applied(a[1:])
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
