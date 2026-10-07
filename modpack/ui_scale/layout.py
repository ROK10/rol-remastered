r"""ui_scale: the interface laid out at a bigger scale, by scaling the layout data itself.

The engine lays its windows out in screen pixels from XML (ui.big, multiplayer_data.big): left/top/width/height,
the *_inverse distances from the parent's right/bottom edge, center_off_x/y and a few more, all integers. At
3440x1440 those designs for 1024x768 come out tiny. Multiplying every pixel attribute by the scale lays the same
interface out bigger on the real screen: the mouse, the 3D view and the renderer stay untouched (the 0.3 attempt
to give the whole engine a smaller virtual screen broke the game window and the terrain passes).

Besides the rectangles, the windows' own spacing attributes are pixels too (PIXEL_ATTRS: the build panel's second
row, gaps between buttons, insets, the widths of the bars over units...). The campaign map's windows live in the
campaign files (CAMPAIGN_FILES, their <INTERFACE> section).

Left alone:
  TEX         rectangles in texture atlases (the picture to draw, not where)
  STYLE, colours, *percent*   flags, colours and fractions of the parent
  -4321       the engine's "not set" value (0xEF1F as int16), written out in some files
  FONT size   fonts are asked for by nominal size (here, in fonts.xml lookups and in many constants of the code);
              rolhook rasterizes every font of fonts.xml at the scale instead (native/rolhook/uilayout.cpp), so all
              text grows alike and stays sharp

build.py makes the scaled variants; rolhook serves the one of the chosen scale ([ui] layout_scale).
"""
import math
import re
import struct

# Gameplay interface files: (archive, entry). Editors and developer tools are left alone.
FILES = [('ui.big', '.\\ui\\' + f) for f in (
    'uimain.xml', 'topmenu.xml', 'launchwin.xml', 'gamesetupwin.xml', 'uimultiplayer.xml', 'simplemodalwins.xml',
    'gamemodalwindow.xml', 'endgamewin.xml', 'chathistory.xml', 'tutorialarrows.xml', 'privacy_win.xml',
    'profile_picker.xml', 'patchmgr.xml', 'gamescenestatwin.xml', 'filebrowser.xml', 'stylesheets\\default.xml',
    'stylesheets\\gold_buttons.xml', 'modifiers\\binksubtitles.xml')] + \
    [('multiplayer_data.big', '.\\data\\' + f) for f in (
        'interface.xml', 'optionswin.xml', 'achievewin.xml', 'ctw.xml', 'questwin.xml', 'auto_help.xml',
        'aboutbox.xml', 'castwinpopup.xml', 'gameinputwindow.xml', 'loadwin.xml', 'warning_popup.xml', 'widgets.xml',
        'colorpicker.xml')]

PIXEL_ATTRS = {'left', 'top', 'right', 'bottom', 'width', 'height', 'left_inverse', 'top_inverse', 'right_inverse',
               'bottom_inverse', 'center_off_x', 'center_off_y', 'x', 'y', 'scrollable_width', 'scrollable_height',
               'scroll_pad_left', 'scroll_pad_right', 'scroll_pad_top', 'scroll_pad_bottom', 'entry_height',
               'border_thickness', 'scroll_thickness', 'break_width', 'break_padding', 'empty_line_padding',
               # window-specific spacing (uimain.xml unless noted)
               'normal_mid_button_top', 'upgrade_mid_button_top',          # OPTIONWIN: where the second row goes
               'horz_buffer', 'vert_buffer', 'space_between_entries',       # OPTIONWINPULLDOWN
               'x_distance_between_buttons', 'y_distance_between_buttons',  # HEROESWIN, CONTROLGROUPWIN
               'multi_spacing_x', 'multi_spacing_y',                        # GARRISONWIN
               'icon_pressed_x_offset', 'icon_pressed_y_offset',            # SELECTEDWIN
               'buffer_top_bottom', 'buffer_left_right',                    # SELECTEDINFOWIN
               'buffer_horz_between_stats', 'buffer_vert_between_stats',
               'distance_between_entries',                                  # TIMERSWIN
               'dist_to_right_edge',                                        # TASKLISTWIN
               'inset_text_x', 'inset_text_y',                              # tutorialarrows.xml
               'button_spacing',                                            # interface.xml MAINMENU
               'label_spacing', 'legend_spacing', 'line_thickness',         # achievewin.xml
               'portrait_shift', 'node_offset',                             # ctw.xml
               'vertical_inset', 'horizontal_inset', 'max_bar_width',       # gameinputwindow.xml: bars over units
               'buffer'}                                                    # SLIDER (widgets.xml)
PIXEL_PATTERN = re.compile(r'(build|unit)_hp_width\d+')   # uimain.xml CONSTANTS: widths of the bars over units

# The campaign map's windows are not in the archives: every campaign file (loose XML under campaigns\, opened through
# the same file overlay) carries its own in an <INTERFACE> section - main_ui (the panel at the bottom right),
# mouseover_node (the territory window), the army, invention, city and hero windows, the RECTs and POINTs the
# campaign scripts place things by. Only that section is layout; the rest (map pieces, camera views, events) is game
# data and stays as it is.
CAMPAIGN_FILES = ('campaigns\\Vinci\\vinci.campaign', 'campaigns\\Alim\\alim.campaign',
                  'campaigns\\CUOTL\\cuotl.campaign')
INTERFACE = re.compile(r'<INTERFACE\b.*?</INTERFACE>', re.S)

# The campaign scripts lay those windows out partly by pixel constants of their own (scripts.big, compiled; the
# sources are in ModPack.exe). Everything they measure comes from the engine, scaled; these do not:
#   245        sf_update_mouseover_node: default_h, the least height of the territory window (the script fits the
#              window to its text, and at 245 a scaled window shrank below its own layout: the last line on the frame)
#   100, 170   sf_update_city_child: arrow_width, arrow_height - the city window's offset from its territory
#   20         sf_update_army_win: the padding of the "nofit" box around its text
#   40, 7      sf_update_army_bucket_child: block_height and grid_y_nudge - the army grid's frame fitted to its rows
#   40, 3      sf_fill_army_grid (campaign_common): the grid's blocks and nudge_x, where the unit pictures go (rolhook
#              draws those pictures - add_sprite, natural size - magnified when the scaled layouts are in use; the
#              grid's frame and lines are a nine-piece expander, magnified as well, its border 3 px times the scale)
# A compiled script keeps its constants in a pool, one entry per value (pool()); the bytecode pushes one by its index
# (28 <i32>). A value can serve unrelated code too - 20 is also set_offset_parse_strings' first argument, the $STR
# number the hero window's help starts its values at - so a pool entry is never changed: each push site gets the
# index of an entry with the scaled value (appended to the pool when the script has none). A site is named by the
# int it pushes and the string constants the bytecode pushes nearest before and after it, with the number of such
# pushes; the build stops when a script does not match exactly.
SCRIPT_ARCHIVE = 'scripts.big'
_UI_SITES = ((245, 'mouseover_node', 'nodes_won', 1), (100, 'left', 'arrow_', 1), (170, 'left', 'arrow_', 1),
             (20, 'nofit', 'nofit', 1), (40, '_allowed', 'army_bucket_', 2), (7, '_allowed', 'army_bucket_', 1))
SCRIPT_SITES = {
    '.\\campaigns\\vinci\\vinci_ui.bhs': _UI_SITES,
    '.\\campaigns\\alim\\alim_ui.bhs': _UI_SITES,
    '.\\campaigns\\cuotl\\cuotl_ui.bhs': _UI_SITES,
    '.\\campaigns\\campaign_common.bhs': ((40, 'selected_army_spot_id', 'army_bucket_', 2),
                                          (3, 'selected_army_spot_id', 'army_bucket_', 1)),
}
POOL_ENTRY = re.compile(rb'\xf1\x32\x4d\x00(.{4})\xf1\x32\x4d\x00\1', re.S)
POOL_TYPES = {bytes.fromhex('ad7b0500'): 'int', bytes.fromhex('74811600'): 'str', bytes.fromhex('39560a00'): 'text'}
SKIP_TAGS = {'TEX', 'STYLE', 'COLOR', 'FONTCOLOR', 'FONTSHADOW', 'BACK_COLOR', 'TEXT_COLOR', 'TEXLOAD'}
NOT_SET = -4321

START = re.compile(r'<([A-Za-z_][\w.-]*)((?:\s+[\w:.-]+\s*=\s*"[^"]*")*)(\s*/?>)')
ATTR = re.compile(r'([\w:.-]+)(\s*=\s*")([^"]*)(")')
INT = re.compile(r'-?\d+')


def scaled(v, pct):
    """v * pct / 100 rounded half away from zero (rolhook's font sizes use the same rounding)."""
    if v == NOT_SET:
        return v
    r = math.floor(abs(v) * pct / 100 + 0.5)
    return -r if v < 0 else r


def is_pixel(key):
    return key in PIXEL_ATTRS or PIXEL_PATTERN.fullmatch(key) is not None


def scale_layout(text, pct):
    """Scale every pixel attribute of the layout tags in an XML text; everything else stays byte-identical."""
    if pct == 100:
        return text

    def tag(m):
        if m.group(1).upper() in SKIP_TAGS:
            return m.group(0)

        def attr(a):
            if is_pixel(a.group(1).lower()) and INT.fullmatch(a.group(3)):
                return a.group(1) + a.group(2) + str(scaled(int(a.group(3)), pct)) + a.group(4)
            return a.group(0)

        return '<' + m.group(1) + ATTR.sub(attr, m.group(2)) + m.group(3)

    return START.sub(tag, text)


def scale_campaign(text, pct):
    """A campaign file with its <INTERFACE> section scaled (scale_layout); everything else byte-identical."""
    if len(INTERFACE.findall(text)) != 1:
        raise ValueError('expected one <INTERFACE> section')
    return INTERFACE.sub(lambda m: scale_layout(m.group(0), pct), text)


def _pool(data):
    """(entries, offset of the count, offset where the last entry ends). The pool follows the bytecode: a vector
    {u32 count, u32 capacity, ff ff, 00}, then per entry {u32 0, f1 32 4d 00, T, f1 32 4d 00, T, payload} - an int's
    payload its i32, a string's (T 74 81 16 00) or a localizable text's (39 56 0a 00) u32 length and UTF-16."""
    first = min(m.start() for m in POOL_ENTRY.finditer(data) if m.group(1) in POOL_TYPES)
    at = first - 15
    count, _, marker, zero = struct.unpack_from('<IIHB', data, at)
    if marker != 0xFFFF or zero or data[at + 11:first] != b'\0' * 4:
        raise ValueError(f'constant pool: unexpected header at {at:#x}')
    out, i = [], first
    for _ in range(count):
        m = POOL_ENTRY.match(data, i)
        if not m or m.group(1) not in POOL_TYPES:
            raise ValueError(f'constant pool: no entry at {i:#x}')
        kind, j = POOL_TYPES[m.group(1)], m.end()
        if kind == 'int':
            out.append(('int', j, struct.unpack_from('<i', data, j)[0]))
            i = j + 4
        else:
            n = struct.unpack_from('<I', data, j)[0]
            out.append((kind, i, data[j + 4:j + 4 + 2 * n].decode('utf-16-le')))
            i = j + 4 + 2 * n
        i += 4   # the next entry's leading u32 0
    return out, at, i - 4


def pool(data):
    """The constant pool of a compiled script, by index (the bytecode's 28 <i32 index>): ('int', offset of the
    value, value), ('str', offset of the entry, text) and ('text', ...) for localizable text."""
    return _pool(data)[0]


def code_end(data):
    """Where the bytecode ends: the constant pool's count."""
    return _pool(data)[1]


def const_refs(data, index):
    """Offsets of the i32 operands of the bytecode's pushes of constant `index` (28 <i32>)."""
    pat, end, out, i = b'\x28' + struct.pack('<i', index), code_end(data), [], 0
    while (i := data.find(pat, i, end)) >= 0:
        out.append(i + 1)
        i += 1
    return out


def add_int(data, value):
    """(script with an int appended to its constant pool, the new entry's index). The bytecode precedes the pool,
    so its offsets stay; the count grows (and the capacity, when full)."""
    entries, at, end = _pool(data)
    count, cap = len(entries) + 1, struct.unpack_from('<I', data, at + 4)[0]
    out = bytearray(data[:end] + b'\0' * 4 + bytes.fromhex('f1324d00ad7b0500f1324d00ad7b0500') +
                    struct.pack('<i', value) + data[end:])
    struct.pack_into('<II', out, at, count, max(cap, 1 << (count - 1).bit_length()))
    return bytes(out), count - 1


def push_sites(data, value, before, after):
    """Operand offsets of the pushes of int `value` whose nearest pushed strings before and after are `before`
    and `after` (a push of a string or text is 28 <index of one>; the scan takes every such byte pattern)."""
    entries = pool(data)
    end = code_end(data)
    strings = []
    i = 0
    while (i := data.find(b'\x28', i, end)) >= 0:
        k = struct.unpack_from('<i', data, i + 1)[0]
        if 0 <= k < len(entries) and entries[k][0] != 'int':
            strings.append((i + 1, entries[k][2]))
        i += 1
    out = []
    for k, (kind, _, v) in enumerate(entries):
        if kind != 'int' or v != value:
            continue
        for site in const_refs(data, k):
            prev = next((s for o, s in reversed(strings) if o < site), None)
            nxt = next((s for o, s in strings if o > site), None)
            if prev == before and nxt == after:
                out.append(site)
    return out


def scale_script(data, sites, pct):
    """A compiled campaign script with its pixel constants' push sites (SCRIPT_SITES) pointed at the scaled values."""
    out = bytearray(data)
    for value, before, after, n in sites:
        offs = push_sites(bytes(out), value, before, after)
        if len(offs) != n:
            raise ValueError(f'{value} pushed between "{before}" and "{after}": {len(offs)} sites, expected {n}')
        want = scaled(value, pct)
        k = next((k for k, (kind, _, v) in enumerate(pool(bytes(out))) if kind == 'int' and v == want), None)
        if k is None:
            data2, k = add_int(bytes(out), want)
            out = bytearray(data2)
        for o in offs:
            struct.pack_into('<i', out, o, k)
    return bytes(out)
