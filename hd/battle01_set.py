"""Battle 01 test set: textures that are actually on screen in a Vinci skirmish on a bavarian map
(taken from rolhook's textures.log), one or more per class. Prompts describe what is painted, as albedo."""

ALBEDO = 'highly detailed, sharp, flat even lighting, no shadows, albedo texture'

SET = [
    # id, archive, entry, kind, prompt
    ('grass', 'bavarian.big', '.\\art\\terrain\\tilesets\\biomes\\bavarian\\bavarian_grass_01.tga', 'tile',
     'top-down seamless ground texture, short dry autumn grass, yellow and olive blades, small soil patches, '
     'strategy game terrain, ' + ALBEDO),
    ('dirt', 'bavarian.big', '.\\art\\terrain\\tilesets\\biomes\\bavarian\\bavarian_dirt.tga', 'tile',
     'top-down seamless texture of an old cobblestone road, irregular worn stones set in brown earth, dirt and '
     'moss in the gaps, strategy game terrain, ' + ALBEDO),
    ('rock', 'bavarian.big', '.\\art\\terrain\\tilesets\\biomes\\bavarian\\bavarian_rock.tga', 'tile',
     'top-down seamless texture of grey-green weathered rock surface, cracks, lichen, fine mineral grain, '
     'strategy game terrain, ' + ALBEDO),
    ('cliff', 'biomes.big', '.\\art\\terrain\\tilesets\\biomes\\cool_cliff_edge_01_diffuse.tga', 'tile',
     'seamless texture of a brown earthen cliff face, compacted soil, small stones, erosion streaks, '
     'strategy game terrain, ' + ALBEDO),
    ('vinci_wall', 'vinci_buildings.big', '.\\art\\buildings\\vinc\\textures\\vinci_base_2_diffuse.tga', 'atlas',
     'texture atlas of a steampunk renaissance building: riveted rusty iron plates, weathered plaster, ornate iron '
     'door and round window, decorative metal friezes with glass panels, game texture, ' + ALBEDO),
    ('alim_wall', 'alim_buildings.big', '.\\art\\buildings\\alim\\textures\\alim_base_diffuse.tga', 'atlas',
     'texture atlas of an arabian desert palace: sandstone blocks, striped stone bands, carved pointed arches, red '
     'and gold ornamental patterns, game texture, ' + ALBEDO),
    ('cuotl_wall', 'cuotl_buildings.big', '.\\art\\buildings\\cuot\\textures\\cuotl_base_texture_diffuse.tga', 'atlas',
     'texture atlas of an ancient mesoamerican temple with alien technology: carved stone tiles with glyphs, bronze '
     'and verdigris inlays, stone reliefs, game texture, ' + ALBEDO),
    ('steamflyer', 'vinci_units.big', '.\\art\\units\\vinci\\steamflyer_diffuse_hi.tga', 'atlas',
     'texture atlas of a steampunk flying machine: cream canvas wings with wooden ribs, brass and steel mechanical '
     'parts, glass cockpit, rivets, game texture, ' + ALBEDO),
    ('musketeer', 'vinci_units.big', '.\\art\\units\\vinci\\musketeer_perf_diffuse.tga', 'atlas',
     'texture atlas of a renaissance musketeer soldier: face, steel breastplate, leather straps, dark cloth uniform, '
     'game character texture, ' + ALBEDO),
]

NORMALS = [
    # id, archive, entry, diffuse id it belongs to
    ('grass_n', 'bavarian.big', '.\\art\\terrain\\tilesets\\biomes\\bavarian\\bavarian_grass_01_dot3.tga', 'grass'),
    ('vinci_wall_n', 'vinci_buildings.big', '.\\art\\buildings\\vinc\\textures\\vinci_base_2_normal.tga', 'vinci_wall'),
]
