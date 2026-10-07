r"""Automated test runs of the game: straight into a fixed quick battle, screenshots, the game ends by itself.

  py -3.14 modkit/tools/autotest.py current no_hd        one game per case, in this order
  py -3.14 modkit/tools/autotest.py --list               the cases
  py -3.14 modkit/tools/autotest.py --dry current        what would run, without starting the game

How it works
  * The battle: tests/autotest/battle.gamesetup.in (Random Small Map with a fixed seed, the map revealed, one easy
    AI) with the current profile's player name -> work/autotest/battle.gamesetup. legends.exe takes a .gamesetup on
    its command line and goes straight into the battle (the path of opening such a file from Explorer:
    0x00B0A550 -> 0x00BDDD60, app state 0x20). The path is relative and has no spaces: the command line is split at
    spaces.
  * rolhook reads one more config layer, named by the ROLHOOK_EXTRA_INI environment variable of the started game:
    work/autotest/run.ini = the [test] block below + the case's overrides. The installed files stay as they are.
  * Afterwards the session's shots, frame captures and logs go to work/autotest/<stamp>/<case>/. The profile files
    the game may write (last_game.gamesetup, the .profile, personal.ini) are put back as they were, the replay of
    the test game is moved out of "Recorded Games" into the case folder. sheet.png puts every case's shots side by
    side.

It refuses to start while the game or the HD pass (Flux needs the GPU) is running.
"""
import argparse
import configparser
import ctypes
import datetime
import filecmp
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time

KIT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.dirname(KIT)
EXE = os.path.join(GAME, 'legends.exe')
TEMPLATE = os.path.join(KIT, 'tests', 'autotest', 'battle.gamesetup.in')
WORK = os.path.join(KIT, 'work', 'autotest')
SETUP = os.path.join(WORK, 'battle.gamesetup')
RUN_INI = os.path.join(WORK, 'run.ini')
SESSIONS = os.path.join(KIT, 'logs', 'sessions')
HD_LOCK = os.path.join(KIT, 'work', 'hd_full', 'RUNNING')
MYGAMES = os.path.join(os.path.expanduser('~'), 'Documents', 'My Games', 'Rise Of Legends')

# Every case: the match is on screen -> shots at 6 and 12 s with a frame capture each -> the game ends at 13 s.
TEST_BLOCK = {
    'test': {'start': 'match', 'shots': '6, 12', 'capture': '1', 'quit': '13', 'timeout': '120'},
    'levels': {'textures': 'debug'},
}

PRISTINE = 'data.big, multiplayer_data.big'
CASES = {
    'current': ('everything as installed', {}),
    'no_hd': ('HD texture override off', {'textures': {'override': '0'}}),
    'pristine': ('original data.big and multiplayer_data.big, i.e. without the archive patches of the mods '
                 '(hidden_features, camera_zoom, mods_tab)', {'test': {'pristine_bigs': PRISTINE}}),
    'shaders': ('everything as installed, every shader the effects create disassembled (session folder shaders)',
                {'debug': {'dump_shaders': '1'}}),
    'soft_shadows': ('soft shadows medium (a box 4 texels wide), the copies\' assembly dumped (session shaders)',
                     {'graphics': {'soft_shadows': '2'}, 'debug': {'dump_shaders': '1'}}),
    'soft_shadows_strong': ('soft shadows strong (a box 6 texels wide: the most taps)', {'graphics': {'soft_shadows': '3'}}),
    'soft_shadows_off': ('soft shadows off (the one-tap test, as the game)', {'graphics': {'soft_shadows': '0'}}),
    'ssao': ('ambient occlusion medium (radius 90), GPU timing, the AO read back once (ssao probe)',
             {'graphics': {'ssao': '2'}, 'debug': {'gpu_timing': '1', 'ssao_probe': '1'}}),
    'ssao_strong': ('ambient occlusion strong (radius 120), GPU timing', {'graphics': {'ssao': '3'}, 'debug': {'gpu_timing': '1'}}),
    'gamma': ('gamma-correct light on buildings and units; the copies\' assembly dumped (session shaders)',
              {'graphics': {'gamma_light': '1'}, 'debug': {'dump_shaders': '1'}}),
    'gamma_off': ('the light on buildings and units as the game (summed in gamma space)',
                  {'graphics': {'gamma_light': '0'}}),
    'post': ('sharpening and vibrance medium (one pass at the scene\'s end), GPU timing',
             {'graphics': {'sharpen': '2', 'vibrance': '2'}, 'debug': {'gpu_timing': '1'}}),
    'post_off': ('sharpening and vibrance off, GPU timing (the scene as the game draws it)',
                 {'graphics': {'sharpen': '0', 'vibrance': '0'}, 'debug': {'gpu_timing': '1'}}),
    'ssao_off': ('ambient occlusion off, GPU timing (the scene as the game draws it)',
                 {'graphics': {'ssao': '0'}, 'debug': {'gpu_timing': '1'}}),
    'no_aniso': ('anisotropic filtering off', {'graphics': {'anisotropy': '0'}}),
    'no_atoc': ('alpha-to-coverage off', {'graphics': {'atoc': '0'}}),
    'bare': ('every rolhook feature off and the original archives: the closest to the unmodded game',
             {'textures': {'override': '0'},
              'graphics': {'anisotropy': '0', 'atoc': '0', 'lod_thresholds': '100', 'borderless': '0',
                           'shadow_size': '0', 'shadow_fit': '0', 'anim_pose_ms': '0', 'soft_shadows': '0', 'ssao': '0',
                           'gamma_light': '0', 'sharpen': '0', 'vibrance': '0'},
              'camera': {'zoom_in': '0', 'zoom_out': '0', 'zoom_steps': '0', 'zoom_spring': '3'},
              'ui': {'scale': '100', 'layout_scale': '100'},
              'test': {'pristine_bigs': PRISTINE}}),
    'no_hidden': ('profile: the hidden features FullResTerrain and UseStaticSplats off', {}),
    'no_fullres': ('profile: FullResTerrain off', {}),
    'no_splats': ('profile: UseStaticSplats off', {}),
    'borderless_focus': ('borderless: Alt-Tab away - the cursor must be free to leave for another monitor',
                         {'graphics': {'borderless': '1'}, 'test': {'shots': '6, 16', 'quit': '22'}}),
}

# Close camera for looking at shadows and texture detail: mouse wheel notches posted to the game window at 1 s of the
# test clock (rolhook [test] wheel), shots once the camera has settled.
CLOSE = {'test': {'wheel': '1:6', 'shots': '6, 12'}}
for _size in (2048, 4096, 8192):
    _g = {} if _size == 2048 else {'shadow_size': str(_size)}
    CASES[f'shadow_{_size}'] = (f'shadow map {_size}, close camera', dict(CLOSE, graphics=dict(_g)))
    CASES[f'shadow_{_size}_lin'] = (f'shadow map {_size} with LINEAR filtering, close camera',
                                    dict(CLOSE, graphics=dict(_g, shadow_filter='linear')))
# One game, three zoom levels (close / as the battle starts / far), a shot and a frame capture at each: how the
# shadow pass changes with the camera distance (its viewport, the light's projection in c0-c1).
# Zoom sweep: closest, then 2 notches farther every 3 s, a shot and a frame capture at each step (8192).
CASES['shadow_sweep'] = ('shadow map 8192, zoom sweep from the closest to the farthest in 2-notch steps',
                         {'graphics': {'shadow_size': '8192', 'shadow_filter': 'linear'},
                          'test': {'wheel': '1:14, 4:-2, 7:-2, 10:-2, 13:-2, 16:-2, 19:-2, 22:-4',
                                   'shots': '3, 6, 9, 12, 15, 18, 21, 25', 'quit': '26'}})
# The card's limit: 16384, closest and farthest zoom (quality and cost).
CASES['shadow_16k'] = ('shadow map 16384 (the limit of the card), closest and farthest zoom',
                       {'graphics': {'shadow_size': '16384', 'shadow_filter': 'linear'},
                        'test': {'wheel': '1:14, 5:-16', 'shots': '4, 9', 'quit': '10'}})
# The shadow map fit ([graphics] shadow_fit 0 / 1 / 2) through a zoom sweep: core.log lists the fit at each zoom
# (texel size, texels per area against the engine's trapezoid), the frame captures its matrices and the map's box.
for _lvl in ('0', '1', '2'):
    CASES[f'shadow_fit_{_lvl}'] = (f'shadow map 8192, shadow_fit {_lvl}, zoom sweep from the closest to the farthest',
                                   {'graphics': {'shadow_size': '8192', 'shadow_filter': 'linear', 'shadow_fit': _lvl},
                                    'test': {'wheel': '1:14, 4:-2, 7:-2, 10:-2, 13:-2, 16:-2, 19:-2, 22:-4',
                                             'shots': '3, 6, 9, 12, 15, 18, 21, 25', 'quit': '26'}})
CASES['shadow_zoom'] = ('shadow map 8192 at three zoom levels: close, start, far',
                        {'graphics': {'shadow_size': '8192', 'shadow_filter': 'linear'},
                         'test': {'wheel': '1:6, 5:-6, 9:-12', 'shots': '4, 8, 12', 'quit': '13'}})
# Interface scale and the camera at the zoom limits: ui.log gets the zoom state at every change ([debug] log_camera)
# while the wheel goes out one notch at a time past the farthest zoom, stays there, comes in, goes out again and then
# all the way in past the closest zoom; the interface scale lines (fonts rasterized at the scale, sprites pulled
# inside their atlas cells) come with it.
_NOTCHES = lambda t0, n, d: ', '.join(f'{t0 + 0.25 * k:g}:{d}' for k in range(n))   # n notches, 4 per second
_LIMIT = {'debug': {'log_camera': '1'},
          'test': {'wheel': f'{_NOTCHES(1, 10, -1)}, {_NOTCHES(6, 3, -1)}, 9:3, {_NOTCHES(11, 5, -1)}, '
                            f'{_NOTCHES(14, 14, 1)}',
                   'shots': '5, 8, 10, 13, 19', 'quit': '20'}}
CASES['ui_zoom_limit'] = ('installed interface scale; zoom state logged out past the farthest zoom and back', _LIMIT)
CASES['ui_zoom_limit_100'] = ('interface scale 100 (as the game); zoom state logged out past the farthest zoom',
                              dict(_LIMIT, ui={'layout_scale': '100'}))
# The zoom's spring back after a wheel step ([camera] zoom_spring 0 none .. 3 as the game) over the same wheel
# schedule: ui.log has the zoom state of every frame and a line for every step whose carried speed was cut.
for _lvl in ('0', '1', '2', '3'):
    CASES[f'zoom_spring_{_lvl}'] = (f'zoom spring {_lvl} (0 none, 1 weak, 2 medium, 3 as the game), same wheel '
                                    f'schedule as ui_zoom_limit', dict(_LIMIT, camera={'zoom_spring': _lvl}))

# Interface pictures at two scales: ui.log lists every distinct picture drawn ([debug] log_sprites) - texture, texel
# rectangle, screen size; tools/ui_sprites.py lines the two games up and lists what kept its size at 150 %.
CASES['ui_sprites'] = ('installed interface scale, every distinct interface picture logged',
                       {'debug': {'log_sprites': '1'}})
CASES['ui_sprites_100'] = ('interface scale 100, every distinct interface picture logged',
                           {'debug': {'log_sprites': '1'}, 'ui': {'layout_scale': '100'}})
CASES['ui_sprites_200'] = ('interface scale 200, every distinct interface picture logged',
                           {'debug': {'log_sprites': '1'}, 'ui': {'layout_scale': '200'}})
# Mod page shots, before / after: the same battle and camera without the modpack (as bare), with it at its defaults
# (the Mods tab's choices in zz_options.ini set aside: 'mod_defaults') and with it as installed - all on the game's
# own zoom ladder (2000-7500 in 8 steps; the modpack's range would put the same wheel notches at other distances):
# all the way in, 4 notches out, all the way out - a close, a middle and a far shot.
_PROMO = {'wheel': '2:12, 7:-4, 12:-12', 'shots': '6, 11, 16', 'quit': '17', 'capture': '0'}
_LADDER = {'zoom_in': '2000', 'zoom_out': '7500', 'zoom_steps': '8'}
CASES['promo_before'] = ('mod page shots without the modpack (as bare): close, middle and far zoom',
                         dict(CASES['bare'][1], test=dict(CASES['bare'][1]['test'], **_PROMO)))
CASES['promo_after'] = ('mod page shots with the modpack at its defaults, on the game\'s zoom ladder: close, middle, far',
                        {'mod_defaults': True, 'camera': dict(_LADDER), 'test': dict(_PROMO)})
CASES['promo_mine'] = ('mod page shots with the modpack as installed (the Mods tab\'s choices too), on the game\'s zoom '
                       'ladder: close, middle, far', {'camera': dict(_LADDER), 'test': dict(_PROMO)})
# Trees close up, before / after: the camera all the way out, then four times 250 ms of the Left arrow (the trees west
# of the city; rolhook [test] keys), a far shot, all the way in, a close shot, all the way out again - four positions
# with a far and a close shot each, the same in every run; tools/compare.py lines a pair up and crops it.
_TREES = {'wheel': '1:-12, 6:12, 11:-12, 16:12, 21:-12, 26:12, 31:-12, 36:12',
          'keys': '3:Left:250, 13:Left:250, 23:Left:250, 33:Left:250',
          'shots': '5, 10, 15, 20, 25, 30, 35, 40', 'quit': '41', 'capture': '0'}
CASES['trees_before'] = ('trees close up without the modpack (as bare): four steps west, a far and a close shot at each',
                         dict(CASES['bare'][1], test=dict(CASES['bare'][1]['test'], **_TREES)))
CASES['trees_after'] = ('trees close up with the modpack at its defaults: four steps west, a far and a close shot at each',
                        {'mod_defaults': True, 'camera': dict(_LADDER), 'test': dict(_TREES)})
CASES['trees_mine'] = ('trees close up with the modpack as installed: four steps west, a far and a close shot at each',
                       {'camera': dict(_LADDER), 'test': dict(_TREES)})
# The Alin's fire scout (the "phoenix") from the start, recorded (ffmpeg ddagrab, 50 fps; 'record') for a before / after
# GIF of the smooth animation (tools/compare.py --video): the player as the Alin ('tribe' 0), the camera all the way
# in and still for 9 s, then 3 notches out and still for 8 s. Borderless in both: the desktop capture sees a window,
# an exclusive full screen may stay black to it.
_PHOENIX = {'wheel': '1:12, 11:-3', 'shots': '4, 14', 'quit': '20', 'capture': '0'}
CASES['phoenix_before'] = ('the Alin fire scout recorded without the modpack (as bare): close, then 3 notches out',
                           dict(CASES['bare'][1], graphics=dict(CASES['bare'][1]['graphics'], borderless='1'),
                                test=dict(CASES['bare'][1]['test'], **_PHOENIX), tribe=0, record=True))
CASES['phoenix_after'] = ('the Alin fire scout recorded with the modpack at its defaults: close, then 3 notches out',
                          {'mod_defaults': True, 'camera': dict(_LADDER), 'graphics': {'borderless': '1'},
                           'test': dict(_PHOENIX), 'tribe': 0, 'record': True})
# Landscapes for the mod page: the template's "(2)Random Small Map" is a list of real maps and its map_submap_id picks
# one ('submap'; the setup's checksum is the list file's, so it stays valid), the player as a nation at home there;
# the promo camera (close, middle, far on the game's zoom ladder) around the player's city, without the modpack and
# with it at its defaults.
RANDOM_SMALL = ["Summer's Heat", 'Spire Forest', 'Corbanile Mines', 'Vents', 'Nohoch Crater', 'Burial Grounds',
                'Rain Flats', 'Valley of Lost Souls', 'Miana Reservoir', 'Frozen Standoff', 'Kalahese Desert',
                'Umberto Lowlands']
LANDSCAPES = {'desert': (10, 0), 'crater': (4, 2), 'snow': (9, 3), 'forest': (1, 2)}   # name: (submap, tribe)
for _name, (_sub, _tribe) in LANDSCAPES.items():
    CASES[f'land_{_name}_before'] = (f'{RANDOM_SMALL[_sub]}, tribe {_tribe}, without the modpack: close, middle, far',
                                     dict(CASES['bare'][1], test=dict(CASES['bare'][1]['test'], **_PROMO),
                                          tribe=_tribe, submap=_sub))
    CASES[f'land_{_name}_after'] = (f'{RANDOM_SMALL[_sub]}, tribe {_tribe}, the modpack at its defaults: close, middle, '
                                    f'far', {'mod_defaults': True, 'camera': dict(_LADDER), 'test': dict(_PROMO),
                                             'tribe': _tribe, 'submap': _sub})
# The players' build of rolhook (build/native/player/ddraw.dll, what the release ships; no developer tools, so no
# [test] shots or quit): put in the game's folder for the run in place of the full build (put back after it), the
# battle started, the game ended from here after 'run' seconds; the logs tell whether it drew, hooked and loaded.
CASES['player_smoke'] = ('the players\' build of rolhook: a battle for 30 s, ended from outside; its logs checked',
                         {'player': True, 'run': 30})
# The same with the camera left to the person at the game (the scout flies off the start view): the start zoom, no
# wheel or keys from the test, 40 s recorded; the GIF's part and crop are picked from the recordings afterwards.
_FREE = {'shots': '10, 30', 'quit': '40', 'capture': '0'}
CASES['phoenix_free_before'] = ('the Alin fire scout without the modpack (as bare), the camera moved by hand, recorded',
                                dict(CASES['bare'][1], graphics=dict(CASES['bare'][1]['graphics'], borderless='1'),
                                     test=dict(CASES['bare'][1]['test'], **_FREE), tribe=0, record=True))
CASES['phoenix_free_after'] = ('the Alin fire scout with the modpack at its defaults, the camera moved by hand, recorded',
                               {'mod_defaults': True, 'camera': dict(_LADDER), 'graphics': {'borderless': '1'},
                                'test': dict(_FREE), 'tribe': 0, 'record': True})
# The interface scale chosen for the resolution ([ui] layout_scale=0, the ui_scale mod's default): core.log names the
# height it found (the profile's resolution) and the scale, then the layouts read from that scale's variant.
CASES['ui_auto'] = ('interface scale auto: the scale for the profile\'s resolution (core.log), its layouts in use',
                    {'ui': {'layout_scale': '0'}})

# Campaign cases: the game starts straight into the newest autosave of a campaign (Saves\AutoSaves\<prefix>*.sav of
# the profile; the command line takes a .sav like a .gamesetup, app state 0x0C) copied to work/autotest/campaign.sav
# (no spaces in the path); the campaign's autosaves are protected files like the profile's. The campaign map has no
# battle terrain, so the test clock starts at the first frame. ctw_files: every file the game opens while it loads
# the campaign (files.log at debug), the campaign scripts' window functions and every distinct picture drawn (ui.log;
# tools/ui_sprites.py <session> lists those drawn at their texel size).
CAMPAIGN = {'ctw_files': 'CTW_VINCI_'}

# The Mods tab in each language of the game: loc.ini's LANG set for the game (restored after it, as the profile
# files), the mods_tab mod re-applied first so its tab is written in that language (modmgr enable mods_tab), the
# options window opened in the battle and the tab shown (rolhook [test] options / options_tab), a shot of it. After
# the run the tab is rebuilt for the language loc.ini has again. An install has the font set (loc\<lang>\fonts.xml) only
# of the languages it was installed with - without it the game stops at "Fonts element not found" - so a language
# that lacks one borrows loc\RU\fonts.xml (Centaur BHG, Arial, Arial Unicode for any other script) for the run.
TAB_LANGS = {'tab_en': 'en', 'tab_de': 'DE', 'tab_fr': 'FR', 'tab_it': 'IT', 'tab_es': 'ES', 'tab_pl': 'PL',
             'tab_cz': 'CZ', 'tab_ru': 'RU', 'tab_ja': 'JA', 'tab_tc': 'TC'}
# --lang <code> runs any other case the same way in that language (mod page shots in English: --lang en)
LANG_CODES = {v.lower(): v for v in TAB_LANGS.values()}
for _case, _lang in TAB_LANGS.items():
    CASES[_case] = (f'the Mods tab with the game in {_lang}: options at 6 s, the tab shown, shot at 9 s',
                    {'test': {'options': '6', 'options_tab': 'Mods', 'shots': '9', 'quit': '10', 'capture': '0'}})
CASES['ctw_files'] = ('Vinci campaign from its newest autosave: every file opened (files.log), script window trace',
                      {'test': {'start': 'first_frame', 'shots': '20, 28', 'quit': '30'},
                       'levels': {'files': 'debug'}, 'debug': {'log_script_ui': '1', 'log_sprites': '1'}})

# Replay cases: the game plays back a recorded game (the profile's Recorded Games, the largest .rec unless
# --replay names part of one) copied to work/autotest/replay.rec - the command line takes a .rec like a .sav (app
# state 0x17). motion: the units on screen read frame by frame for 4 s ([debug] motion_probe -> motion.log;
# tools/motion.py <session> turns it into update rates).
REPLAY = {'motion', 'anim_calls', 'anim_source', 'skin_calls', 'bone_watch', 'bone_chain', 'anim_time', 'pose_cache', 'motion_smooth', 'smooth_fps', 'smooth_fps_off'}
CASES['motion'] = ('replay of a recorded game, units on screen measured frame by frame (motion.log)',
                   {'test': {'shots': '14, 20', 'quit': '22'}, 'debug': {'motion_probe': '15, 4'}})
# motion_smooth: the same with rolhook's smooth animation ([graphics] anim_pose_ms=1: a fresh skeleton pose every frame)
CASES['motion_smooth'] = ('replay of a recorded game, smooth animation on, units measured frame by frame',
                          {'test': {'shots': '14, 20', 'quit': '22'}, 'debug': {'motion_probe': '15, 4'},
                           'graphics': {'anim_pose_ms': '1'}})
# anim_calls: the same, with the renderlib animation module's functions counted per frame (calls.log, [debug]
# count_calls; addresses and safe lengths from tools/prologue.py) - which run on every frame, which on the ticks.
ANIM_FUNCS = ('0xEE0F40/6, 0xEE1E00/5, 0xEE43A0/6, 0xEE4A40/6, 0xEE4C00/5, 0xEE4DC0/6, 0xEE5170/7, 0xEE5570/7, '
              '0xEE5860/6, 0xEE67B0/6, 0xEE81F0/6, 0xEE8020/5, 0xEE7CF0/7, 0xEE61B0/6, 0xEE3E20/5')
CASES['anim_calls'] = ('replay of a recorded game, animation functions counted per frame (calls.log, motion.log)',
                       {'test': {'quit': '19'}, 'debug': {'motion_probe': '15, 3', 'count_calls': ANIM_FUNCS,
                                                         'count_window': '15, 3'}})
# anim_source: where the game copies a unit's posed vertices from ([debug] motion_source: the uploaded first vertex
# looked for in memory, write watchpoints there - probe.log names the code that poses units on the ticks)
CASES['anim_source'] = ('replay of a recorded game, the posed-vertex source found and watched (d3d.log, probe.log)',
                        {'test': {'quit': '19'}, 'debug': {'motion_probe': '15, 3', 'motion_source': '1'}})
# skin_calls: the mesh render / vertex fill / skinning chain the watchpoints found, counted per frame next to the
# motion probe's tick frames - which parts run every frame and which on the ticks
SKIN_FUNCS = ('0xE7FF10/5, 0xE807D0/5, 0xE80AF0/5, 0xE83BC0/6, 0xE853F0/6, 0xE84430/6, 0xE858E0/6, 0xE83A80/6, '
              '0xA69180/6, 0xE8C090/6, 0xEE9720/7, 0xE81B80/6, 0xE82190/6, 0xE82BB0/6, 0xE83250/6, 0xE845A0/6, '
              '0xE85AA0/6, 0xE86250/9, 0xE86B80/9, 0xE80340/6')
CASES['skin_calls'] = ('replay of a recorded game, the skinning chain counted per frame (calls.log, motion.log)',
                       {'test': {'quit': '19'}, 'debug': {'motion_probe': '15, 3', 'count_calls': SKIN_FUNCS,
                                                         'count_window': '15, 3'}})
# bone_watch: the per-frame skinning (0x00E858E0) reads each bone group's matrix at +0x70 of the group (esi at
# 0x00E85901); from 15 s the first group it skins gets a write watch there - probe.log names the code that poses
# the bones on the ticks
CASES['bone_watch'] = ('replay of a recorded game, a bone matrix watched for its writer (probe.log)',
                       {'test': {'quit': '19'}, 'debug': {'motion_probe': '15, 3', 'watch_follow': '0xE85901:esi+0x70:4w',
                                                         'watch_follow_at': '15'}})
# bone_chain: the bone node's animated matrix (+0x40: billboards write it too, 0x00EA0D30; +0x10 stayed unwritten)
# watched for its writer, and the node update chain (scene graph: 0x00EA21D0 dirty list -> 0x00EA0EB0 node update
# -> 0x0055EF10 matrix product) counted
BONE_FUNCS = ('0xE72150/5, 0xEA21D0/6, 0xEA0EB0/10, 0xA69C40/6, 0xEF31A0/5, 0xE8D880/6, 0xE8BE90/5, 0xEA0FC0/6, '
              '0x55EF10/5')
CASES['bone_chain'] = ('replay of a recorded game, a bone node\'s animated matrix watched, the node update chain counted',
                       {'test': {'quit': '19'}, 'debug': {'motion_probe': '15, 3', 'watch_follow': '0xE85901:esi+0x40:4w',
                                                         'watch_follow_at': '15', 'count_calls': BONE_FUNCS,
                                                         'count_window': '15, 3'}})

# anim_time: the animation time each drawn object samples at, frame by frame ([debug] motion_anim: object + 0x14,
# copied from its animation state by 0x00EE94E0) - how it steps on the ticks and in what units
CASES['anim_time'] = ("replay of a recorded game, each object's animation time, frame by frame (motion.log A lines)",
                      {'test': {'quit': '18'}, 'debug': {'motion_probe': '15, 2', 'motion_vertices': '0',
                                                        'motion_anim': '1'}})

# pose_cache: the unit render update 0x00A69C40 either copies its cached bone matrices (from 0x00A69F5F) or evaluates
# the animation (0x00E72150, back at 0x00A69FCF) - counted per frame next to the pose ticks
CASES['pose_cache'] = ('replay of a recorded game, cached against evaluated poses counted per frame (calls.log)',
                       {'test': {'quit': '19'}, 'debug': {'motion_probe': '15, 3', 'count_window': '15, 3',
                                                         'count_calls': '0xA69F5F/6, 0xA69FCF/5, 0xA69C40/6, '
                                                                        '0xE72150/5'}})

# smooth_fps / smooth_fps_off: the replay without probes, with the smooth animation (a fresh pose every frame) and
# without it - what the extra skeleton evaluations cost (frame ms in the match)
CASES['smooth_fps'] = ('replay of a recorded game, smooth animation on, no probes (frame times)',
                       {'test': {'shots': '20', 'quit': '30'}, 'graphics': {'anim_pose_ms': '1'}})
CASES['smooth_fps_off'] = ('replay of a recorded game, smooth animation off, no probes (frame times)',
                           {'test': {'shots': '20', 'quit': '30'}, 'graphics': {'anim_pose_ms': '0'}})

# bld_motion: the building parts of the main pass followed frame by frame ([debug] motion_buildings: P lines; the
# replay's view has an Alin glass mill with turning sails from 6 s on) - how often an animated part moves, against the
# frame rate; the animation times of the drawn objects too (A lines)
CASES['bld_motion'] = ('replay of a recorded game, building parts and animation times frame by frame (motion.log)',
                       {'test': {'quit': '13'}, 'debug': {'motion_probe': '7, 3', 'motion_vertices': '0',
                                                         'motion_buildings': '1', 'motion_anim': '1'}})
# bld_pose: the mill's sails and the flags are posed on the CPU into dynamic buffers like units (bld_motion: dyn): the
# same with the vertices read - how often their pose changes - and the source of the first part's vertices watched
# (probe.log: the code that poses it); the A lines name the render update that asked for each object's time
CASES['bld_pose'] = ('replay of a recorded game, CPU-posed building parts measured frame by frame, their vertex source '
                     'watched (motion.log, probe.log)',
                     {'test': {'quit': '13'}, 'debug': {'motion_probe': '7, 3', 'motion_buildings': '1',
                                                       'motion_anim': '1', 'motion_source': '1'}})
# bld_time: the same measures for 3 s without the source search (its 2 s memory scan ate bld_pose's window), then the
# animation time's writer: at 0x00E72166 (edi = the drawn object, ebx = the animation state it took the time from) the
# first building object with a running animation (handle 0..15, time > 0 and not the last evaluated one: the mill's
# sails, flags; a finished animation holds its time) - its state's time (+ 0x38) gets a write watch, probe.log names
# the code that sets the time
CASES['bld_time'] = ('replay of a recorded game, CPU-posed building parts measured, then the writer of a building '
                     'animation\'s time watched (motion.log, probe.log)',
                     {'test': {'quit': '14'}, 'debug': {'motion_probe': '7, 3', 'motion_buildings': '1',
                                                       'motion_anim': '1', 'watch_follow': '0xE72166:ebx+0x38:4w',
                                                       'watch_follow_at': '11',
                                                       'watch_follow_if': 'edi+0x10>=0 & edi+0x10<16 & edi+0x14>0 & '
                                                                          'edi+0x14!=edi+0x1c'}})
# anim_clock: the states are made anew every frame (a pool, 0x00E7E110) and many fillers take the time from the game
# world's clock ([0x013E50AC] + 0x38 / + 0x3C): the clock frame by frame next to the drawn objects' times (W and A
# lines), then a write watch on it - the code that advances it
CASES['anim_clock'] = ("replay of a recorded game, the game world's clock and the animation times frame by frame, the "
                       "clock's writer watched (motion.log, probe.log)",
                       {'test': {'quit': '14'}, 'debug': {'motion_probe': '7, 3', 'motion_vertices': '0',
                                                         'motion_anim': '1', 'watch_deref': '0x13E50AC+0x38:4w',
                                                         'watch_deref_at': '11'}})
# clock_smooth / clock_engine: rolhook's animation clocks that run on through a late tick ([graphics] anim_clock, on
# with smooth animation) and the engine's that stand at its end - the world clock (W lines, with the tick's part as the
# engine had it and as given), the mill's sails and the flags (U lines with motion_buildings), the units' times (A
# lines), frame by frame; motion.py: how evenly each advances per ms of frame time
_CLOCK = {'test': {'quit': '13'}, 'debug': {'motion_probe': '7, 3', 'motion_buildings': '1', 'motion_anim': '1'}}
CASES['clock_smooth'] = ('replay of a recorded game, late ticks run on: the clock, building parts and units measured',
                         dict(_CLOCK, graphics={'anim_pose_ms': '1', 'anim_clock': '1'}))
CASES['clock_engine'] = ("replay of a recorded game, the engine's game clock: the clock, building parts and units "
                         "measured", dict(_CLOCK, graphics={'anim_pose_ms': '1', 'anim_clock': '0'}))
# mill_matrix: the Alin mine's turning head (its 371-vertex part) turns 3.6 degrees once a tick and stands between
# (P lines of clock_smooth): its world matrix looked for in memory at a frame it moved ([debug] motion_matsrc), write
# watches there - probe.log names the code that turns it and how often it runs
CASES['mill_matrix'] = ("replay of a recorded game, the world matrix of the Alin mine's turning head found and watched "
                        "(d3d.log, probe.log)",
                        {'test': {'quit': '14'}, 'debug': {'motion_probe': '7, 4', 'motion_buildings': '1',
                                                          'motion_vertices': '0', 'motion_matsrc': 'alim_mill:371'}})
# xform: every drawn object's transform as the render copies it (0x00E71AF0, [debug] motion_xform: X lines) next to
# the ticks (W lines) - which objects move only when a tick runs (the mine's head; units?)
CASES['xform'] = ("replay of a recorded game, every drawn object's transform frame by frame next to the ticks "
                  "(motion.log X and W lines)",
                  {'test': {'quit': '13'}, 'debug': {'motion_probe': '7, 3', 'motion_vertices': '0', 'motion_anim': '1',
                                                    'motion_xform': '1'}})
REPLAY.update({'bld_motion', 'bld_pose', 'bld_time', 'anim_clock', 'clock_smooth', 'clock_engine', 'mill_matrix',
               'xform'})

# Settings that live in the profile, set for a case's game in every place and put back byte for byte afterwards:
#  * graphics features: <FEATURE name=... value=.../> in every SYSHASH__ block (one block per system configuration
#    the profile has seen; the game uses the current system's);
#  * 'ELEMENT.attribute' keys: an attribute of a top-level element, e.g. GRAPHICS.shadowmap_size.
# BASE_PROFILE applies to every case: no tips window over the middle of the screen at the start of the battle.
BASE_PROFILE = {'ROOT.show_tips': '0'}
PROFILE = {
    'no_hidden': {'FullResTerrain': '0', 'UseStaticSplats': '0'},
    'no_fullres': {'FullResTerrain': '0'},
    'no_splats': {'UseStaticSplats': '0'},
}


# ---------------------------------------------------------------- environment
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


def game_running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq legends.exe', '/NH'], capture_output=True, text=True).stdout
    return 'legends.exe' in out.lower()


def hd_pass_running():
    try:
        with open(HD_LOCK, encoding='utf-8') as f:
            return pid_alive(json.load(f).get('pid'))
    except (OSError, ValueError):
        return False


def read_ini_flat(path):
    vals = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if '=' in line and not line.lstrip().startswith((';', '[')):
                k, v = line.split('=', 1)
                vals[k.strip()] = v.strip()
    return vals


def profile():
    """(profile folder, player name) from personal.ini: CurrentProfileFile, ProfilesDirectoryOverride."""
    vals = read_ini_flat(os.path.join(MYGAMES, 'personal.ini'))
    root = vals.get('ProfilesDirectoryOverride') or MYGAMES
    name = os.path.splitext(vals.get('CurrentProfileFile', ''))[0]
    if not name:
        raise SystemExit('personal.ini names no current profile (CurrentProfileFile)')
    return os.path.join(root, name), name


# ---------------------------------------------------------------- files the game may write
def protected_files(prof_dir, name):
    return [os.path.join(prof_dir, 'Saves', 'Game Setup Files', 'last_game.gamesetup'),
            os.path.join(prof_dir, name + '.profile'),
            os.path.join(MYGAMES, 'personal.ini')]


def backup(paths, dst):
    os.makedirs(dst, exist_ok=True)
    for i, p in enumerate(paths):
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(dst, f'{i}_{os.path.basename(p)}'))


def restore(paths, bak, out_dir):
    """Puts back every protected file the game changed; the changed copy stays in out_dir for inspection."""
    notes = []
    for i, p in enumerate(paths):
        b = os.path.join(bak, f'{i}_{os.path.basename(p)}')
        if not os.path.exists(b):
            continue
        if os.path.exists(p) and filecmp.cmp(p, b, shallow=False):
            continue
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(out_dir, 'changed_' + os.path.basename(p)))
        shutil.copy2(b, p)
        notes.append(f'restored {os.path.basename(p)}')
    return notes


def set_profile_features(path, feats):
    """Sets <FEATURE ... name="X" value="..."/> in every block of the profile, and 'ELEMENT.attr' attributes;
    returns how many values were set."""
    with open(path, 'rb') as f:
        data = f.read()
    n = 0
    for name, value in feats.items():
        if '.' in name:
            el, attr = (s.encode() for s in name.split('.', 1))
            pat = re.compile(rb'(<' + re.escape(el) + rb'\s[^>]*?\b' + re.escape(attr) + rb'=")[^"]*(")')
        else:
            pat = re.compile(rb'(<FEATURE id="\d+" name="' + re.escape(name.encode()) + rb'" value=")[^"]*(")')
        data, k = pat.subn(rb'\g<1>' + value.encode() + rb'\g<2>', data)
        n += k
    with open(path, 'wb') as f:
        f.write(data)
    return n


# ---------------------------------------------------------------- probes: checks made from outside while the game runs
def wait_for_match(t0, timeout=90):
    """The session's core.log line that the match is on screen (the test clock started)."""
    end = time.time() + timeout
    while time.time() < end:
        s = session_after(t0)
        if s:
            try:
                with open(os.path.join(s, 'core.log'), encoding='utf-8', errors='replace') as f:
                    if 'match on screen' in f.read():
                        return True
            except OSError:
                pass
        time.sleep(0.5)
    return False


def game_window(pid):
    """The largest visible top-level window of the process."""
    import ctypes.wintypes as wt
    u = ctypes.windll.user32
    best = [None, 0]

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, _):
        owner = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(owner))
        if owner.value == pid and u.IsWindowVisible(h):
            r = wt.RECT()
            u.GetWindowRect(h, ctypes.byref(r))
            area = (r.right - r.left) * (r.bottom - r.top)
            if area > best[1]:
                best[:] = [h, area]
        return True

    u.EnumWindows(cb, 0)
    return best[0]


def probe_focus(t0, pid):
    """The borderless game in front: the cursor is clipped to its monitor. Alt-Tab away: no clip, another window in
    front, and the cursor stays where it is put on another monitor (the engine's RestrictMouse callback must not pull
    it back). Alt-Tab back. A game started by this script may not get the foreground by itself: it is brought forward
    first (an Alt press lets SetForegroundWindow through), and the result says INCONCLUSIVE if that fails."""
    import ctypes.wintypes as wt
    u = ctypes.windll.user32
    if not wait_for_match(t0):
        return ['probe: the match never came on screen']
    time.sleep(2)
    game = game_window(pid)
    if game:
        u.keybd_event(0x12, 0, 0, 0)
        u.SetForegroundWindow(game)
        u.keybd_event(0x12, 0, 2, 0)
        time.sleep(1.5)   # a Present or two: rolhook clips the cursor while the game is in front
    r, p0, p = wt.RECT(), wt.POINT(), wt.POINT()
    u.GetCursorPos(ctypes.byref(p0))
    u.GetClipCursor(ctypes.byref(r))
    clip_front = (r.left, r.top, r.right, r.bottom)
    game_fg = u.GetForegroundWindow()
    if not game or game_fg != game:
        return [f'probe focus: INCONCLUSIVE - the game window {game} never came to the front (foreground {game_fg})']
    gr = wt.RECT()
    u.GetWindowRect(game, ctypes.byref(gr))
    clipped_front = clip_front == (gr.left, gr.top, gr.right, gr.bottom)

    def alt_tab():
        u.keybd_event(0x12, 0, 0, 0)          # Alt down
        u.keybd_event(0x09, 0, 0, 0)          # Tab
        time.sleep(0.05)
        u.keybd_event(0x09, 0, 2, 0)
        u.keybd_event(0x12, 0, 2, 0)          # Alt up

    alt_tab()
    time.sleep(1.5)
    u.GetClipCursor(ctypes.byref(r))
    clip_away = (r.left, r.top, r.right, r.bottom)
    fg_away = u.GetForegroundWindow()
    vx, vy, vw, vh = (u.GetSystemMetrics(i) for i in (76, 77, 78, 79))
    target = (vx + vw - 200, vy + vh // 2)            # far end of the virtual desktop: another monitor if there is one
    u.SetCursorPos(*target)
    time.sleep(0.8)
    u.GetCursorPos(ctypes.byref(p))
    stayed = abs(p.x - target[0]) <= 2 and abs(p.y - target[1]) <= 2
    unclipped = clip_away == (vx, vy, vx + vw, vy + vh)
    u.SetCursorPos(p0.x, p0.y)
    alt_tab()                                         # back to the game, so it presents frames and ends itself
    ok = clipped_front and fg_away != game_fg and unclipped and stayed
    return [f'probe focus: {"PASS" if ok else "FAIL"} | in front: clip {clip_front} '
            f'({"the game window" if clipped_front else "NOT the game window"}) | after Alt-Tab: '
            f'{"another window" if fg_away != game_fg else "STILL THE GAME"} in front, clip {clip_away} '
            f'({"free" if unclipped else "CLIPPED"}), cursor put at {target} -> {(p.x, p.y)} '
            f'({"stayed" if stayed else "PULLED BACK"})']


PROBES = {'borderless_focus': probe_focus}


def listing(folder):
    if not os.path.isdir(folder):
        return {}
    return {n: os.path.getmtime(os.path.join(folder, n)) for n in os.listdir(folder)
            if os.path.isfile(os.path.join(folder, n))}


# ---------------------------------------------------------------- one run
def mod_defaults():
    """{section: {key: value}}: for every key of the Mods tab's own layer (rolhook.d/zz_options.ini, read last), the
    value the layers under it give - rolhook.ini, then rolhook.d/*.ini in name order: the modpack's defaults, the
    user's choices set aside."""
    def read(path):
        cfg = configparser.ConfigParser(interpolation=None, strict=False)
        cfg.optionxform = str
        with open(path, encoding='utf-8', errors='replace') as f:
            cfg.read_string(f.read())
        return cfg
    d = os.path.join(GAME, 'rolhook.d')
    user = os.path.join(d, 'zz_options.ini')
    if not os.path.isfile(user):
        return {}
    layers = [read(os.path.join(GAME, 'rolhook.ini'))] + [
        read(os.path.join(d, n)) for n in sorted(os.listdir(d)) if n.lower().endswith('.ini') and n != 'zz_options.ini']
    out = {}
    u = read(user)
    for s in u.sections():
        for k in u[s]:
            vals = [c[s][k] for c in layers if c.has_option(s, k)]
            if vals:
                out.setdefault(s, {})[k] = vals[-1]
    return out


def write_run_ini(overrides):
    secs = {s: dict(kv) for s, kv in TEST_BLOCK.items()}
    if overrides.get('mod_defaults'):   # the modpack as a new player has it: the Mods tab's choices set aside
        for s, kv in mod_defaults().items():
            secs.setdefault(s, {}).update(kv)
    for s, kv in overrides.items():
        if isinstance(kv, dict):   # sections; the rest (mod_defaults, tribe, record) are the run's own options
            secs.setdefault(s, {}).update(kv)
    lines = ['; written by modkit/tools/autotest.py for one test game (read through ROLHOOK_EXTRA_INI)']
    for s, kv in secs.items():
        lines.append(f'[{s}]')
        lines += [f'{k}={v}' for k, v in kv.items()]
    with open(RUN_INI, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')


def session_after(t_launch):
    """The rolhook session folder this game created (named by its local start time)."""
    best = None
    if not os.path.isdir(SESSIONS):
        return None
    for n in os.listdir(SESSIONS):
        try:
            t = datetime.datetime.strptime(n, '%Y-%m-%d_%H-%M-%S').timestamp()
        except ValueError:
            continue
        if t >= t_launch - 3 and (best is None or t < best[0]):
            best = (t, n)
    return os.path.join(SESSIONS, best[1]) if best else None


def frame_times(session):
    """'frame ms in the match: avg A, worst B' from the periodic lines of frame.log that start after the test clock
    did (core.log: 'test clock starts' at frame N); the map loading before it would swamp the numbers."""
    start = None
    core = os.path.join(session, 'core.log')
    if os.path.exists(core):
        with open(core, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.search(r'\[f\s+(\d+)\].*test clock starts', line)
                if m:
                    start = int(m.group(1))
                    break
    p = os.path.join(session, 'frame.log')
    avgs, worst, prev = [], 0.0, None
    if start is not None and os.path.exists(p):
        with open(p, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.search(r'\[f\s+(\d+)\].*frame ms avg\s+([\d.]+)\s+min\s+[\d.]+\s+max\s+([\d.]+)', line)
                if not m:
                    continue
                # a line covers the frames since the previous one: count it when all of them are in the match
                if prev is not None and prev >= start:
                    avgs.append(float(m.group(2)))
                    worst = max(worst, float(m.group(3)))
                prev = int(m.group(1))
    if not avgs:
        return None
    return f'frame ms in the match: avg {sum(avgs) / len(avgs):.2f}, worst {worst:.1f} ({len(avgs)} periods)'


def summarize(session, out_dir):
    lines, errors, hd = [], 0, 0
    core = os.path.join(session, 'core.log')
    if os.path.exists(core):
        with open(core, encoding='utf-8', errors='replace') as f:
            for line in f:
                body = line.split('| ', 1)[-1].rstrip()
                if body.startswith(('TEST RUN', 'test:', 'shot', 'process exit', 'extra config', 'shadow fit')):
                    lines.append(body)
    ft = frame_times(session)
    if ft:
        lines.append(ft)
    for name, counter in (('all.log', 'ERROR '), ('textures.log', 'INFO  textures| HD ')):
        p = os.path.join(session, name)
        if os.path.exists(p):
            with open(p, encoding='utf-8', errors='replace') as f:
                n = sum(1 for line in f if counter in line)
            if name == 'all.log':
                errors = n
            else:
                hd = n
    return lines, errors, hd


LOC_INI = os.path.join(GAME, 'loc.ini')
MODMGR = os.path.join(KIT, 'tools', 'modmgr.py')
RECORD_FPS = 50                  # screen recordings: a GIF shows 50 fps at most (2/100 s a frame)
KEY_VK = {'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28}   # [test] keys names rolhook knows


def set_game_language(lang):
    """loc.ini's LANG (the file keeps its other lines)."""
    with open(LOC_INI, encoding='utf-8', errors='replace') as f:
        text = f.read()
    text, n = re.subn(r'(?mi)^(\s*LANG\s*=\s*)\S*', lambda m: m.group(1) + lang, text)
    if not n:
        text = text.rstrip('\n') + f'\nLANG={lang}'
    with open(LOC_INI, 'w', encoding='utf-8', newline='') as f:
        f.write(text)


def reapply_mods_tab(why):
    """The mods_tab mod applied again: its tab written in loc.ini's language (archives rebuilt)."""
    r = subprocess.run([sys.executable, MODMGR, 'enable', 'mods_tab'], cwd=GAME, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    print(f'   mods_tab re-applied ({why}): exit {r.returncode}', flush=True)
    if r.returncode:
        raise SystemExit(r.stdout + r.stderr)


def case_language(case, args):
    """The language a case's game runs in: a tab case's own, else --lang's, else None (loc.ini as it is)."""
    if case in TAB_LANGS:
        return TAB_LANGS[case]
    return LANG_CODES[args.lang.lower()] if args.lang else None


def run_case(case, out_root, args):
    desc, overrides = CASES[case]
    out_dir = os.path.join(out_root, case)
    os.makedirs(out_dir, exist_ok=True)
    write_run_ini(overrides)
    shutil.copy2(RUN_INI, os.path.join(out_dir, 'run.ini'))
    prof_dir, name = profile()
    paths = protected_files(prof_dir, name)
    lang = case_language(case, args)
    if lang:
        paths.append(LOC_INI)
    launch = SETUP
    if case in CAMPAIGN:
        autosaves = os.path.join(prof_dir, 'Saves', 'AutoSaves')
        saves = [os.path.join(autosaves, n) for n in os.listdir(autosaves) if n.startswith(CAMPAIGN[case])]
        if not saves:
            raise SystemExit(f'{case}: no {CAMPAIGN[case]}* autosave in {autosaves}')
        launch = os.path.join(WORK, 'campaign.sav')
        shutil.copy2(max(saves, key=os.path.getmtime), launch)
        paths += saves
    replays = os.path.join(prof_dir, 'Recorded Games')
    if case in REPLAY:
        recs = [os.path.join(replays, n) for n in os.listdir(replays)
                if n.lower().endswith('.rec') and (args.replay or '').lower() in n.lower()]
        if not recs:
            raise SystemExit(f'{case}: no .rec{" matching " + args.replay if args.replay else ""} in {replays}')
        pick = max(recs, key=os.path.getsize)
        launch = os.path.join(WORK, 'replay.rec')
        shutil.copy2(pick, launch)
        print(f'   replay: {os.path.basename(pick)} ({os.path.getsize(pick)} bytes)')
    bak = os.path.join(out_dir, 'profile_backup')
    backup(paths, bak)
    before = listing(replays)

    print(f'== {case}: {desc}', flush=True)
    pre = []
    lent_fonts = None
    t0 = time.time()
    try:
        if lang:
            loc_before = set(os.listdir(os.path.join(GAME, 'loc', lang)))
            fonts = os.path.join(GAME, 'loc', lang, 'fonts.xml')
            if not os.path.exists(fonts):
                os.makedirs(os.path.dirname(fonts), exist_ok=True)
                shutil.copyfile(os.path.join(GAME, 'loc', 'RU', 'fonts.xml'), fonts)   # not its read-only flag
                lent_fonts = fonts
                pre.append(f'loc\\{lang}\\fonts.xml lent from loc\\RU for the run (this install has no {lang} font set)')
            set_game_language(lang)
            reapply_mods_tab(f'LANG={lang}')
            pre.append(f'game language for this run: LANG={lang}')
            t0 = time.time()
        edits = dict(BASE_PROFILE, **PROFILE.get(case, {}))
        if edits:
            n = set_profile_features(paths[1], edits)
            pre.append(f'profile for this game: {n} value(s) set (' + ', '.join(f'{k}={v}' for k, v in edits.items())
                       + ')')
        if 'tribe' in overrides or 'submap' in overrides:
            with open(launch, encoding='utf-8') as f:
                text = f.read()
            tag = ''
            if 'tribe' in overrides:   # the player's nation (0 Alin, 2 Cuotl, 3 Vinci as the template has it)
                text = re.sub(r'(<PLAYER [^>]*?\btribe=")\d+(")', rf'\g<1>{overrides["tribe"]}\g<2>', text, count=1)
                tag += f'_tribe{overrides["tribe"]}'
                pre.append(f'the player\'s nation for this game: tribe {overrides["tribe"]}')
            if 'submap' in overrides:   # which of Random Small Map's maps (RANDOM_SMALL); its checksum stays valid
                text = re.sub(r'map_submap_id="\d+"', f'map_submap_id="{overrides["submap"]}"', text, count=1)
                tag += f'_map{overrides["submap"]}'
                pre.append(f'map for this game: {RANDOM_SMALL[overrides["submap"]]}')
            launch = os.path.join(WORK, f'battle{tag}.gamesetup')
            with open(launch, 'w', encoding='utf-8') as f:
                f.write(text)
        env = dict(os.environ, ROLHOOK_EXTRA_INI=os.path.relpath(RUN_INI, GAME))
        rec = None
        if overrides.get('record'):   # the screen as the player sees it, the whole run (tools/compare.py --video)
            rec = {'file': os.path.join(out_dir, 'record.mp4'), 'fps': RECORD_FPS}
            ff = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i',
                                   f'ddagrab=output_idx=0:framerate={RECORD_FPS}:draw_mouse=0', '-fps_mode', 'cfr',
                                   '-r', str(RECORD_FPS), '-c:v', 'h264_nvenc', '-preset', 'p5', '-rc', 'constqp',
                                   '-qp', '14', rec['file']],
                                  stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            rec['ffmpeg_start'] = time.time()
            time.sleep(1.0)   # its first frames before the game's window comes up
        if overrides.get('player'):   # the players' build for this game; the full one waits in the case folder
            dll = os.path.join(GAME, 'ddraw.dll')
            dev_dll = os.path.join(out_dir, 'ddraw_full.dll')
            shutil.copy2(dll, dev_dll)
            shutil.copy2(os.path.join(KIT, 'build', 'native', 'player', 'ddraw.dll'), dll)
            pre.append('rolhook: the players\' build (build\\native\\player\\ddraw.dll) for this game')
        t_game = time.time()
        p = subprocess.Popen([EXE, os.path.relpath(launch, GAME)], cwd=GAME, env=env)
        probe_out = []
        th = None
        if case in PROBES:
            import threading
            th = threading.Thread(target=lambda: probe_out.extend(PROBES[case](t0, p.pid)), daemon=True)
            th.start()
        run = overrides.get('run') or args.timeout   # 'run': the game has no test quit, it is ended from here
        try:
            rc = p.wait(timeout=run)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            rc = ('ended after %d s, as planned' if overrides.get('run') else 'killed after %d s') % run
        if rec:
            try:
                _, err = ff.communicate(b'q', timeout=60)
            except subprocess.TimeoutExpired:
                ff.kill()
                err = b'ffmpeg did not stop'
            rec['game_start'] = t_game
            with open(os.path.join(out_dir, 'record.json'), 'w', encoding='utf-8') as f:
                json.dump(rec, f, indent=1)
            if os.path.isfile(rec['file']) and os.path.getsize(rec['file']) > 0:
                pre.append(f'screen recorded: {os.path.relpath(rec["file"], KIT)} '
                           f'({os.path.getsize(rec["file"]) / 1e6:.0f} MB)')
            else:
                pre.append('screen recording FAILED: ' + (err or b'').decode('utf-8', 'replace')[-300:])
        if th:
            th.join(timeout=5)
            pre += probe_out or ['probe: no result']
    finally:
        if overrides.get('player') and os.path.isfile(os.path.join(out_dir, 'ddraw_full.dll')):
            for _ in range(20):   # the killed game may hold the file a moment
                try:
                    shutil.copy2(os.path.join(out_dir, 'ddraw_full.dll'), os.path.join(GAME, 'ddraw.dll'))
                    break
                except PermissionError:
                    time.sleep(0.5)
            pre.append('rolhook: the full build put back')
        keys = overrides.get('test', {}).get('keys', '')
        if keys:   # rolhook lets its keys go itself; one more 'up' here in case the game died holding one
            for name in {k.split(':')[1].strip() for k in keys.split(',') if k.count(':') == 2}:
                vk = KEY_VK.get(name.lower())
                if vk:
                    ctypes.windll.user32.keybd_event(vk, 0, 0x0002 | 0x0001, 0)   # KEYEVENTF_KEYUP | EXTENDEDKEY
        notes = pre + restore(paths, bak, out_dir)
        if lent_fonts and os.path.exists(lent_fonts):
            os.chmod(lent_fonts, stat.S_IWRITE)
            os.remove(lent_fonts)
            notes.append('the lent fonts.xml removed')
        if lang:   # folders the game makes for a language it starts in (voiceover/taunts...), if empty
            loc_dir = os.path.join(GAME, 'loc', lang)
            for sub in set(os.listdir(loc_dir)) - loc_before:
                for root, dirs, files in os.walk(os.path.join(loc_dir, sub), topdown=False):
                    if not files and not os.listdir(root):
                        os.rmdir(root)
    took = time.time() - t0
    for n, mt in listing(replays).items():
        if n not in before or mt != before[n]:
            shutil.move(os.path.join(replays, n), os.path.join(out_dir, n))
            notes.append(f'moved the test replay "{n}" out of Recorded Games')

    session = session_after(t0)
    shots = []
    if session:
        for n in sorted(os.listdir(session)):
            if n.endswith(('.png', '.log', '.dmp')):
                shutil.copy2(os.path.join(session, n), os.path.join(out_dir, n))
                if n.startswith('shot_') and n.endswith('.png'):
                    shots.append(os.path.join(out_dir, n))
        lines, errors, hd = summarize(session, out_dir)
    else:
        lines, errors, hd = ['no rolhook session folder found for this game'], 0, 0
    print(f'   exit {rc} after {took:.0f} s | session {os.path.basename(session) if session else "-"} | '
          f'{len(shots)} shot(s) | {hd} HD textures | {errors} ERROR line(s)')
    for line in lines:
        print('   ' + line)
    for n in notes:
        print('   ' + n)
    return {'case': case, 'desc': desc, 'rc': str(rc), 'seconds': round(took), 'session': session,
            'shots': shots, 'errors': errors, 'hd': hd, 'log': lines, 'notes': notes}


def contact_sheet(results, path, width=1146):
    from PIL import Image, ImageDraw
    rows = [r for r in results if r['shots']]
    if not rows:
        return None
    ncol = max(len(r['shots']) for r in rows)
    first = Image.open(rows[0]['shots'][0])
    h = round(first.height * width / first.width)
    label = 28
    sheet = Image.new('RGB', (ncol * width, len(rows) * (h + label)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for i, r in enumerate(rows):
        y = i * (h + label)
        draw.text((8, y + 6), f"{r['case']}: {r['desc']}", fill=(255, 220, 120))
        for j, s in enumerate(r['shots']):
            im = Image.open(s).convert('RGB').resize((width, h), Image.LANCZOS)
            sheet.paste(im, (j * width, y + label))
            draw.text((j * width + 8, y + label + 6), os.path.basename(s), fill=(255, 255, 255))
    sheet.save(path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('cases', nargs='*', help='cases to run, in order (see --list)')
    ap.add_argument('--list', action='store_true', help='list the cases')
    ap.add_argument('--dry', action='store_true', help='write the setup and run.ini, start nothing')
    ap.add_argument('--timeout', type=int, default=200, help='kill the game after this many seconds (default 200)')
    ap.add_argument('--replay', default='', help='replay cases: part of the .rec name (default: the largest .rec)')
    ap.add_argument('--lang', type=str.lower, choices=sorted(LANG_CODES),
                    help='run the cases with the game in this language (loc.ini LANG for the run; tab_* cases keep '
                         'their own)')
    args = ap.parse_args()
    if args.list or not args.cases:
        for k, (d, o) in CASES.items():
            print(f'{k:10s} {d}')
            for s, kv in o.items():
                if isinstance(kv, dict):
                    print(f'{"":10s}   [{s}] ' + ' '.join(f'{a}={b}' for a, b in kv.items()))
            run = {s: v for s, v in o.items() if not isinstance(v, dict)}
            if run:
                print(f'{"":10s}   run: ' + ' '.join(f'{a}={b}' for a, b in run.items()))
            if k in PROFILE:
                print(f'{"":10s}   profile: ' + ' '.join(f'{a}={b}' for a, b in PROFILE[k].items()))
        return
    bad = [c for c in args.cases if c not in CASES]
    if bad:
        raise SystemExit(f'unknown case(s): {", ".join(bad)} (see --list)')
    if game_running():
        raise SystemExit('legends.exe is running: close the game first')
    if hd_pass_running():
        raise SystemExit('the HD pass is running (Flux needs the GPU): "hd stop" first')

    os.makedirs(WORK, exist_ok=True)
    prof_dir, name = profile()
    with open(TEMPLATE, 'rb') as f:
        setup = f.read().replace(b'{player}', name.encode('utf-8'))
    with open(SETUP, 'wb') as f:
        f.write(setup)
    if ' ' in os.path.relpath(SETUP, GAME):
        raise SystemExit('the setup path has a space: the game would split it')
    stamp = datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')
    out_root = os.path.join(WORK, stamp)
    print(f'profile {name} ({prof_dir})\nsetup {os.path.relpath(SETUP, GAME)}\nresults {out_root}')
    if args.dry:
        write_run_ini(CASES[args.cases[0]][1])
        print(open(RUN_INI, encoding='utf-8').read())
        return
    os.makedirs(out_root, exist_ok=True)
    results = []
    try:
        for case in args.cases:
            results.append(run_case(case, out_root, args))
            if game_running():
                time.sleep(3)
    finally:
        if any(case_language(c, args) for c in args.cases):
            reapply_mods_tab('the language loc.ini has again')
    sheet = contact_sheet(results, os.path.join(out_root, 'sheet.png'))
    with open(os.path.join(out_root, 'results.json'), 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=1, ensure_ascii=False)
    print(f'sheet: {sheet}' if sheet else 'no shots taken')


if __name__ == '__main__':
    main()
