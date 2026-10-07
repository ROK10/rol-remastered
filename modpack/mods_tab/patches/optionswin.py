r"""mods_tab: an eighth tab "Mods" in the game's options window (data\optionswin.xml, multiplayer_data.big), in the
game's language.

Language (0.23): the patch reads the game's language from loc.ini (LANG; the game has en de fr it es pl cz ru ja tc,
its string tables in strings.big) and writes the tab's texts in it - tab_strings.py, a literary translation that uses
the game's own words where it has them; any other language gets English. The game translates the titles it knows
(OK, Apply, Defaults, Cancel) by itself. After the game's language changes, the tab is rebuilt by re-enabling the mod
(modmgr enable mods_tab). Every text is checked against the room it gets with the game's fonts by
modkit/tools/tabfit.py (run it after any change of tab_strings.py).

The seven original tab buttons (130 px wide, centres -375..375 step 125, 5 px overlap) get widths that follow
their titles in the game's language so that eight fit into the 880 px tab row: each tab gets what its title needs
(tab_strings.py TAB_NEED, measured by tabfit.py: Centaur BHG 22 px + 20, at least 100) and the rest of the row is
shared out evenly; the 5 px overlap and the centred row are kept. In 0.1 all eight were 116 px and the Russian
"Keyboard" title lost its last letter. The new WINDOW copies the frame, fonts and bottom buttons of the Performance tab.

The engine binds options-window controls to its own settings by id_string and ignores ids it does not know
(Rol*); rolhook (ui.cpp) fills these controls when the tab opens and reads them on Apply/OK. The id_strings
below are rolhook's contract:
  RolAnisotropy     ITEM ids = anisotropy
  RolLodDetail      ITEM ids = model LOD thresholds in % of data\lod.xml: 100 as in the game, 50 / 25 = detailed
                    models 2x / 4x farther, 0 = always the full model
  RolHdTextures     on/off, for textures loaded from then on
  RolAtoc           alpha-to-coverage on/off
  RolSmoothAnim     [graphics] anim_pose_ms on/off: skeleton poses sampled every frame instead of the engine's 66 ms
                    steps, the animation clocks running evenly across the simulation's ticks
  RolShadowSize     ITEM ids = shadow map size: 0 as the game (capped at 2048 by the engine), 4096 / 8192; next start
  RolSoftShadows    ITEM ids = [graphics] soft_shadows: 0 as the game, 1 / 2 / 3 the shadow filtered over a box
                    2 / 4 / 6 shadow-map texels wide on buildings, units, grass and the ground
  RolShadowFalloff  ITEM ids = [graphics] shadow_fit: 0 the engine's trapezoid over the whole view frustum, 1 the part
                    of the map in view, 2 a layer of relief around the look-at point
  RolSsao           ITEM ids = [graphics] ssao: 0 off, 1 / 2 / 3 ambient occlusion of radius 60 / 90 / 120 world
                    units, stronger
  RolGammaLight     [graphics] gamma_light on/off: the sun's falloff on buildings and units as light summed in linear
                    space gives it
  RolSharpen        ITEM ids = [graphics] sharpen: 0 off, 1 / 2 / 3 the battle scene sharpened (CAS) before the interface
  RolVibrance       ITEM ids = [graphics] vibrance: 0 off, 1 / 2 / 3 its dull colours livened (vibrance)
  RolZoomOut        ITEM ids = [camera] zoom_out distance
  RolZoomSteps      ITEM ids = [camera] zoom_steps (a combobox since 0.21: the slider showed no number)
  RolZoomSpring     ITEM ids = [camera] zoom_spring: how far a wheel step may overshoot and spring back, 0 none,
                    1 weak, 2 medium, 3 as the game
  RolBorderless     fullscreen as a borderless window over the monitor; next start
  RolUiScale        ITEM ids = [ui] layout_scale in percent, the ui_scale mod's scaled layouts, 0 = auto (0.24: the
                    scale for the resolution the game starts at, rolhook's choice); next start
Everything not marked "next start" takes effect at once, in a running game too.

Layout (0.21, 0.22): four framed groups in a 2x2 grid over the width the Performance tab's groups take (x 29..849) -
Image, Shadows and Lighting / Camera, Window and Interface - each with two columns of 170 px comboboxes on 50 px rows
and the checkboxes under them; the legend and the defaults note below. 0.22 added Sharpening and Vibrance as a
second row of Image; its four checkboxes went into two rows of two (textures / world and motion) so that the group
stays as tall as Shadows and Lighting. Every combobox's caption is a LABEL of its own 18 px above it, not the
combobox's title: the engine draws an icon in a combobox title away from it (0.20 had two stray #ICON142 marks, one by
the Cancel button), while a LABEL draws icons inline wherever they stand in the text, as checkbox titles do. #ICON142
marks what takes effect after a restart, as on the Performance tab. Until 0.20 the tab had two tall groups, long
captions wider than their comboboxes and the rim light and specular comboboxes, both dropped with their effects.

UseStaticSplats is different: it is one of the engine's own features (persistent battlefield markings, unlocked by
the hidden_features mod). The engine binds feature checkboxes by id_string anywhere in the window and saves them in
the profile itself, so it needs no rolhook; like the Performance tab's custom options it is editable only with
"Allow Custom Options" on, and takes effect after a restart (#ICON142).
FullResTerrain ("Smoother Terrain", hidden by BHG as NOTACHECKBOX) had a checkbox here until 0.9 and is gone for
good: with it on, the terrain textures are smeared into long streaks (autotest 2026-10-03: no_fullres clean,
no_splats streaked).
"""
import os
import re
import sys

from entrypatch import insert_before, set_attrs

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tab_strings  # noqa: E402

GAME = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
BUTTONS = ['window1', 'window2', 'window3', 'window4', 'window5', 'window6', 'window7', 'window8']
TAB_ROW = 880
TAB_OVERLAP = 5
SUBTITLE = 'Rise of Legends Remastered'


def game_language():
    """tab_strings' key for loc.ini's LANG; English for a language the table lacks."""
    try:
        with open(os.path.join(GAME, 'loc.ini'), encoding='utf-8', errors='replace') as f:
            m = re.search(r'^\s*LANG\s*=\s*(\S+)', f.read(), re.M | re.I)
        lang = m.group(1).lower() if m else 'en'
    except OSError:
        lang = 'en'
    return tab_strings.ALIASES.get(lang, 'en')


def esc(text):
    return text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def tab_layout(need):
    """[(button, width, centre_x)]: every tab what its title needs plus an even share of the rest of the row, centred
    on 0, neighbours overlapping by TAB_OVERLAP."""
    extra = max(0, (TAB_ROW + TAB_OVERLAP * (len(need) - 1) - sum(need)) // len(need))
    widths = [n + extra for n in need]
    span = sum(widths) - TAB_OVERLAP * (len(widths) - 1)
    left, out = -span / 2, []
    for name, w in zip(BUTTONS, widths):
        out.append((name, w, round(left + w / 2)))
        left += w - TAB_OVERLAP
    return out


def tab_button(title, width, centre):
    return (f'      <BUTTON name="window8" id_string="Mods" title="{esc(title)}" top="0" width="{width}" height="44" '
            f'center_off_x="{centre}">\n'
            '        <FONTCOLOR r="255" g="255" b="255"/>\n'
            '        <EXPANDER id_string="up">\n'
            '          <TEX texture="GOLD_TABS2" left="0" top="0" right="75" bottom="44"/>\n'
            '          <TEX texture="GOLD_TABS2" left="76" top="0" right="151" bottom="44"/>\n'
            '          <TEX texture="GOLD_TABS2" left="152" top="0" right="227" bottom="44"/>\n'
            '        </EXPANDER>\n'
            '      </BUTTON>\n')


FRAME = '''{pad}<EXPANDER left="0" top="20" right_inverse="0" bottom_inverse="0">
{pad}  <TEX texture="HELP_BOX_BACK" left="0" top="0" right="9" bottom="9"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="10" top="0" right="118" bottom="9"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="119" top="0" right="128" bottom="9"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="0" top="10" right="9" bottom="118"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="10" top="10" right="118" bottom="118"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="119" top="10" right="128" bottom="118"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="0" top="119" right="9" bottom="128"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="10" top="119" right="118" bottom="128"/>
{pad}  <TEX texture="HELP_BOX_BACK" left="119" top="119" right="128" bottom="128"/>
{pad}  <COLOR r="255" g="255" b="200" a="150"/>
{pad}</EXPANDER>
'''

# The grid. Groups: two columns 400 wide 20 apart over x 29..849 (the Performance tab's span), two rows 20 apart.
# Inside a group: the header at y 5, the frame from y 20; combobox rows at y 50 / 100 with their captions 18 px
# above; checkboxes on a 22 px pitch; 11 px or more between the last control and the frame's 9 px border.
GROUP_W = 400
GROUP_X = (29, 449)
GROUP_Y = (80, 296)
GROUP_H = (196, 140)
COL = (20, 210)
ROW = (50, 100)
CHECK = (134, 156)   # checkbox rows under two combobox rows
COMBO_W = 170
CAPTION_H = 18
RESTART = '#ICON142'
NOTES_Y = GROUP_Y[1] + GROUP_H[1] + 15


def group(id_string, title, left, top, height, body):
    pad = ' ' * 8
    return (f'      <WINDOW id_string="{id_string}" title="{id_string}" left="{left}" top="{top}" width="{GROUP_W}" height="{height}">\n'
            f'        <STYLE MOUSE_THRU="1" CHILD="1" NO_CLIPPING="1" PASS_CONTROL_EVENTS="1"/>\n'
            + FRAME.format(pad=pad) +
            f'        <LABEL title="{esc(title)}" align="left" vert_align="top" left="5" top="5" width="{GROUP_W - 10}" height="20">\n'
            f'          <FONT title="large_button" size="18"/>\n'
            f'        </LABEL>\n'
            + body +
            f'      </WINDOW>\n')


def combo(id_string, caption, col, row, items):
    """A combobox with its caption as a LABEL above it; items = [(ITEM id, text)]."""
    left, top = COL[col], ROW[row]
    s = (f'        <LABEL title="{esc(caption)}" align="left" vert_align="top" left="{left}" top="{top - CAPTION_H}" '
         f'width="{COMBO_W + 10}" height="{CAPTION_H}"/>\n'
         f'        <COMBOBOX id_string="{id_string}" left="{left}" top="{top}" width="{COMBO_W}" height="20" '
         f'display_length="-1" min_display_length="5">\n')
    for item_id, text in items:
        s += f'          <ITEM id="{item_id}">{esc(text)}</ITEM>\n'
    return s + '        </COMBOBOX>\n'


def checkbox(id_string, title, top, engine_feature=False, col=None):
    """A checkbox across the group, or in one column of it (col 0 / 1)."""
    grey = ' grey_text_disabled="1"' if engine_feature else ''
    left, width = (COL[0], 360) if col is None else (COL[col], COL[1] - COL[0] - 10 if col == 0 else GROUP_W - 9 - COL[1])
    return (f'        <CHECKBOX id_string="{id_string}" title="{esc(title)}" left="{left}" top="{top}" width="{width}" height="20"{grey}>\n'
            f'          <STYLE NO_CLIPPING="1"/>\n'
            f'        </CHECKBOX>\n')


def levels(s, off_key, lv_key):
    return [(0, s[off_key])] + list(zip((1, 2, 3), s[lv_key]))


def note(text, top, height):
    return (f'      <LABEL title="{esc(text)}" align="left" vert_align="top" left="{GROUP_X[0]}" top="{top}" width="820" height="{height}">\n'
            f'        <FONTCOLOR r="140" g="140" b="140"/>\n'
            f'      </LABEL>\n')


BOTTOM_BUTTONS = '''      <BUTTON id_string="Accept" title="OK" width="210" height="44" center_off_x="-315" center_off_y="510">
        <EXPANDER id_string="up">
          <TEX texture="GOLD_BUTTONS" left="0" top="0" right="75" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="76" top="0" right="151" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="152" top="0" right="227" bottom="44"/>
        </EXPANDER>
      </BUTTON>
      <BUTTON id_string="Apply" title="Apply" width="210" height="44" center_off_x="-105" center_off_y="510">
        <EXPANDER id_string="up">
          <TEX texture="GOLD_BUTTONS" left="0" top="0" right="75" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="76" top="0" right="151" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="152" top="0" right="227" bottom="44"/>
        </EXPANDER>
      </BUTTON>
      <BUTTON id_string="reset" title="Defaults" width="210" height="44" center_off_x="105" center_off_y="510">
        <EXPANDER id_string="up">
          <TEX texture="GOLD_BUTTONS" left="0" top="0" right="75" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="76" top="0" right="151" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="152" top="0" right="227" bottom="44"/>
        </EXPANDER>
      </BUTTON>
      <BUTTON id_string="Cancel" title="Cancel" width="210" height="44" center_off_x="315" center_off_y="510">
        <EXPANDER id_string="up">
          <TEX texture="GOLD_BUTTONS" left="0" top="0" right="75" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="76" top="0" right="151" bottom="44"/>
          <TEX texture="GOLD_BUTTONS" left="152" top="0" right="227" bottom="44"/>
        </EXPANDER>
      </BUTTON>
'''


def mods_window(s):
    """The tab's WINDOW with the texts of one language (a tab_strings.LANGS entry)."""
    orig = s['ORIGINAL'].format
    image = (
        combo('RolAnisotropy', s['C_ANISO'], 0, 0,
              [(0, s['OFF_ANISO']), (2, '2x'), (4, '4x'), (8, '8x'), (16, '16x')])
        + combo('RolLodDetail', s['C_LOD'], 1, 0, list(zip((100, 50, 25, 0), s['LOD'])))
        + combo('RolSharpen', s['C_SHARPEN'], 0, 1, levels(s, 'OFF_SHARPEN', 'LV_SHARPEN'))
        + combo('RolVibrance', s['C_VIBRANCE'], 1, 1, levels(s, 'OFF_VIBRANCE', 'LV_VIBRANCE'))
        + checkbox('RolHdTextures', s['K_HD'], CHECK[0], col=0)
        + checkbox('RolAtoc', s['K_ATOC'], CHECK[0], col=1)
        + checkbox('UseStaticSplats', s['K_SPLATS'] + RESTART, CHECK[1], engine_feature=True, col=0)
        + checkbox('RolSmoothAnim', s['K_ANIM'], CHECK[1], col=1)
    )
    shadows = (
        combo('RolShadowSize', s['C_SHADOW_SIZE'] + RESTART, 0, 0,
              [(0, orig('2048')), (4096, '4096'), (8192, '8192')])
        + combo('RolSoftShadows', s['C_SOFT'], 1, 0, levels(s, 'OFF_SOFT', 'LV_SOFT'))
        + combo('RolShadowFalloff', s['C_FIT'], 0, 1, list(zip((0, 1, 2), s['FIT'])))
        + combo('RolSsao', s['C_SSAO'], 1, 1, levels(s, 'OFF_SSAO', 'LV_SSAO'))
        + checkbox('RolGammaLight', s['K_GAMMA'], CHECK[0])
    )
    camera = (
        combo('RolZoomOut', s['C_ZOOM_OUT'], 0, 0,
              [(7500, orig('7500')), (9000, '9000'), (11000, '11000'), (14000, '14000')])
        + combo('RolZoomSteps', s['C_ZOOM_STEPS'], 1, 0,
                [(8, orig('8')), (10, '10'), (12, '12'), (14, '14'), (16, '16')])
        + combo('RolZoomSpring', s['C_SPRING'], 0, 1, list(zip((3, 2, 1, 0), s['SPRING'])))
    )
    pct = s['PERCENT'].format
    window_ui = (
        checkbox('RolBorderless', s['K_BORDERLESS'] + RESTART, ROW[0] - CAPTION_H)
        + combo('RolUiScale', s['C_UI_SCALE'] + RESTART, 0, 1,
                [(0, s['AUTO']), (100, orig(pct(100)))] + [(p, pct(p)) for p in (125, 150, 175, 200)])
    )
    return (
        '    <WINDOW id_string="Mods" title="Mods" width="880" height="660" center_off_x="0" center_off_y="90">\n'
        '      <STYLE MOUSE_THRU="1" CHILD="1" NO_CLIPPING="1" PASS_CONTROL_EVENTS="1"/>\n'
        '      <TEXLOAD id="HELP_BOX_BACK" file=".\\art\\interface\\help_box_back.tga"/>\n'
        f'      <LABEL title="{esc(s["TITLE"])}" align="left" vert_align="top" top="5" width="850" height="40" center_off_x="0">\n'
        '        <FONT title="large_button" size="26"/>\n'
        '      </LABEL>\n'
        f'      <LABEL title="{esc(SUBTITLE)}" align="left" vert_align="top" left="15" top="45" width="450" height="30">\n'
        '        <FONT title="large_button" size="18"/>\n'
        '      </LABEL>\n'
        + group('Mods Image', s['G_IMAGE'], GROUP_X[0], GROUP_Y[0], GROUP_H[0], image)
        + group('Mods Shadows', s['G_SHADOWS'], GROUP_X[1], GROUP_Y[0], GROUP_H[0], shadows)
        + group('Mods Camera', s['G_CAMERA'], GROUP_X[0], GROUP_Y[1], GROUP_H[1], camera)
        + group('Mods Window', s['G_WINDOW'], GROUP_X[1], GROUP_Y[1], GROUP_H[1], window_ui)
        + note(s['LEGEND'].format(RESTART), NOTES_Y, 40)
        + note(s['NOTE'], NOTES_Y + 42, 22)
        + BOTTOM_BUTTONS +
        '    </WINDOW>\n'
    )


def patch(text, lang=None):
    s = tab_strings.LANGS[lang or game_language()]
    layout = tab_layout(s['TAB_NEED'])
    for name, width, centre in layout[:7]:
        text = set_attrs(text, 'BUTTON', {'name': name}, {'width': width, 'center_off_x': centre})
    text = insert_before(text, '    </TABWIN>', tab_button(s['TABS'][7], layout[7][1], layout[7][2]))
    text = insert_before(text, '    <ANIMATION event="show" action="fade_in" id="parent"', mods_window(s))
    return text
