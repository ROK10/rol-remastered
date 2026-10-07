"""Calls per frame from rolhook's call counter ([debug] count_calls, calls.log): for each function, on how many
frames it ran, how many times per frame, and how its frames are spaced - every frame, or on the simulation's ticks
(the motion probe's pose-change frames, when motion.log is in the same session, mark the ticks).

  py -3.14 modkit/tools/calls.py <session>"""
import collections
import os
import statistics
import sys

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSIONS = os.path.join(KIT, 'logs', 'sessions')


def ticks_from_motion(path):
    """Frames on which more than half of the followed units changed their pose."""
    seen, changed = collections.Counter(), collections.Counter()
    if not os.path.exists(path):
        return set()
    for line in open(path, encoding='utf-8', errors='replace'):
        p = line.split()
        if p and p[0] == 'U':
            posed = float(p[-1])
            if posed >= 0:
                seen[int(p[1])] += 1
                if posed > 1e-4:
                    changed[int(p[1])] += 1
    return {f for f in seen if changed[f] * 2 > seen[f]}


def main():
    session = sys.argv[1] if os.path.isdir(sys.argv[1]) else os.path.join(SESSIONS, sys.argv[1])
    lines = open(os.path.join(session, 'calls.log'), encoding='utf-8').read().splitlines()
    names = lines[0].split('calls of')[1].split()
    rows = []
    for line in lines[1:]:
        p = line.split()
        if p and p[0] == 'C':
            rows.append((int(p[1]), float(p[3]), [int(x) for x in p[4:]]))
    ticks = ticks_from_motion(os.path.join(session, 'motion.log'))
    frame_ms = statistics.mean(dt for _, dt, _ in rows[1:])
    print(f'{len(rows)} frames, {frame_ms:.2f} ms apart; {len(ticks)} tick frames from motion.log')
    for i, name in enumerate(names):
        on = [(fr, c[i]) for fr, _, c in rows if c[i]]
        if not on:
            print(f'  {name}: never')
            continue
        frames = [fr for fr, _ in on]
        gaps = collections.Counter(b - a for a, b in zip(frames, frames[1:]))
        per = [n for _, n in on]
        at_ticks = sum(1 for fr in frames if fr in ticks)
        print(f'  {name}: on {len(frames)} of {len(rows)} frames ({len(frames) / len(rows):.0%}), {min(per)}..{max(per)} '
              f'calls per frame (median {statistics.median(per):g}); frame steps {dict(gaps.most_common(4))}; '
              f'{at_ticks} of its frames are ticks')


if __name__ == '__main__':
    main()
