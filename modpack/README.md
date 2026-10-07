# Modpack

Each mod is a folder `<id>/` with a `mod.ini` and its own files. Mods are enabled and disabled one by one; a mod's
requirements are enabled with it, and a mod that others require cannot be disabled.

```
py -3.14 modkit/tools/modmgr.py mods            list the mods and what is enabled
py -3.14 modkit/tools/modmgr.py enable <id>     enable (with requirements)
py -3.14 modkit/tools/modmgr.py disable <id>    disable
py -3.14 modkit/tools/modmgr.py diff <id>       what the mod changes in archive entries, against the original
py -3.14 modkit/tools/modmgr.py setup           the release's mods (release.ini), as Install.bat runs it
```

## Mods

| id | What it does | Requires |
|---|---|---|
| `core` | rolhook (`ddraw.dll` + `rolhook.ini`): loader, logs, engine assert handling, texture override. Options without a mod of their own: gamma-correct lighting on buildings and units (`[graphics] gamma_light`, a Mods tab checkbox, off by default). | — |
| `laa` | Large Address Aware flag in `legends.exe`: 4 GB of address space instead of 2. | — |
| `anisotropic_16x` | 16× anisotropic filtering (the engine uses linear filtering only). | `core` |
| `camera_zoom` | Zoom range: out 7500 → 11000 (extended 8600 → 14000), in 2000 → 1500, 12 steps. rolhook sets the values on the live camera (`[camera]` in `rolhook.d`) and stops the zoom from bouncing after a wheel step (`zoom_spring=0`; adjustable in the Mods tab). | — |
| `mods_tab` | An eighth tab, Mods, in the game's Options window, in the game's language (10 languages, with the game's own terms; the language is read from `loc.ini` at install, so run `modmgr enable mods_tab` after changing it). Groups: Image (anisotropic filtering, model detail via LOD thresholds, sharpening, vibrance, HD textures, foliage anti-aliasing, smooth animation, battlefield markings), Shadows and Lighting (shadow resolution, soft shadows, zoomed-out shadows, ambient occlusion, gamma-correct lighting), Camera (zoom-out limit, zoom steps, zoom bounce), Window and Interface (borderless fullscreen, interface scale). ✱ marks what applies after a restart. The user's choices go to `rolhook.d\zz_options.ini`. | `core` |
| `hd_textures` | rolhook loads textures from `modkit\mods\hd` instead of the archives. The pack is built by the scripts in `hd\` and is not part of the repository. | `core` |
| `hidden_features` | Restores Battlefield Markings (UseStaticSplats), disabled by the developers, and turns it on at the Best detail level (`data.big` → `game_features.xml`, bxml). FullResTerrain is broken (streaked terrain) and has been left alone since 0.3. | — |
| `hires_shadows` | 8192 shadow map instead of the engine's 2048 cap (rolhook patches the three places that compute its size), sampled with linear filtering. Size in the Mods tab, after a restart. | `core` |
| `soft_shadows` | The shadow test in the shaders of buildings, units, grass, decals and the terrain's shadow pass samples a box of shadow-map texels with smoothed edges (2 / 4 / 6 texels wide at levels 1 / 2 / 3) instead of a single tap; each texel is compared against the receiving surface's depth, so slopes do not shadow themselves (`[graphics] soft_shadows`; shader copies are built at startup). Level in the Mods tab, live; default 2. | `core` |
| `ssao` | Ambient occlusion at the base of buildings, in corners and creases, under trees, between units. rolhook redraws the opaque draws into its own INTZ depth buffer (the game's MSAA depth is unreadable), masks particles and overlays, and applies the occlusion before post-processing (`[graphics] ssao`). Level in the Mods tab, live; default 2. | `core` |
| `sharpen` | AMD CAS adaptive sharpening in one pass at the end of the 3D scene, before bloom and the interface: finer detail, no halos, flat areas unchanged (`[graphics] sharpen`). Level in the Mods tab, live; default 2 (Medium). | `core` |
| `vibrance` | Vibrance of the 3D scene: dull colors are saturated more than vivid ones, greys stay grey, brightness is unchanged; same pass as sharpening (`[graphics] vibrance`). Level in the Mods tab, live; default 1 (Light). | `core` |
| `smooth_animation` | The shared skeleton pose cache returns poses no older than 1 ms instead of the game's 66 ms, so limbs move every frame instead of about 16 times a second (`[graphics] anim_pose_ms`). Animation clocks run evenly across simulation ticks instead of holding a frame and jumping 15 times a second: Alin timonium mine sails, flags, units (`anim_clock`). Smooth Animation checkbox in the Mods tab, live. | `core` |
| `ui_scale` | Interface scale through the layouts themselves: coordinates, sizes and spacing in 31 layout files (ui.big, multiplayer_data.big) and in the `<INTERFACE>` section of three campaign files (campaign map windows) are multiplied by the scale, as are the campaign scripts' pixel constants (in the variant's `scripts.big`). `build.py` builds the 125–200% variants with the other mods' patches; at startup rolhook reads the chosen variant, rasterizes all fonts larger, draws pieced frames and buttons magnified, scales pictures the code draws at natural size, and insets the UVs of magnified pictures into their atlas cells. Scale in the Mods tab, from the next start; default Auto, by the resolution in the game profile (1080 lines 100%, 1200 125%, 1440 150%, 1600 175%, 2160 200%). Mouse input, 3D view and camera are not affected. | `core` |
| `dev_probe` | Not for normal play: enables rolhook's probes for one session (`[debug] watch`, `ui_probe`) to find the engine code that reads given values; results in `probe.log`. | `core` |

## Mod format

```ini
[mod]
id=anisotropic_16x
version=1.0
requires=core                 ; comma-separated
name=...
name_ru=...
description_ru=...

[files]
; path in the game folder = path inside the mod folder
rolhook.d/anisotropic_16x.ini=rolhook.d/anisotropic_16x.ini

[patch]
laa=1                         ; exe patches (LAA only for now)

[entries]
; an entry inside a game archive: archive::entry = replacement file or patch:<script.py>
multiplayer_data.big::.\data\gamecamera.xml=patch:patches/gamecamera.py
```

The preferred way to change an entry is a **patch script**: a `.py` file in the mod folder with `patch(text) -> str`.
It gets the entry's text and returns the changed text; the helpers in `tools/entrypatch.py` (`set_attrs`,
`insert_before`, `insert_after`, `replace_once`) change only the intended places and fail if an anchor is missing or
appears more than once. The repository holds only the change, not a copy of the game file, and several mods can patch
the same entry: the patches are applied as a chain. Replacement files (`= path`) are supported too, but they overwrite
the changes of mods enabled earlier (the manager warns).

Archive entries (`[entries]`) are applied like this: on every `enable` / `disable` the affected archive is rebuilt
from the untouched copy in `modkit\backup\BIGS\` with the changes of **all** enabled mods, in the order they were
enabled. A mod is dry-run before it is enabled, so a broken patch stops `enable` before anything changes. When no
changes are left, the original comes back. Rebuilds are byte-exact, so unchanged entries stay exactly as they were.
Text patches (`patch(text)`) work on plain XML (`multiplayer_data.big`, `ui.big`); for the bxml entries of `data.big`
the script defines `patch_bxml(bx)` and edits values with `bx.find(...)`, `bx.get`, `bx.set` (`tools/bxml.py`; the
value type is kept). `modmgr diff` shows a bxml change as an XML diff.

rolhook's settings are layered: `rolhook.ini` (from `core`), then every `rolhook.d\*.ini` in alphabetical order, a
later file overriding a key. A mod that changes rolhook's behavior puts its fragment into `rolhook.d\`, and disabling
the mod removes it. `core.log` lists the files read (`config: ...`).

`rolhook.d\zz_options.ini` is read last: the user's choices from the Mods tab. rolhook writes only the values that
differ from what the enabled mods set and removes a key when its value goes back to the default. A mod lists such
files in `[mod] user_files=`: the manager keeps them when the mod is disabled (settings survive updates), and
`uninstall-all` removes them.
