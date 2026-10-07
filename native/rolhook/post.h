// post.h — sharpening (AMD's Contrast Adaptive Sharpening) and vibrance over a finished image, D3D9: the part that
// knows nothing of the game. rolhook's ssao.cpp runs it on the battle scene before the interface comes; shadertest.exe
// runs it on a synthetic image against the same formulas computed on the CPU (cas_vibrance below).
#pragma once

#include <d3d9.h>

#include <string>

#include "ao.h"   // Compile_t, Calls

namespace rol {
namespace post {

struct Params {
    // CAS: how far a pixel may move away from its neighbours' mean, at most (an unsharp mask's amount; CAS lowers it
    // where the neighbourhood nears black or white, so edges get no halos and the strongest work is on fine detail).
    // AMD's sharpness 0 / 0.5 / 1 = 1 / 1.6 / 4. 0 = no sharpening.
    float sharpen;
    // vibrance: the saturation raised by up to this share, the less saturated a colour the more (grey stays grey).
    // 0 = none.
    float vibrance;
};

// The CAS filter's weight for a pixel whose neighbourhood allows the full amount ('amp' 1): w = -k / (4 (1 + k)), so
// that (e + w (b + d + f + h)) / (1 + 4 w) = e + k (e - mean): AMD's peak -1 / lerp(8, 5, sharpness).
inline float cas_peak(float k) { return k > 0 ? -k / (4 * (1 + k)) : 0.0f; }

// The pass on one pixel, as the shader computes it (channels 0..1): e and its 8 neighbours a b c / d . f / g h i.
void cas_vibrance(const float a[3], const float b[3], const float c[3], const float d[3], const float e[3],
                  const float f[3], const float g[3], const float h[3], const float i[3], const Params& p, float out[3]);

class Renderer {
public:
    // The shader and the texture the image is resolved into, for a w x h image of format 'fmt' (the target's); false
    // (and why) when the card lacks ps_3_0 or that format as a render-target texture.
    bool init(IDirect3DDevice9* d, ao::Compile_t compile, const ao::Calls& calls, UINT w, UINT h, D3DFORMAT fmt,
              std::string* why);
    void release();
    bool ready() const { return ps_ != nullptr; }
    UINT width() const { return w_; }
    UINT height() const { return h_; }
    D3DFORMAT format() const { return fmt_; }
    IDirect3DTexture9* source() const { return src_; }   // the resolved image the pass read (last apply)

    // 'target' (w x h, multisampled or not) resolved into the source texture, then drawn over whole by the pass:
    // CAS, then vibrance. Leaves render target 0, the depth-stencil (none), shaders, declaration, texture and sampler
    // 0, pixel shader constants 0-1 and some render states changed: the caller restores.
    void apply(IDirect3DDevice9* d, IDirect3DSurface9* target, const Params& p);

private:
    ao::Calls c_ = {};
    UINT w_ = 0, h_ = 0;
    D3DFORMAT fmt_ = D3DFMT_UNKNOWN;
    IDirect3DTexture9* src_ = nullptr;
    IDirect3DSurface9* src_surf_ = nullptr;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DPixelShader9* ps_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
};

}  // namespace post
}  // namespace rol
