"""HD pack from the battle 01 results with the user's picks (2026-10-03):

  natural textures (terrain tiles)        -> B  PBRify V4
  textures with drawn patterns (atlases)  -> D  PBRify + Flux strong
  colour lock on everywhere; normal maps  -> N3 built from the chosen diffuse

Writes modkit/mods/hd/<entry>.dds (+ pack.json with the QA numbers); the hd_textures mod points rolhook there.
Run with the ComfyUI portable Python.
"""
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import KIT, load_png  # noqa: E402
from battle01_set import SET, NORMALS  # noqa: E402
import pack  # noqa: E402

BATTLE = os.path.join(KIT, 'work', 'battle01', 'out')
OUT = os.path.join(KIT, 'mods', 'hd')
CHAIN_BY_KIND = {'tile': 'B_pbrify', 'atlas': 'D_flux_strong'}


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    reports, chosen = [], {}
    for tid, big, entry, kind, _prompt in SET:
        chain = CHAIN_BY_KIND[kind] + '_cl'
        rgb = load_png(os.path.join(BATTLE, tid, chain + '.png'), 'RGB')
        chosen[tid] = (rgb, kind == 'tile')
        t = time.time()
        rep = pack.build_texture(big, entry, rgb, OUT)
        rep.update({'id': tid, 'chain': chain, 'seconds': round(time.time() - t, 1)})
        reports.append(rep)
        print(f"{tid:13s} {rep['src']:>13s} -> {rep['fmt']} {rep['size'][0]}x{rep['size'][1]} L{rep['mips']} "
              f"{rep['bytes'] / 1048576:5.1f} MB  alpha {rep['alpha']:6s} PSNR rgb {rep['psnr_rgb']} a {rep['psnr_a']} "
              f"dE {rep['drift_dE']}  ({chain})", flush=True)
    for nid, big, entry, did in NORMALS:
        diffuse, tileable = chosen[did]
        t = time.time()
        rep = pack.build_texture(big, entry, diffuse, OUT, normal_of=diffuse, tileable=tileable)
        rep.update({'id': nid, 'chain': f'N3 from {did}', 'seconds': round(time.time() - t, 1)})
        reports.append(rep)
        print(f"{nid:13s} {rep['src']:>13s} -> {rep['fmt']} {rep['size'][0]}x{rep['size'][1]} L{rep['mips']} "
              f"{rep['bytes'] / 1048576:5.1f} MB  alpha {rep['alpha']:6s} PSNR rgb {rep['psnr_rgb']} a {rep['psnr_a']}",
              flush=True)
    pack.write_report(OUT, reports, {'source': 'battle01', 'rule': CHAIN_BY_KIND})
    total = sum(r['bytes'] for r in reports)
    print(f'pack: {len(reports)} textures, {total / 1048576:.1f} MB -> {OUT}')


if __name__ == '__main__':
    main()
