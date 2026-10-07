r"""Does every text of the Mods tab fit its place, in every language? (mods_tab: patches/tab_strings.py, optionswin.py)

  py -3.14 modkit/tools/tabfit.py [lang ...]

Measured with the fonts the game draws them with (loc\<lang>\fonts.xml and the font matcher; a size is the em in pixels,
lfHeight = -size at 0x00E6DA30): controls' text - captions, items, checkboxes, the notes - Arial bold 12 px; group
headers (FONT size 18) Centaur BHG bold 18 px, the window's title (26) 26 px, the tab buttons 22 px (the Russian
Keyboard title ~118 px measured on its tab). A character the font lacks is drawn by the
game's catch-all Arial Unicode MS (fonts\ARIALUNI.TTF), as Japanese and Chinese are; bold the game synthesises (Centaur,
Arial Unicode) is taken as 1 px wider a character. The restart mark #ICON142 is 18 px. Limits come from optionswin.py's
grid. Also prints the tab widths each language needs (title + 20 px, at least 100).
"""
import importlib.util
import os
import sys

from fontTools.ttLib import TTFont
from PIL import ImageFont

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.dirname(KIT)
PATCHES = os.path.join(KIT, 'modpack', 'mods_tab', 'patches')
sys.path.insert(0, PATCHES)
sys.path.insert(0, os.path.join(KIT, 'tools'))
import tab_strings  # noqa: E402

spec = importlib.util.spec_from_file_location('optionswin', os.path.join(PATCHES, 'optionswin.py'))
ow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ow)

ICON = 18
F = os.path.join(GAME, 'fonts')
# (file, face in the collection, px, bold the game synthesises: +1 px a character); fallbacks for missing characters
FONTS = {
    'default': {'arial': (r'C:\Windows\Fonts\arialbd.ttf', 0, 12, 0),
                'centaur': (os.path.join(F, 'CentBHG.ttf'), 0, 18, 1),
                'centaur_tab': (os.path.join(F, 'CentBHG.ttf'), 0, 22, 1),
                'centaur_big': (os.path.join(F, 'CentBHG.ttf'), 0, 26, 1)},
    # loc\JA\fonts.xml: HGPKyokashotai 48/26/20/19/18 (the tab's 22 is matched to 20), HGPGothicM 12/13/10/8/14
    'ja': {'arial': (os.path.join(F, 'HGRGM.TTC'), 1, 12, 0),
           'centaur': (os.path.join(F, 'HGRKK.TTC'), 1, 18, 0),
           'centaur_tab': (os.path.join(F, 'HGRKK.TTC'), 1, 20, 0),
           'centaur_big': (os.path.join(F, 'HGRKK.TTC'), 1, 26, 0)},
}
FALLBACKS = {'default': [(os.path.join(F, 'ARIALUNI.TTF'), 0)],
             'ja': [(os.path.join(F, 'MSMINCHO.TTC'), 1), (os.path.join(F, 'ARIALUNI.TTF'), 0)]}
_cmaps, _pil = {}, {}


def cmap(path, face):
    if (path, face) not in _cmaps:
        _cmaps[(path, face)] = set(TTFont(path, fontNumber=face)['cmap'].getBestCmap().keys())
    return _cmaps[(path, face)]


def pil(path, face, px):
    if (path, face, px) not in _pil:
        _pil[(path, face, px)] = ImageFont.truetype(path, px, index=face)
    return _pil[(path, face, px)]


def width(text, kind, lang='en'):
    """Pixel width of a text as the game draws it with the font of 'kind' in 'lang'."""
    group = 'ja' if lang == 'ja' else 'default'
    path, face, px, bold = FONTS[group][kind]
    w = 0.0
    icon = '#ICON142'
    while icon in text:
        text = text.replace(icon, '', 1)
        w += ICON
    run, run_font, run_synth = '', None, 0
    for ch in text:
        f, synth = (path, face), bold
        if ord(ch) not in cmap(path, face):
            for fb in FALLBACKS[group]:
                if ord(ch) in cmap(*fb):
                    f, synth = fb, 1 if group == 'default' else 0   # Arial Unicode has no bold face: the game makes it
                    break
        if f != run_font and run:
            w += pil(run_font[0], run_font[1], px).getlength(run) + run_synth * len(run)
            run = ''
        run_font, run_synth = f, synth
        run += ch
    if run:
        w += pil(run_font[0], run_font[1], px).getlength(run) + run_synth * len(run)
    return w


def checks(lang):
    """[(what, text, kind, limit)] - every text of the tab in 'lang' with the room it has."""
    s = tab_strings.LANGS[lang]
    R = ow.RESTART
    col_caption = [ow.COL[1] - ow.COL[0] - 6, ow.GROUP_W - 9 - ow.COL[1] - 2]
    col_check = [ow.COL[1] - ow.COL[0] - 10 - 22, ow.GROUP_W - 9 - ow.COL[1] - 22]
    full_check = ow.GROUP_W - 9 - ow.COL[0] - 22
    item = ow.COMBO_W - 22 - 6
    out = [('title', s['TITLE'], 'centaur_big', 850)]
    for g in ('G_IMAGE', 'G_SHADOWS', 'G_CAMERA', 'G_WINDOW'):
        out.append((g, s[g], 'centaur', ow.GROUP_W - 15))
    captions = [('C_ANISO', 0, ''), ('C_LOD', 1, ''), ('C_SHARPEN', 0, ''), ('C_VIBRANCE', 1, ''),
                ('C_SHADOW_SIZE', 0, R), ('C_SOFT', 1, ''), ('C_FIT', 0, ''), ('C_SSAO', 1, ''),
                ('C_ZOOM_OUT', 0, ''), ('C_ZOOM_STEPS', 1, ''), ('C_SPRING', 0, ''), ('C_UI_SCALE', 0, R)]
    for k, col, mark in captions:
        out.append((k, s[k] + mark, 'arial', col_caption[col]))
    for k, col, mark in (('K_HD', 0, ''), ('K_ATOC', 1, ''), ('K_SPLATS', 0, R), ('K_ANIM', 1, '')):
        out.append((k, s[k] + mark, 'arial', col_check[col]))
    for k, mark in (('K_GAMMA', ''), ('K_BORDERLESS', R)):
        out.append((k, s[k] + mark, 'arial', full_check))
    items = [s[k] for k in ('OFF_ANISO', 'OFF_SHARPEN', 'OFF_VIBRANCE', 'OFF_SOFT', 'OFF_SSAO')]
    for k in ('LV_SHARPEN', 'LV_VIBRANCE', 'LV_SOFT', 'LV_SSAO', 'LOD', 'FIT', 'SPRING'):
        items += s[k]
    items += [s['ORIGINAL'].format(v) for v in ('2048', '7500', '8')]
    items += [s['ORIGINAL'].format(s['PERCENT'].format(100)), s['AUTO']]
    for t in items:
        out.append(('item', t, 'arial', item))
    out.append(('LEGEND', s['LEGEND'].format(R), 'arial', 2 * 820 - 60))   # two lines, some room lost to wrapping
    out.append(('NOTE', s['NOTE'], 'arial', 820))
    return out


def tab_widths(lang):
    return [max(100, int(width(t, 'centaur_tab', lang) + 20 + 0.999)) for t in tab_strings.LANGS[lang]['TABS']]


def main():
    langs = sys.argv[1:] or list(tab_strings.LANGS)
    bad = 0
    for lang in langs:
        over = []
        tightest = None
        for what, text, kind, limit in checks(lang):
            w = width(text, kind, lang)
            if tightest is None or limit - w < tightest[0]:
                tightest = (limit - w, what, text, w, limit)
            if w > limit:
                over.append(f'    {what}: {text!r} {w:.0f} px > {limit} px')
        need = tab_widths(lang)
        stored = tab_strings.LANGS[lang]['TAB_NEED']
        layout = ow.tab_layout(stored)
        span = sum(w for _, w, _ in layout) - ow.TAB_OVERLAP * (len(layout) - 1)
        short = [f'{t!r} needs {n}, TAB_NEED has {s}' for t, n, s in zip(tab_strings.LANGS[lang]['TABS'], need, stored)
                 if s < n]
        tab_ok = span <= ow.TAB_ROW and not short
        print(f'{lang}: {"ok" if not over and tab_ok else "OVER"} - tightest {tightest[1]} {tightest[2]!r} '
              f'{tightest[3]:.0f}/{tightest[4]} px; tabs need {need}, laid out {[w for _, w, _ in layout]} = {span} px '
              f'of {ow.TAB_ROW}')
        for o in over + ['    tab ' + x for x in short]:
            print(o)
        bad += len(over) + (0 if tab_ok else 1)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
