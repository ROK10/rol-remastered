// post.cpp — sharpening and vibrance over a finished image (see post.h).
//
// The target is resolved (StretchRect: a multisampled target's samples averaged) into a texture of its own format,
// and one full-screen pass (ps_3_0, every pixel reading its own texel through VPOS) draws over the whole target:
//   CAS (AMD FidelityFX Contrast Adaptive Sharpening, the version without scaling): of the 3 x 3 neighbourhood a b c /
//   d e f / g h i, the soft minimum and maximum (min / max of the cross plus min / max of all nine, twice the size);
//   amp = sqrt(saturate(min(mn, 2 - mx) / mx)) - small where the neighbourhood nears black or white, so the result
//   stays inside it (no halos), largest on low-contrast detail; weight w = amp x peak on the cross, out =
//   (e + w (b + d + f + h)) / (1 + 4 w): a flat area stays as it is. As AMD does, amp is taken from green and the same
//   weight sharpens all three channels: no hue shifts at coloured edges.
//   vibrance (as ReShade's Vibrance.fx): luma (Rec. 709) + (colour - luma) x (1 + vibrance (1 - saturation)),
//   saturation = max - min of the channels: grey stays grey, dull colours gain the most, vivid ones little.
// Both in the image's own (gamma-encoded) values, as the game hands them to the screen.
#define NOMINMAX
#include "post.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rol {
namespace post {
namespace {

const char kHlsl[] = R"(
float4 g_size : register(c0);     // 1/w, 1/h, w, h
float4 g_post : register(c1);     // CAS peak (0: none), vibrance

sampler2D s_img : register(s0);

float4 vs_main(float4 p : POSITION) : POSITION { return p; }

float3 px(float2 uv) { return tex2Dlod(s_img, float4(uv, 0, 0)).rgb; }

float4 ps_post(float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * g_size.xy;
    float2 dx = float2(g_size.x, 0), dy = float2(0, g_size.y);
    float3 a = px(uv - dx - dy), b = px(uv - dy), c = px(uv + dx - dy);
    float3 d = px(uv - dx), e = px(uv), f = px(uv + dx);
    float3 g = px(uv - dx + dy), h = px(uv + dy), i = px(uv + dx + dy);
    float mn = min(min(min(d.g, e.g), min(f.g, b.g)), h.g);
    float mx = max(max(max(d.g, e.g), max(f.g, b.g)), h.g);
    mn += min(mn, min(min(a.g, c.g), min(g.g, i.g)));
    mx += max(mx, max(max(a.g, c.g), max(g.g, i.g)));
    float amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 1.0 / 1024)));
    float w = amp * g_post.x;
    float3 col = saturate(((b + d + f + h) * w + e) / (1 + 4 * w));
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    float sat = max(col.r, max(col.g, col.b)) - min(col.r, min(col.g, col.b));
    col = saturate(lerp(luma.xxx, col, 1 + g_post.y * (1 - sat)));
    return float4(col, 1);
}
)";

std::string buffer_text(IUnknown* buf) {
    if (!buf) return {};
    void** vt = *reinterpret_cast<void***>(buf);
    auto get_ptr = reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>(vt[3]);
    auto get_size = reinterpret_cast<DWORD(STDMETHODCALLTYPE*)(IUnknown*)>(vt[4]);
    const char* p = static_cast<const char*>(get_ptr(buf));
    return std::string(p ? p : "", p ? strnlen(p, get_size(buf)) : 0);
}

const DWORD* buffer_code(IUnknown* buf) {
    return static_cast<const DWORD*>(
        reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>((*reinterpret_cast<void***>(buf))[3])(buf));
}

void release_unknown(IUnknown* u) {
    if (u) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>((*reinterpret_cast<void***>(u))[2])(u);
}

template <class T>
void safe_release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

IUnknown* compile(ao::Compile_t c, const char* entry, const char* profile, std::string* why) {
    IUnknown *code = nullptr, *errs = nullptr, *table = nullptr;
    const HRESULT hr = c(kHlsl, UINT(sizeof(kHlsl) - 1), nullptr, nullptr, entry, profile, 0, &code, &errs, &table);
    if (FAILED(hr) || !code) {
        if (why) *why = std::string(entry) + ": " + (errs ? buffer_text(errs) : std::string("compile failed"));
        release_unknown(code);
        code = nullptr;
    }
    release_unknown(errs);
    release_unknown(table);
    return code;
}

float sat01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

}  // namespace

void cas_vibrance(const float a[3], const float b[3], const float c[3], const float d[3], const float e[3],
                  const float f[3], const float g[3], const float h[3], const float i[3], const Params& p,
                  float out[3]) {
    float mn = std::min({d[1], e[1], f[1], b[1], h[1]});
    float mx = std::max({d[1], e[1], f[1], b[1], h[1]});
    mn += std::min({mn, a[1], c[1], g[1], i[1]});
    mx += std::max({mx, a[1], c[1], g[1], i[1]});
    const float amp = std::sqrt(sat01(std::min(mn, 2 - mx) / std::max(mx, 1.0f / 1024)));
    const float w = amp * cas_peak(p.sharpen);
    float col[3];
    for (int k = 0; k < 3; ++k) col[k] = sat01(((b[k] + d[k] + f[k] + h[k]) * w + e[k]) / (1 + 4 * w));
    const float luma = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2];
    const float sat = std::max({col[0], col[1], col[2]}) - std::min({col[0], col[1], col[2]});
    for (int k = 0; k < 3; ++k) out[k] = sat01(luma + (col[k] - luma) * (1 + p.vibrance * (1 - sat)));
}

bool Renderer::init(IDirect3DDevice9* d, ao::Compile_t compile_fn, const ao::Calls& calls, UINT w, UINT h, D3DFORMAT fmt,
                    std::string* why) {
    release();
    c_ = calls;
    auto fail = [&](const char* m) {
        if (why && why->empty()) *why = m;
        release();
        return false;
    };
    if (!compile_fn) return fail("no shader compiler");
    if (!c_.stretch_rect) return fail("no StretchRect");
    D3DCAPS9 caps = {};
    d->GetDeviceCaps(&caps);
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
        return fail("no shader model 3");
    w_ = w;
    h_ = h;
    fmt_ = fmt;
    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &src_, nullptr)) ||
        FAILED(src_->GetSurfaceLevel(0, &src_surf_)))
        return fail("the texture to resolve the image into was not made");
    std::string err;
    IUnknown* vs = compile(compile_fn, "vs_main", "vs_3_0", &err);
    IUnknown* ps = vs ? compile(compile_fn, "ps_post", "ps_3_0", &err) : nullptr;
    const bool made = ps && SUCCEEDED(d->CreateVertexShader(buffer_code(vs), &vs_)) &&
                      SUCCEEDED(d->CreatePixelShader(buffer_code(ps), &ps_));
    release_unknown(vs);
    release_unknown(ps);
    if (!made) {
        if (why && why->empty()) *why = err.empty() ? "shaders not made" : err;
        return fail("shaders not made");
    }
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    D3DDECL_END()};
    if (FAILED(d->CreateVertexDeclaration(el, &decl_))) return fail("vertex declaration not made");
    return true;
}

void Renderer::release() {
    safe_release(src_surf_);
    safe_release(src_);
    safe_release(vs_);
    safe_release(ps_);
    safe_release(decl_);
    w_ = h_ = 0;
    fmt_ = D3DFMT_UNKNOWN;
}

void Renderer::apply(IDirect3DDevice9* d, IDirect3DSurface9* target, const Params& p) {
    if (!ready() || !target) return;
    if (FAILED(c_.stretch_rect(d, target, nullptr, src_surf_, nullptr, D3DTEXF_NONE))) return;
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
    c_.set_sampler(d, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    c_.set_sampler(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    c_.set_sampler(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    c_.set_sampler(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    c_.set_sampler(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    c_.set_sampler(d, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    c_.set_sampler(d, 0, D3DSAMP_MAXMIPLEVEL, 0);
    d->SetVertexDeclaration(decl_);
    c_.set_vs(d, vs_);
    const float k[2][4] = {{1.0f / w_, 1.0f / h_, float(w_), float(h_)}, {cas_peak(p.sharpen), p.vibrance, 0, 0}};
    d->SetPixelShaderConstantF(0, &k[0][0], 2);
    // one triangle over the whole target, every side well outside it (multisampled: the edge pixels' samples covered)
    static const float tri[12] = {-2, -2, 0, 1, 6, -2, 0, 1, -2, 6, 0, 1};
    c_.set_rt(d, 0, target);
    c_.set_texture(d, 0, src_);
    c_.set_ps(d, ps_);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
    c_.set_texture(d, 0, nullptr);
}

}  // namespace post
}  // namespace rol
