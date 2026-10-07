// gfxraw.h — the device methods behind rolhook's hooks (gfx.cpp), for rolhook's own drawing: its trackers of the game's
// state (render target, shadow-map stage, ATOC, shaders) do not see it. And ssao.cpp's redraw of a scene draw.
#pragma once

#include <d3d9.h>

namespace rol {

HRESULT STDMETHODCALLTYPE gfx_raw_set_render_target(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s);
HRESULT STDMETHODCALLTYPE gfx_raw_set_texture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t);
HRESULT STDMETHODCALLTYPE gfx_raw_set_sampler(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE type, DWORD v);
HRESULT STDMETHODCALLTYPE gfx_raw_set_viewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp);
HRESULT STDMETHODCALLTYPE gfx_raw_set_vertex_shader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs);
HRESULT STDMETHODCALLTYPE gfx_raw_set_pixel_shader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps);
HRESULT STDMETHODCALLTYPE gfx_raw_stretch_rect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr,
                                               IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f);
HRESULT gfx_raw_draw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT count);
HRESULT gfx_raw_draw_indexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base_vertex, UINT min_index, UINT num_vertices,
                             UINT start, UINT count);
HRESULT gfx_raw_draw_up(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT count, const void* vertices, UINT stride);
HRESULT gfx_raw_draw_indexed_up(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT min_index, UINT num_vertices, UINT count,
                                const void* indices, D3DFORMAT index_format, const void* vertices, UINT stride);

// A scene draw as the game made it, for ssao.cpp to draw it again into its depth or mask.
struct SsaoDraw {
    bool indexed;
    D3DPRIMITIVETYPE type;
    INT base_vertex;
    UINT min_index, num_vertices, start, count;
    const void* up_vertices;   // drawn from memory (DrawPrimitiveUP / DrawIndexedPrimitiveUP)
    UINT up_stride;
    const void* up_indices;
    D3DFORMAT up_index_format;
};
void ssao_redraw(IDirect3DDevice9* d, int action, const SsaoDraw& dr);   // action: what ssao_before_draw returned

}  // namespace rol
