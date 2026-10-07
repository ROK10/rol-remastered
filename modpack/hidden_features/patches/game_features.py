r"""hidden_features: graphics features BHG switched off or capped in data\game_features.xml (data.big, bxml).

UseStaticSplats ("Persistent Battlefield Markings": craters and scorch marks stay on the ground) carries
<CHECK type="false" compare="equals" value="1"/> — "BLF turned off this feature always": the "false" check yields 0,
so the feature is never available. value="0" makes the check pass.
UseStaticSplats has every quality step at 0; the "best" step turns it on. With "Allow Custom Options" the checkbox
in the Mods tab decides (the engine keeps it in the profile).
FullResTerrain ("Smoother Terrain", every step 0 as well) is left alone since 0.3: switched on, it smears the
terrain textures into long streaks — BHG turned it off for a reason (autotest 2026-10-03).
Up to 0.3 this mod also lifted ShadowMapMaxSize to 4096 and added an "Huge (4096)" Shadow Detail item; neither
ever took effect (the engine caps the shadow map at 2048 in code, autotest 2026-10-03). Shadow resolution is
rolhook's [graphics] shadow_size now (mod hires_shadows).
"""


def feature(bx, name):
    hits = bx.find('root', 'features', 'feature', name=name)
    if len(hits) != 1:
        raise KeyError(f'feature {name}: {len(hits)} matches')
    return hits[0]


def child(bx, node, tag):
    hits = [c for c in bx.children(node) if bx.tag(c) == tag]
    if not hits:
        raise KeyError(f'<{bx.tag(node)}> has no <{tag}>')
    return hits[0]


def patch_bxml(bx):
    f = feature(bx, 'UseStaticSplats')
    checks = [c for c in bx.children(f) if bx.tag(c) == 'check' and bx.get(c, 'type').lower() == 'false']
    if len(checks) != 1:
        raise KeyError(f'UseStaticSplats: {len(checks)} "false" checks')
    bx.set(checks[0], 'value', 0)
    bx.set(child(bx, child(bx, feature(bx, 'UseStaticSplats'), 'settings'), 'best'), 'value', 1)
