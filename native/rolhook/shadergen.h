// shadergen.h — pixel shader assembly text: the soft-shadow rewrite of the effects' one-tap shadow test and the
// gamma-correct light on buildings and units. Free of the game and of D3D, so the offline bench (shadertest.exe) runs
// the very code rolhook runs, on dumped shaders.
#pragma once

#include <string>
#include <vector>

namespace rol {
namespace sg {

struct ShadowPlan {
    std::vector<std::string> code;   // the disassembly's code lines, from the version line on
    bool terrain = false;            // the terrain's shadow pass (terrain_shadow_pass_0/2), known by its code's hash
    int tap = -1, frag_at = -1, cmp_at = -1;
    std::string coord;     // the tap's coordinate register
    std::string sampler;   // "s0"
    std::string eps;       // shadowmap_epsilon, register.component ("c2.x")
    std::string frag;      // the fragment's depth in the light, register.component ("r3.w")
    std::string cmp_dst;   // the lit factor's destination ("r2.w")
    int max_r = 0;
    bool used_c[32] = {};
};

// The one-tap test in a disassembled pixel shader (D3DXDisassembleShader text, comments included: the shadow sampler is
// found in the constant table), or the terrain's shadow pass; false when it is neither, *why says what was missing.
bool analyse_shadow(const std::string& text, ShadowPlan& pl, std::string* why = nullptr);

// Temporary registers the rewrite adds above the shader's own.
constexpr int kSoftTemps = 7;

// The terrain's pass, as d3dx9_27 disassembles it: FNV-1a 64 of its code lines joined by '\n', and their count.
constexpr unsigned long long kTerrainHash = 0x2544a7d7bffb95f8ull;
constexpr int kTerrainLines = 78;

// The terrain's pass tests with no epsilon (the engine's quad sits 1.5 texels off the fragment instead); the box,
// compared against the plane under each tap, takes the objects' default shadowmap_epsilon.
constexpr double kTerrainEps = 0.0003;

// The box's width in texels by [graphics] soft_shadows level (0 = off). 7 with the plane bias is past ps_2_x's 512
// instruction slots in the longest shaders (metal_and_dot3shadow_2: 513).
constexpr int kSoftWidth[4] = {0, 2, 4, 6};

// The rewritten shader: the test made over a box of 'width' texels on a 'size' map (see shadergen.cpp); with
// plane_bias each tap is compared against the receiving surface's depth under it. Empty when the shader has no room.
std::string soft_shadow_source(const ShadowPlan& pl, int width, int size, bool plane_bias);

// ---- Gamma-correct light on the buildings and units (see shadergen.cpp): every saturated N.L remapped to the falloff
// light summed in linear space (gamma 2.2) gives, full sun and full shade kept as they were.
struct LightPlan {
    std::vector<std::string> code;   // the original's code lines, from the version line on
    bool gamma = false;              // the anchors found
    std::string sky_diff, ground;    // the preshader's sky - ground colour, hemispheric_ground_color
    std::string light;               // light_diffuse
    std::vector<std::string> anchors;   // the instructions (their text) that leave a saturated N.L ...
    std::vector<std::string> targets;   // ... in these components ("r4.x")
};

// The gamma's scope: the buildings' and units' effects (props such as rocks too). The ground's - the terrain, its
// texture transitions, the splats and decals over it (terrain*, splats*) - keep the game's light: the transitions have
// no saturated N.L to remap, and a decal lit unlike the ground under it would show its edge. 'effect' is the name
// rolhook gives an effect ("dot3shadow_2.fxo"; "a/b" when two share a size, out if either is the ground's).
bool gamma_effect(const std::string& effect);

// The anchors in a disassembled pixel shader (comments included: the constants' names); false when it has none, *why
// says what was missing.
bool analyse_light(const std::string& text, LightPlan& lp, std::string* why = nullptr);

struct LightOptions {
    bool gamma = false;
};
constexpr double kGamma = 2.2;
constexpr int kLightTemps = 2;

// The lighting change made to 'src' - an assembly text: the original's code (code_text) or a soft-shadow copy; the
// anchors are found by their text, registers and constants taken above what 'src' uses. 'src' when the options ask for
// nothing this shader has; empty when it cannot be done.
std::string light_source(const std::string& src, const LightPlan& lp, const LightOptions& o);

// Code lines as an assembly text.
std::string code_text(const std::vector<std::string>& code);

}  // namespace sg
}  // namespace rol
