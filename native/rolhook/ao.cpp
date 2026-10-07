// ao.cpp — screen-space ambient occlusion from an INTZ depth buffer (see ao.h).
//
// Pass 1 (ps_3_0, full resolution): the depth turned into view-space positions (the projection's near / far and half
// angles); the surface normal from the neighbours on the side with the smaller depth step (no smearing over edges);
// 12 taps on a spiral over a disc of 'radius' world units (taps closer to the centre, as in Scalable Ambient Obscurance,
// McGuire et al. 2012), turned per pixel by interleaved gradient noise; a tap at v from the point occludes by how far it
// rises above the tangent plane - max(cos(v, n) - bias, 0), a pure angle, so the world's scale does not matter - times
// (1 - v.v / r^2), fading to nothing at the radius (no dark halo where a silhouette stands far in front of the ground).
// The result, and depth / far for the blur, go to a G16R16F target. Passes 2-3: a 9-tap Gaussian across, then down,
// with weights cut where the depth differs (no AO bleeding over silhouettes). Composite: the scene's colour
// multiplied by lerp(1, AO, strength x (1 - mask)) (blend ZERO / SRCCOLOR), drawn at the scene's own resolution and
// sampling; the mask's alpha holds what the caller drew over the scene that must not darken (particles).
// Pixels read their own texel through VPOS: no half-pixel offsets to get right.
#include "ao.h"

#include <cmath>
#include <cstring>

namespace rol {
namespace ao {
namespace {

const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
const D3DFORMAT kNull = static_cast<D3DFORMAT>(MAKEFOURCC('N', 'U', 'L', 'L'));
constexpr int kTaps = 12;      // spiral taps per pixel (the shader takes them two by two)
constexpr float kTurns = 7;    // turns of the spiral over the taps (prime to kTaps: no two on one ray)

const char kHlsl[] = R"(
float4 g_size : register(c0);     // 1/w, 1/h, w, h

float4 vs_main(float4 p : POSITION) : POSITION { return p; }

// ---- AO
sampler2D s_depth : register(s0); // INTZ: z/w
float4 g_lin : register(c1);      // n f, f, f - n, depth past which nothing was drawn
float4 g_proj : register(c2);     // tan half x, tan half y, pixels per unit at depth 1, radius
float4 g_ao : register(c3);       // intensity / taps, bias (cosine), 1 / r^2, eps
float4 g_lim : register(c4);      // largest / smallest disc in pixels
float4 g_disc[6] : register(c5);  // the spiral's 12 points in the unit disc, two per register

float3 at(float2 uv) {
    float z = g_lin.x / (g_lin.y - tex2Dlod(s_depth, float4(uv, 0, 0)).r * g_lin.z);
    return float3((uv.x * 2 - 1) * g_proj.x * z, (1 - uv.y * 2) * g_proj.y * z, z);
}

float occ(float3 v, float3 n) {
    float vv = dot(v, v);
    return saturate(1 - vv * g_ao.z) * max(dot(v, n) * rsqrt(vv + g_ao.w) - g_ao.y, 0);
}

float4 ps_ao(float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * g_size.xy;
    float3 P = at(uv);
    float3 pr = at(uv + float2(g_size.x, 0)), pl = at(uv - float2(g_size.x, 0));
    float3 pd = at(uv + float2(0, g_size.y)), pu = at(uv - float2(0, g_size.y));
    float3 dx = abs(pr.z - P.z) < abs(P.z - pl.z) ? pr - P : P - pl;
    float3 dy = abs(pd.z - P.z) < abs(P.z - pu.z) ? pd - P : P - pu;
    float3 n = normalize(cross(dx, dy));
    float rpix = clamp(g_proj.z * g_proj.w / P.z, g_lim.y, g_lim.x);
    float a = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715)))) * 6.2831853;
    float2 cs;
    sincos(a, cs.y, cs.x);
    float2 ex = float2(cs.x, cs.y) * rpix * g_size.xy, ey = float2(-cs.y, cs.x) * rpix * g_size.xy;
    float sum = 0;
    for (int i = 0; i < 6; ++i) {
        float4 d = g_disc[i];
        sum += occ(at(uv + d.x * ex + d.y * ey) - P, n);
        sum += occ(at(uv + d.z * ex + d.w * ey) - P, n);
    }
    // nothing drawn (the far plane): unoccluded (the old compiler takes no early return)
    return P.z >= g_lin.w ? float4(1, 1, 0, 0) : float4(saturate(1 - sum * g_ao.x), P.z / g_lin.y, 0, 0);
}

// ---- blur: 9 taps one way, cut where the depth differs
sampler2D s_ao : register(s0);
float4 g_blur : register(c1);     // step (1/w, 0) or (0, 1/h), sharpness

float4 ps_blur(float2 vpos : VPOS) : COLOR {
    float2 uv = (vpos + 0.5) * g_size.xy;
    float2 c = tex2Dlod(s_ao, float4(uv, 0, 0)).rg;
    const float w[5] = {0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162};
    float sum = c.x * w[0], ws = w[0];
    for (int k = 1; k < 5; ++k) {
        float2 a = tex2Dlod(s_ao, float4(uv + k * g_blur.xy, 0, 0)).rg;
        float2 b = tex2Dlod(s_ao, float4(uv - k * g_blur.xy, 0, 0)).rg;
        float wa = w[k] * saturate(1 - abs(a.y - c.y) * g_blur.z / c.y);
        float wb = w[k] * saturate(1 - abs(b.y - c.y) * g_blur.z / c.y);
        sum += a.x * wa + b.x * wb;
        ws += wa + wb;
    }
    return c.y >= 0.999 ? float4(1, c.y, 0, 0) : float4(sum / ws, c.y, 0, 0);
}

// ---- composite: the colour times lerp(1, AO, strength (1 - mask))
sampler2D s_mask : register(s1);  // alpha: particles over the scene
float4 g_comp : register(c1);     // strength

float4 ps_comp(float2 vpos : VPOS) : COLOR {
    float4 uv = float4((vpos + 0.5) * g_size.xy, 0, 0);
    float k = lerp(1, tex2Dlod(s_ao, uv).r, g_comp.x * (1 - tex2Dlod(s_mask, uv).a));
    return float4(k, k, k, 1);
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

IUnknown* compile(Compile_t c, const char* entry, const char* profile, std::string* why) {
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

}  // namespace

bool Renderer::init(IDirect3DDevice9* d, Compile_t compile_fn, const Calls& calls, UINT w, UINT h, std::string* why) {
    release();
    c_ = calls;
    auto fail = [&](const char* w) {
        if (why && why->empty()) *why = w;
        release();
        return false;
    };
    if (!compile_fn) return fail("no shader compiler");
    D3DCAPS9 caps = {};
    d->GetDeviceCaps(&caps);
    if (caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
        return fail("no shader model 3");
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    d->GetDirect3D(&d3d);
    d->GetCreationParameters(&cp);
    if (d3d) {
        const bool intz = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8,
                                                           D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kIntz));
        const bool g16 = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8,
                                                          D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_G16R16F));
        d3d->Release();
        if (!intz) return fail("the card has no INTZ depth textures");
        if (!g16) return fail("the card has no G16R16F render targets");
    }
    w_ = w;
    h_ = h;
    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kIntz, D3DPOOL_DEFAULT, &depth_tex_, nullptr)) ||
        FAILED(depth_tex_->GetSurfaceLevel(0, &depth_)))
        return fail("INTZ depth texture not made");
    if (FAILED(d->CreateRenderTarget(w, h, kNull, D3DMULTISAMPLE_NONE, 0, FALSE, &null_, nullptr)) &&
        FAILED(d->CreateRenderTarget(w, h, D3DFMT_R5G6B5, D3DMULTISAMPLE_NONE, 0, FALSE, &null_, nullptr)))
        return fail("colourless render target not made");
    for (int i = 0; i < 2; ++i)
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &ao_[i], nullptr)) ||
            FAILED(ao_[i]->GetSurfaceLevel(0, &ao_surf_[i])))
            return fail("AO targets not made");
    if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &mask_, nullptr)) ||
        FAILED(mask_->GetSurfaceLevel(0, &mask_surf_)))
        return fail("mask target not made");
    std::string err;
    IUnknown* vs = compile(compile_fn, "vs_main", "vs_3_0", &err);
    IUnknown* pa = vs ? compile(compile_fn, "ps_ao", "ps_3_0", &err) : nullptr;
    IUnknown* pb = pa ? compile(compile_fn, "ps_blur", "ps_3_0", &err) : nullptr;
    IUnknown* pc = pb ? compile(compile_fn, "ps_comp", "ps_3_0", &err) : nullptr;
    bool made = pc && SUCCEEDED(d->CreateVertexShader(buffer_code(vs), &vs_)) &&
                SUCCEEDED(d->CreatePixelShader(buffer_code(pa), &ps_ao_)) &&
                SUCCEEDED(d->CreatePixelShader(buffer_code(pb), &ps_blur_)) &&
                SUCCEEDED(d->CreatePixelShader(buffer_code(pc), &ps_comp_));
    release_unknown(vs);
    release_unknown(pa);
    release_unknown(pb);
    release_unknown(pc);
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
    safe_release(depth_);
    safe_release(depth_tex_);
    safe_release(null_);
    for (int i = 0; i < 2; ++i) {
        safe_release(ao_surf_[i]);
        safe_release(ao_[i]);
    }
    safe_release(mask_surf_);
    safe_release(mask_);
    safe_release(vs_);
    safe_release(ps_ao_);
    safe_release(ps_blur_);
    safe_release(ps_comp_);
    safe_release(decl_);
    w_ = h_ = 0;
}

void Renderer::pass(IDirect3DDevice9* d, IDirect3DSurface9* rt, IDirect3DPixelShader9* ps, IDirect3DBaseTexture9* tex) {
    // one triangle over the whole target, every side well outside it (multisampled: the edge pixels' samples covered)
    static const float tri[12] = {-2, -2, 0, 1, 6, -2, 0, 1, -2, 6, 0, 1};
    c_.set_rt(d, 0, rt);
    c_.set_texture(d, 0, tex);
    c_.set_ps(d, ps);
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
}

void Renderer::point_sampler(IDirect3DDevice9* d, DWORD s) {
    c_.set_sampler(d, s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    c_.set_sampler(d, s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    c_.set_sampler(d, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    c_.set_sampler(d, s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    c_.set_sampler(d, s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    c_.set_sampler(d, s, D3DSAMP_SRGBTEXTURE, FALSE);
    c_.set_sampler(d, s, D3DSAMP_MAXMIPLEVEL, 0);
}

void Renderer::compute(IDirect3DDevice9* d, const View& v, const Params& p) {
    if (!ready()) return;
    d->SetDepthStencilSurface(nullptr);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
    point_sampler(d, 0);
    d->SetVertexDeclaration(decl_);
    c_.set_vs(d, vs_);
    const float r = p.radius > 1 ? p.radius : 1;
    float c[11][4] = {};
    c[0][0] = 1.0f / w_, c[0][1] = 1.0f / h_, c[0][2] = float(w_), c[0][3] = float(h_);
    c[1][0] = v.n * v.f, c[1][1] = v.f, c[1][2] = v.f - v.n, c[1][3] = v.f * 0.999f;
    c[2][0] = v.tx, c[2][1] = v.ty, c[2][2] = h_ / (2 * v.ty), c[2][3] = r;
    c[3][0] = p.intensity / kTaps, c[3][1] = p.bias, c[3][2] = 1 / (r * r), c[3][3] = 1e-4f * r * r;
    c[4][0] = 0.15f * h_, c[4][1] = 1.5f;
    for (int i = 0; i < kTaps; ++i) {   // the spiral: radius (i + 0.5) / taps, angle kTurns turns over the taps
        const double a = (i + 0.5) / kTaps, t = a * kTurns * 6.283185307179586;
        c[5 + i / 2][(i % 2) * 2] = float(a * std::cos(t));
        c[5 + i / 2][(i % 2) * 2 + 1] = float(a * std::sin(t));
    }
    d->SetPixelShaderConstantF(0, &c[0][0], 11);
    pass(d, ao_surf_[0], ps_ao_, depth_tex_);
    const float sharp = 16;
    const float across[4] = {1.0f / w_, 0, sharp, 0}, down[4] = {0, 1.0f / h_, sharp, 0};
    d->SetPixelShaderConstantF(1, across, 1);
    pass(d, ao_surf_[1], ps_blur_, ao_[0]);
    d->SetPixelShaderConstantF(1, down, 1);
    pass(d, ao_surf_[0], ps_blur_, ao_[1]);
    c_.set_texture(d, 0, nullptr);
}

void Renderer::composite(IDirect3DDevice9* d, IDirect3DSurface9* target, const Params& p) {
    if (!ready() || !target) return;
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    const float k[4] = {p.strength, 0, 0, 0};
    d->SetPixelShaderConstantF(1, k, 1);
    point_sampler(d, 1);
    c_.set_texture(d, 1, mask_);
    pass(d, target, ps_comp_, ao_[0]);
    c_.set_texture(d, 0, nullptr);
    c_.set_texture(d, 1, nullptr);
}

}  // namespace ao
}  // namespace rol
