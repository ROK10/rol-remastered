"""Update rates of the units on screen from rolhook's motion probe ([debug] motion_probe, autotest case motion):
how often a unit's place in the world and its pose change from frame to frame, against the frame rate.

motion.log (native/rolhook/motion.cpp): "T id texture"; "F frame t_ms dt_ms units main_width vp0..vp15" (vp = the
view camera's view-projection from engine memory, '-' when missing); "B frame tex nv m0..m15" (up to 4 buildings of
the main pass); "U frame tex nv unit rt m0..m15 posed" (rt = the render target's width - the main pass draws into a
target as wide as the backbuffer; m = the unit's c0..c3; posed = the largest change of a vertex's model-space offset
from the centre since the last frame, -1 unknown).
A draw's registers c0..c3 are the transposed world x view-projection, T = (W VP)^T. Against a building of the same
pass, T_unit^T (T_b^T)^-1 = W_unit W_b^-1 - the camera drops out: its 4th row is where the unit stands, seen from
the building (the anchor: the building drawn in most frames).

  * walking units: those whose world position went at least WALK units over the window; each frame's step along
    their way - movement shown every frame advances a little every frame, movement shown at the simulation's rate
    advances in jumps with standing frames between them;
  * poses: posed > POSE - an animation sampled every frame changes the pose every frame, one updated at the
    simulation's rate in jumps;
  * building parts ([debug] motion_buildings: "P frame tex nv dyn m0..m15", texture 1 = the part's): their place in
    the anchor's space, followed like the units (same texture and vertex count, nearest place and turn); a part whose
    turn (the angle between its frames' rotations) or place changes on 10%+ of its frames is animated - a mill's
    sails, a mine's engines. dyn parts come from dynamic buffers: their animation would be in the vertices.
Gaps = runs of frames without a change between two changes, in frames and in milliseconds.

  py -3.14 modkit/tools/motion.py <session or motion.log> [--units]"""
import collections
import math
import os
import statistics
import sys

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSIONS = os.path.join(KIT, 'logs', 'sessions')
WALK = 5.0      # world units over the window: the unit walks
STEP = 1e-3     # a frame's advance that counts as moving
POSE = 1e-4     # a pose change that counts
CAM = 1e-5      # a change of the camera's matrix that counts
FOLLOW = 20.0   # world units a unit may go from one frame to the next and still be the same unit
TURN = 1e-4     # radians: a building part's turn from one frame to the next that counts
TURN_W = 100.0  # following parts: world units one radian of turn counts as (identical parts on one axle differ in turn)


def load(arg):
    path = arg if os.path.isfile(arg) else os.path.join(SESSIONS, arg, 'motion.log')
    tex, frames, units, anchors = {}, {}, collections.defaultdict(list), collections.defaultdict(dict)
    parts = collections.defaultdict(list)   # frame -> [(tex, nv, dyn, m)]
    clock = {}                              # frame -> (world +0x38, tick part as the engine had it, as given); None, None
                                            # without anim_clock
    main_w = 0
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == 'T':
                tex[int(p[1])] = ' '.join(p[2:])
            elif p[0] == 'F':
                main_w = int(p[5]) or main_w
                vp = [float(x) for x in p[6:22]] if p[6] != '-' else None
                frames[int(p[1])] = (float(p[2]), float(p[3]), vp)
            elif p[0] == 'B':
                key = (int(p[2]), int(p[3]))
                anchors[key].setdefault(int(p[1]), [float(x) for x in p[4:20]])
            elif p[0] == 'U':
                fr, t, nv, uid, rt = int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])
                m = [float(x) for x in p[6:22]]
                units[uid].append((fr, t, nv, m, float(p[22]), rt))
            elif p[0] == 'P':
                parts[int(p[1])].append((int(p[2]), int(p[3]), int(p[4]), [float(x) for x in p[5:21]]))
            elif p[0] == 'W':
                clock[int(p[1])] = (int(p[3]), int(p[6]) if len(p) > 7 else None, int(p[7]) if len(p) > 7 else None)
    return tex, frames, units, anchors, main_w, parts, clock


def inverse(m):
    """4x4 inverse (row-major list of 16), Gauss-Jordan with partial pivoting; None when singular."""
    a = [m[r * 4:r * 4 + 4] + [1.0 if r == c else 0.0 for c in range(4)] for r in range(4)]
    for c in range(4):
        piv = max(range(c, 4), key=lambda r: abs(a[r][c]))
        if abs(a[piv][c]) < 1e-12:
            return None
        a[c], a[piv] = a[piv], a[c]
        d = a[c][c]
        a[c] = [x / d for x in a[c]]
        for r in range(4):
            if r != c and a[r][c]:
                f = a[r][c]
                a[r] = [x - f * y for x, y in zip(a[r], a[c])]
    return [a[r][4 + c] for r in range(4) for c in range(4)]


def mul(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def pct(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * (len(xs) - 1) + 0.5))] if xs else float('nan')


def gaps(flags):
    """Lengths of the runs of False between two True."""
    out, run, seen = [], 0, False
    for f in flags:
        if f:
            if seen and run:
                out.append(run)
            run, seen = 0, True
        else:
            run += 1
    return out


def describe_gaps(g, frame_ms):
    if not g:
        return 'no gaps (a change every frame)'
    c = collections.Counter(g)
    top = ', '.join(f'{k} frame{"s" if k > 1 else ""} x{n}' for k, n in sorted(c.items())[:8])
    med = statistics.median(g)
    return f'gaps {top}; median {med:g} frames = {med * frame_ms:.1f} ms standing still'


def transpose(m):
    return [m[c * 4 + r] for r in range(4) for c in range(4)]


def steps_line(ratios, what):
    """Per-frame advance against the frame's time (ratios): how even it is - the frames that nearly stood (< 0.3 of
    the median) and those that leapt (> 1.6 x)."""
    if len(ratios) < 10:
        return f'{what}: too few frames'
    med = statistics.median(ratios)
    if med <= 0:
        return f'{what}: stands still (median 0)'
    cv = statistics.pstdev(ratios) / statistics.mean(ratios)
    stood = sum(1 for r in ratios if r < 0.3 * med)
    leapt = sum(1 for r in ratios if r > 1.6 * med)
    return (f'{what}: per ms of frame time {pct(ratios, 0.05) / med:.2f} .. {pct(ratios, 0.95) / med:.2f} x the median '
            f'(varies {cv:.0%}); {stood} frames nearly stood, {leapt} leapt, of {len(ratios)}')


def evenness(track, frames):
    """A CPU-posed part's per-frame pose change (posed: the largest vertex move, for a turning part ~ the turn) over
    the frame's time."""
    ratios = []
    for fr, _, _, _, p, _ in track:
        dt = frames.get(fr, (0, 0, None))[1]
        if p >= 0 and dt > 0:
            ratios.append(p / dt)
    return steps_line(ratios, 'pose change')


def clock_report(clock, frames, order):
    """The game world's clock (W lines): its advance per frame against the frame time; with anim_clock, the frames on
    which a late tick's part ran on past the tick's end (given > the engine's)."""
    out = []
    ratios = []
    for a, b in zip(order, order[1:]):
        if a in clock and b in clock and frames[b][1] > 0:
            ratios.append((clock[b][0] - clock[a][0]) / frames[b][1])
    if ratios:
        out.append(steps_line(ratios, 'world clock (+0x38)'))
    ran_on = [v[2] - v[1] for v in clock.values() if v[1] is not None and v[2] > v[1]]
    if any(v[1] is not None for v in clock.values()):
        out.append(f'late ticks run on (anim_clock): {len(ran_on)} frames, the part {statistics.mean(ran_on):.1f} ms past '
                   f'the tick on average' if ran_on else 'late ticks run on (anim_clock): none in the window')
    return '\n'.join(out)


def rotation(w):
    """The rotation of a world matrix (row vectors: rows 0-2 the turned and scaled axes): its rows made unit long."""
    rows = []
    for r in range(3):
        v = w[r * 4:r * 4 + 3]
        n = math.sqrt(sum(x * x for x in v)) or 1.0
        rows.append([x / n for x in v])
    return rows


def turn(a, b):
    """Radians between two rotations: Q = A^T B, the angle from its skew part and trace (exact for small angles)."""
    q = [[sum(a[i][k] * b[i][l] for i in range(3)) for l in range(3)] for k in range(3)]
    s = math.sqrt((q[2][1] - q[1][2]) ** 2 + (q[0][2] - q[2][0]) ** 2 + (q[1][0] - q[0][1]) ** 2)
    return math.atan2(s, q[0][0] + q[1][1] + q[2][2] - 1.0)


def building_parts(parts, inv, order, tex, frames, frame_ms):
    """Building parts followed through the frames in the anchor's space; prints the animated ones."""
    draws = collections.defaultdict(list)   # frame -> [((tex, nv), pos, rot, dyn)]
    for fr in order:
        if fr not in inv:
            continue
        for t, nv, dyn, m in parts.get(fr, []):
            w = mul(transpose(m), inv[fr])
            draws[fr].append(((t, nv), (w[12], w[13], w[14]), rotation(w), dyn))
    paths, live, next_id = {}, {}, 0   # track -> {frame: (pos, rot)}; track -> (key, pos, rot, frame, dyn)
    for fr in order:
        cur = draws.get(fr, [])
        pairs = []
        for tid, (key, p, r, f0, _) in live.items():
            if f0 != fr - 1:
                continue
            for i, (key2, p2, r2, _) in enumerate(cur):
                if key2 == key:
                    d = math.dist(p, p2)
                    if d <= FOLLOW:
                        pairs.append((d + TURN_W * turn(r, r2), tid, i))
        pairs.sort()
        used_t, used_i = set(), set()
        for _, tid, i in pairs:
            if tid in used_t or i in used_i:
                continue
            used_t.add(tid)
            used_i.add(i)
            key, p, r, dyn = cur[i]
            live[tid] = (key, p, r, fr, dyn)
            paths[tid][fr] = (p, r)
        for i, (key, p, r, dyn) in enumerate(cur):
            if i not in used_i:
                live[next_id] = (key, p, r, fr, dyn)
                paths[next_id] = {fr: (p, r)}
                next_id += 1
    rows, still, noise, dyn_parts = [], 0, [], 0
    for tid, pos in paths.items():
        fs = sorted(pos)
        if len(fs) < 30:
            continue
        key, dyn = live[tid][0], live[tid][4]
        dyn_parts += dyn
        pairs = [(a, b) for a, b in zip(fs, fs[1:]) if b == a + 1]
        turns = [turn(pos[a][1], pos[b][1]) for a, b in pairs]
        moves = [math.dist(pos[a][0], pos[b][0]) for a, b in pairs]
        changed = [t > TURN or s > STEP for t, s in zip(turns, moves)]
        if sum(changed) < max(3, len(changed) // 10):
            still += 1
            noise.extend(turns)
            continue
        steps = [t for t, c in zip(turns, changed) if c]
        span = (frames[fs[-1]][0] - frames[fs[0]][0]) / 1000.0
        cv = statistics.pstdev(steps) / statistics.mean(steps) if len(steps) > 1 and statistics.mean(steps) else 0.0
        rows.append((tid, tex.get(key[0], '?').split('\\')[-1], key[1], dyn, len(fs), sum(changed) / len(changed),
                     gaps(changed), statistics.median(steps), cv, math.degrees(sum(turns)) / span if span > 0 else 0.0))
    seen = sum(1 for v in paths.values() if len(v) >= 30)
    print(f'building parts: {len(paths)} seen, {seen} followed for 30+ frames ({dyn_parts} of them from dynamic buffers),'
          f' {still} still; turn noise of the still ones: 95% under {pct(noise, 0.95):.2g} rad')
    if not rows:
        print('ANIMATED building parts: none turned or moved on 10%+ of its frames')
        return
    share = [r[5] for r in rows]
    allg = [g for r in rows for g in r[6]]
    print(f'ANIMATED building parts: {len(rows)}; frames on which they turned or moved: median '
          f'{statistics.median(share):.0%} (from {min(share):.0%} to {max(share):.0%})')
    print(f'  {describe_gaps(allg, frame_ms)}')
    for tid, name, nv, dyn, n, sh, g, step, cv, speed in sorted(rows, key=lambda r: (r[1], r[2])):
        print(f'  part {tid:5d} {name:34s} {nv:5d} v{" dyn" if dyn else ""} {n:4d} frames, changed on {sh:4.0%}, '
              f'{speed:6.1f} deg/s, a step {math.degrees(step):.3f} deg (varies {cv:.0%}), {describe_gaps(g, frame_ms)}')


def main():
    tex, frames, units, anchors, main_w, parts, clock = load(sys.argv[1])
    if not frames:
        raise SystemExit('motion.log has no frames')
    order = sorted(frames)
    dts = [frames[f][1] for f in order[1:] if frames[f][1] > 0]
    frame_ms = statistics.mean(dts)
    print(f'{len(frames)} frames, {frame_ms:.2f} ms apart on average ({1000 / frame_ms:.0f} fps; median '
          f'{pct(dts, 0.5):.2f}, 95% {pct(dts, 0.95):.2f}, max {max(dts):.2f} ms); main pass {main_w} px wide')
    vps = {f: frames[f][2] for f in order if frames[f][2]}
    cam_moves = sum(1 for a, b in zip(order, order[1:]) if a in vps and b in vps and
                    max(abs(x - y) for x, y in zip(vps[a], vps[b])) > CAM)
    print(f'camera (engine memory): view-projection in {len(vps)} frames, changed on {cam_moves} of them')
    # the anchor: the building of the main pass seen in most frames
    inv = {}
    if anchors:
        key = max(anchors, key=lambda k: len(anchors[k]))
        am = anchors[key]
        moves = sum(1 for a, b in zip(order, order[1:]) if a in am and b in am and
                    max(abs(x - y) for x, y in zip(am[a], am[b])) > CAM)
        print(f'anchor: {tex.get(key[0], "?").split(chr(92))[-1]} ({key[1]} vertices) in {len(am)} frames; its matrix '
              f'changed on {moves} of them (the camera moved)')
        inv = {f: inverse(transpose(m)) for f, m in am.items()}
    else:
        print('anchor: no building of the main pass - movement not measured')

    tracks = {u: sorted(v) for u, v in units.items() if len(v) >= 30}
    main_tracks = sum(1 for v in tracks.values() if v[0][5] == main_w)
    print(f'{len(units)} units seen, {len(tracks)} followed for 30+ frames ({main_tracks} of them in the main pass)')
    walk_rows, pose_rows, still_units, part_rows = [], [], [], []
    # poses: the probe's own following (texture, vertex count, nearest clip-space position) is good enough - all
    # units change pose on the same frames
    for uid, v in tracks.items():
        full = tex.get(v[0][1], '?')
        name = full.split('\\')[-1]
        posed = [p > POSE for _, _, _, _, p, _ in v[1:] if p >= 0]
        if v[0][5] == main_w and len(posed) >= 30 and sum(posed) >= max(3, len(posed) // 10):
            # building parts posed on the CPU come as U lines too (motion_buildings), with the part's texture
            rows = part_rows if '\\buildings\\' in full else pose_rows
            rows.append((uid, name, len(v), sum(posed) / len(posed), gaps(posed)))
    # places: units of a kind walking in a group swap places in clip-space matching, so they are followed again here,
    # in the anchor's space: each frame, the closest pairs of (last frame's unit, this frame's draw) of the same
    # texture and vertex count first, up to FOLLOW world units apart
    draws = collections.defaultdict(list)   # frame -> [(tex, nv, pos)]
    for v in units.values():
        for fr, t, nv, m, _, rt in v:
            if '\\buildings\\' in tex.get(t, ''):   # a building part posed on the CPU (motion_buildings) stays put
                continue
            if rt == main_w and fr in inv:
                w = mul(transpose(m), inv[fr])
                draws[fr].append((t, nv, (w[12], w[13], w[14])))
    paths = {}              # track -> {frame: pos}
    live = {}               # track -> (tex, nv, pos, frame)
    next_id = 0
    for fr in order:
        cur = draws.get(fr, [])
        pairs = []
        for tid, (t, nv, p, f0) in live.items():
            if f0 != fr - 1:
                continue
            for i, (t2, nv2, p2) in enumerate(cur):
                if t2 == t and nv2 == nv:
                    d = math.dist(p, p2)
                    if d <= FOLLOW:
                        pairs.append((d, tid, i))
        pairs.sort()
        used_t, used_i = set(), set()
        for d, tid, i in pairs:
            if tid in used_t or i in used_i:
                continue
            used_t.add(tid)
            used_i.add(i)
            t, nv, p = cur[i]
            live[tid] = (t, nv, p, fr)
            paths[tid][fr] = p
        for i, (t, nv, p) in enumerate(cur):
            if i not in used_i:
                live[next_id] = (t, nv, p, fr)
                paths[next_id] = {fr: p}
                next_id += 1
    for tid, pos in paths.items():
        name = tex.get(live[tid][0], '?').split('\\')[-1]
        v = pos
        fs = sorted(pos)
        if len(fs) < 30:
            continue
        still = all(math.dist(pos[a], pos[b]) < STEP for a, b in zip(fs, fs[1:]))
        if still:
            still_units.append(tid)
        uid = tid
        net = math.dist(pos[fs[0]], pos[fs[-1]])
        if net < WALK:
            continue
        d = [(pos[fs[-1]][k] - pos[fs[0]][k]) / net for k in range(3)]
        steps = [sum((pos[b][k] - pos[a][k]) * d[k] for k in range(3)) for a, b in zip(fs, fs[1:]) if b == a + 1]
        moving = [s > STEP for s in steps]
        adv = [s for s in steps if s > STEP]
        cv = statistics.pstdev(adv) / statistics.mean(adv) if len(adv) > 1 else float('nan')
        span = (frames[fs[-1]][0] - frames[fs[0]][0]) / 1000.0
        walk_rows.append((uid, name, len(v), net, net / span if span > 0 else 0.0, sum(moving) / max(1, len(moving)),
                          gaps(moving), cv, max(adv) if adv else 0.0))

    if inv:
        print(f'standing units (not one step against the anchor in the window): {len(still_units)} - a camera move '
              f'that the anchor did not cancel would show here as every unit moving')
    print()
    if walk_rows:
        share = [r[5] for r in walk_rows]
        allg = [g for r in walk_rows for g in r[6]]
        cvs = [r[7] for r in walk_rows if not math.isnan(r[7])]
        print(f'WALKING units: {len(walk_rows)} (speeds {min(r[4] for r in walk_rows):.0f}..'
              f'{max(r[4] for r in walk_rows):.0f} world units/s); frames on which they advanced: median '
              f'{statistics.median(share):.0%} (from {min(share):.0%} to {max(share):.0%})')
        print(f'  {describe_gaps(allg, frame_ms)}')
        if cvs:
            print(f'  advance per moving frame varies by {statistics.median(cvs):.0%} (median coefficient of variation;'
                  f' small = even steps)')
    else:
        print('WALKING units: none walked 5+ world units in the window')
    if pose_rows:
        share = [r[3] for r in pose_rows]
        allg = [g for r in pose_rows for g in r[4]]
        print(f'ANIMATED units: {len(pose_rows)}; frames on which the pose changed: median '
              f'{statistics.median(share):.0%} (from {min(share):.0%} to {max(share):.0%})')
        print(f'  {describe_gaps(allg, frame_ms)}')
    else:
        print('ANIMATED units: none changed pose on 10%+ of its frames')
    if part_rows:
        share = [r[3] for r in part_rows]
        allg = [g for r in part_rows for g in r[4]]
        print(f'ANIMATED building parts posed on the CPU: {len(part_rows)}; frames on which the pose changed: median '
              f'{statistics.median(share):.0%} (from {min(share):.0%} to {max(share):.0%})')
        print(f'  {describe_gaps(allg, frame_ms)}')
        for uid, name, n, sh, g in sorted(part_rows, key=lambda r: (r[1], r[0])):
            print(f'  part {uid:5d} {name:36s} {n:4d} frames, changed on {sh:4.0%}, {describe_gaps(g, frame_ms)}')
            print(f'             {evenness(sorted(units[uid]), frames)}')
    if clock:
        print()
        print(clock_report(clock, frames, order))
    if parts:
        print()
        if inv:
            building_parts(parts, inv, order, tex, frames, frame_ms)
        else:
            print('building parts: no anchor in the main pass - not measured')

    if '--units' in sys.argv:
        print()
        for uid, name, n, net, speed, share, g, cv, big in sorted(walk_rows, key=lambda r: -r[4]):
            print(f'  walk {uid:5d} {name:36s} {n:4d} frames, {net:7.1f} units at {speed:5.1f}/s, advanced on '
                  f'{share:4.0%}, biggest step {big:.2f}, {describe_gaps(g, frame_ms)}')
        for uid, name, n, share, g in sorted(pose_rows, key=lambda r: r[3]):
            print(f'  pose {uid:5d} {name:36s} {n:4d} frames, changed on {share:4.0%}, {describe_gaps(g, frame_ms)}')


if __name__ == '__main__':
    main()
