// ao.h — screen-space ambient occlusion from a depth buffer, D3D9: the part that knows nothing of the game. rolhook's
// ssao.cpp draws the scene's depth into its INTZ surface and composites the result; shadertest.exe runs it on a
// synthetic scene and checks the numbers.
#pragma once

#include <d3d9.h>

#include <string>

namespace rol {
namespace ao {

// D3DXCompileShader of d3dx9_27.dll
typedef HRESULT(WINAPI* Compile_t)(LPCSTR src, UINT len, const void* defines, void* include, LPCSTR entry, LPCSTR profile,
                                   DWORD flags, IUnknown** code, IUnknown** errors, IUnknown** constants);

// The device methods the passes call that rolhook hooks: rolhook hands in the originals behind its hooks (so its
// trackers of the game's state do not see the passes), the bench the device's own. post.cpp uses them too.
struct Calls {
    HRESULT(STDMETHODCALLTYPE* set_rt)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
    HRESULT(STDMETHODCALLTYPE* set_texture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
    HRESULT(STDMETHODCALLTYPE* set_sampler)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
    HRESULT(STDMETHODCALLTYPE* set_viewport)(IDirect3DDevice9*, const D3DVIEWPORT9*);
    HRESULT(STDMETHODCALLTYPE* set_vs)(IDirect3DDevice9*, IDirect3DVertexShader9*);
    HRESULT(STDMETHODCALLTYPE* set_ps)(IDirect3DDevice9*, IDirect3DPixelShader9*);
    HRESULT(STDMETHODCALLTYPE* stretch_rect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                             const RECT*, D3DTEXTUREFILTERTYPE);   // post.cpp's resolve
};

// The camera the depth was drawn with (a D3D perspective projection: z/w = f/(f-n) - n f / ((f-n) z)).
struct View {
    float n, f;     // near / far plane
    float tx, ty;   // tan of half the field of view across / up
};

struct Params {
    float radius;      // world units: how far an occluder counts
    float intensity;   // darkness of a fully occluded point
    float bias;        // cosine: occluders rising less than this above the tangent plane do not count (tessellation)
    float strength;    // composite: 0 = none, 1 = the full AO
};

class Renderer {
public:
    // Shaders and targets for a w x h scene; false (and why) when the card lacks INTZ, G16R16F or ps_3_0.
    bool init(IDirect3DDevice9* d, Compile_t compile, const Calls& calls, UINT w, UINT h, std::string* why);
    void release();
    bool ready() const { return depth_ != nullptr; }
    UINT width() const { return w_; }
    UINT height() const { return h_; }

    IDirect3DSurface9* depth() const { return depth_; }        // INTZ: the depth-stencil the scene's depth goes into
    IDirect3DSurface9* null_target() const { return null_; }   // a colourless render target of the same size
    // A8R8G8B8: in its alpha, what covers the scene and must not be darkened (particles); the composite leaves the
    // scene as it is by that much. Cleared to 0 by the caller each frame.
    IDirect3DSurface9* mask() const { return mask_surf_; }
    IDirect3DTexture9* result() const { return ao_[0]; }       // R = AO, G = depth / far, after the blur

    // AO from the depth, blurred (3 passes into its own targets). Leaves the device's render target 0, depth-stencil,
    // shaders, declaration, textures and samplers 0, viewport and some render states changed: the caller restores.
    void compute(IDirect3DDevice9* d, const View& v, const Params& p);
    // The scene multiplied by lerp(1, AO, strength x (1 - mask)): a full-screen pass onto 'target' (the scene's size,
    // multisampled or not).
    void composite(IDirect3DDevice9* d, IDirect3DSurface9* target, const Params& p);

private:
    void pass(IDirect3DDevice9* d, IDirect3DSurface9* rt, IDirect3DPixelShader9* ps, IDirect3DBaseTexture9* tex);
    void point_sampler(IDirect3DDevice9* d, DWORD s);
    Calls c_ = {};
    UINT w_ = 0, h_ = 0;
    IDirect3DTexture9* depth_tex_ = nullptr;
    IDirect3DSurface9* depth_ = nullptr;
    IDirect3DSurface9* null_ = nullptr;
    IDirect3DTexture9* ao_[2] = {};
    IDirect3DSurface9* ao_surf_[2] = {};
    IDirect3DTexture9* mask_ = nullptr;
    IDirect3DSurface9* mask_surf_ = nullptr;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DPixelShader9* ps_ao_ = nullptr;
    IDirect3DPixelShader9* ps_blur_ = nullptr;
    IDirect3DPixelShader9* ps_comp_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
};

}  // namespace ao
}  // namespace rol
