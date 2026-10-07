// Ambient occlusion in the battle scene ([graphics] ssao = 0 off | 1 light | 2 medium | 3 strong): contact shadows
// where buildings stand on the ground, in creases and corners, under trees, between units.
//
// The scene is drawn multisampled (MSAA 8x, D24S8): a depth buffer D3D9 cannot read. So from the terrain's base pass
// (terrain_2) on, every draw into the scene that writes depth - the terrain, buildings, units, trees, grass, rocks - is
// drawn a second time right after the game's, with the game's own shaders, textures and states, into rolhook's
// single-sampled INTZ depth with a colourless render target: alpha-tested leaves and grates cut their holes there too.
// Skipped: the terrain's shadow pass, splats and decals, the units' silhouettes behind buildings (unitocclusion).
// Particles, distortions and shaderless overlays drawn into the scene are drawn again into a mask instead - their
// alpha, accumulated 'over', depth-tested against the INTZ - for the engine sorts some buildings (those with smoke on
// them) in among the particles, after the first one. When the scene's render target is left for the post-processing
// (or the interface's first pretransformed draw comes, if there is none), ao.cpp computes the AO from the depth with
// the camera's projection (near / far / half angles read from the view camera) and multiplies the scene by
// lerp(1, AO, strength x (1 - mask)): smoke, fire, explosions and overlays stay as they are; the interface comes after.
// The states the passes touch are saved and put back one by one (render target and depth-stencil included). A frame
// without the terrain (menus, the campaign map) gets nothing. Levels: radius 60 / 90 / 120 world units, intensity
// 2 / 3 / 4; [graphics] ssao_radius / ssao_intensity (percent) override them for tests. [debug] gpu_timing=1 times the
// scene, the passes and the rest on the GPU (timestamp queries; with ssao=0 too, for the comparison).
// The scene's end is also where the sharpening and vibrance go ([graphics] sharpen / vibrance = 0 off | 1 / 2 / 3,
// post.cpp): after the AO, the scene resolved and drawn over by one pass - before the post-processing (bloom) and the
// interface, so the interface is left alone. Either one on is enough for the scene to be followed.
#include "rolhook.h"

#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "ao.h"
#include "gfxraw.h"
#include "post.h"

namespace rol {
namespace {

enum Class : uint8_t { C_NONE = 0, C_TERRAIN, C_SKIP, C_FX };
enum Action { A_NONE = 0, A_DEPTH, A_MASK };

SRWLOCK g_class_lock = SRWLOCK_INIT;
std::unordered_map<void*, uint8_t> g_class;   // the effects' shaders -> what their draws are
uint8_t g_vs_class = C_NONE, g_ps_class = C_NONE;
bool g_vs_set = false;                         // a vertex shader is set (no: fixed function)
IDirect3DSurface9* g_rt0 = nullptr;            // render target 0 as the game last set it (identity only)

volatile LONG g_ssao_level = 0;
int g_radius_set = 0, g_intensity_set = 0;     // [graphics] ssao_radius / ssao_intensity, read with the level (the ini
                                               // layers are read from disk on every config_int: 0.4 ms each)
int g_phase = 0;                               // 0 before the scene, 1 the scene (depth drawn), 2 done this frame
IDirect3DSurface9* g_scene_rt = nullptr;       // AddRef'd while the frame lasts
IDirect3DSurface9* g_scene_ds = nullptr;
ao::Renderer g_ao;
bool g_failed = false;                         // the renderer could not be made: off until the next Reset
volatile LONG g_sharpen = 0, g_vibrance = 0;   // [graphics] sharpen / vibrance levels (post.cpp)
post::Renderer g_post;
bool g_post_failed = false;
int g_gpu_timing = -1;
int g_probe = -1;                              // [debug] ssao_probe: the AO read back once, its numbers logged
uint32_t g_probe_count = 0;
bool g_told_target = false;

// statistics
struct Win {
    uint32_t frames = 0, composited = 0, posted = 0;
    uint64_t depth = 0, masked = 0, late = 0;
    double redraw_ms = 0, pass_ms = 0;
    uint32_t by_target = 0, by_ui = 0;
};
Win g_win, g_total;
double g_win_start = -1;
LARGE_INTEGER g_qpf = {};

double qpc_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    return double(t.QuadPart) * 1000.0 / double(g_qpf.QuadPart);
}

void add_win(Win& to, const Win& w) {
    to.frames += w.frames;
    to.composited += w.composited;
    to.posted += w.posted;
    to.depth += w.depth;
    to.masked += w.masked;
    to.late += w.late;
    to.redraw_ms += w.redraw_ms;
    to.pass_ms += w.pass_ms;
    to.by_target += w.by_target;
    to.by_ui += w.by_ui;
}

// ---- GPU timing: timestamps at the scene's start, before and after the passes, at Present; read 3 frames later
struct GpuFrame {
    IDirect3DQuery9* disjoint = nullptr;
    IDirect3DQuery9* freq = nullptr;
    IDirect3DQuery9* ts[4] = {};
    bool issued[4] = {};
    bool open = false;
};
constexpr int kGpuRing = 4;
GpuFrame g_gpu[kGpuRing];
int g_gpu_cur = 0;
struct GpuSum {
    uint32_t n = 0;
    double scene = 0, passes = 0, rest = 0, frame = 0;
} g_gpu_win, g_gpu_total;

void gpu_release() {
    for (auto& f : g_gpu) {
        if (f.disjoint) f.disjoint->Release();
        if (f.freq) f.freq->Release();
        for (auto*& q : f.ts) {
            if (q) q->Release();
            q = nullptr;
        }
        f = GpuFrame();
    }
}

void gpu_stamp(IDirect3DDevice9* d, int k) {
    if (g_gpu_timing <= 0) return;
    GpuFrame& f = g_gpu[g_gpu_cur];
    if (k == 0) {
        if (!f.disjoint && FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &f.disjoint))) return;
        if (!f.freq && FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &f.freq))) return;
        for (auto*& q : f.ts)
            if (!q && FAILED(d->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q))) return;
        f.disjoint->Issue(D3DISSUE_BEGIN);
        f.open = true;
        for (bool& b : f.issued) b = false;
    }
    if (!f.open || !f.ts[k]) return;
    f.ts[k]->Issue(D3DISSUE_END);
    f.issued[k] = true;
}

void gpu_end_frame(IDirect3DDevice9* d) {
    if (g_gpu_timing <= 0) return;
    GpuFrame& f = g_gpu[g_gpu_cur];
    if (f.open) {
        gpu_stamp(d, 3);
        f.freq->Issue(D3DISSUE_END);
        f.disjoint->Issue(D3DISSUE_END);
    }
    g_gpu_cur = (g_gpu_cur + 1) % kGpuRing;
    GpuFrame& o = g_gpu[g_gpu_cur];   // the oldest: issued kGpuRing - 1 frames ago
    if (!o.open) return;
    o.open = false;
    BOOL disjoint = TRUE;
    UINT64 freq = 0, t[4] = {};
    if (o.disjoint->GetData(&disjoint, sizeof(disjoint), 0) != S_OK || disjoint) return;
    if (o.freq->GetData(&freq, sizeof(freq), 0) != S_OK || !freq) return;
    for (int k = 0; k < 4; ++k)
        if (o.issued[k] && o.ts[k]->GetData(&t[k], sizeof(t[k]), 0) != S_OK) return;
    if (!o.issued[0] || !o.issued[3]) return;
    const double ms = 1000.0 / double(freq);
    const UINT64 t1 = o.issued[1] ? t[1] : t[3], t2 = o.issued[2] ? t[2] : t1;
    for (GpuSum* s : {&g_gpu_win, &g_gpu_total}) {
        s->n++;
        s->scene += double(t1 - t[0]) * ms;
        s->passes += double(t2 - t1) * ms;
        s->rest += double(t[3] - t2) * ms;
        s->frame += double(t[3] - t[0]) * ms;
    }
}

// ---- levels
void level_params(int level, ao::Params& p) {
    // calibrated on shadertest's cube: the ground 2 units from a wall ~0.75 / 0.65 / 0.55
    static const float radius[4] = {0, 60, 90, 120}, intensity[4] = {0, 2.0f, 3.0f, 4.0f}, strength[4] = {0, 1, 1, 1};
    p.radius = radius[level];
    p.intensity = intensity[level];
    p.strength = strength[level];
    p.bias = 0.1f;
    if (g_radius_set > 0) p.radius = float(g_radius_set);
    if (g_intensity_set > 0) p.intensity = g_intensity_set / 100.0f;   // percent
}

// The sharpening's amount and the vibrance by level (post.h): CAS amount 0.4 / 0.8 / 1.6 (AMD's own sharpness 0 is
// 1, 0.5 is 1.6), vibrance 0.12 / 0.25 / 0.4.
post::Params post_params() {
    static const float sharpen[4] = {0, 0.4f, 0.8f, 1.6f}, vibrance[4] = {0, 0.12f, 0.25f, 0.4f};
    return {sharpen[int(g_sharpen)], vibrance[int(g_vibrance)]};
}

bool post_on() { return int(g_sharpen) > 0 || int(g_vibrance) > 0; }

Class classify(const char* effect) {
    if (!effect) return C_NONE;
    const std::string e = effect;
    auto has = [&](const char* s) { return e.find(s) != std::string::npos; };
    if (has("particle") || has("distortion")) return C_FX;
    if (has("terrain_shadow_pass") || has("splat") || has("unitocclusion") || has("z_overlays") || has("dst_") ||
        has("shadowmap") || has("shadowfloat") || has("floatshadow") || has("postprocess") || has("fowblur"))
        return C_SKIP;
    if (e == "terrain_2.fxo") return C_TERRAIN;
    return C_NONE;
}

Class class_of(void* sh) {
    if (!sh) return C_NONE;
    AcquireSRWLockShared(&g_class_lock);
    auto it = g_class.find(sh);
    const Class c = it == g_class.end() ? C_NONE : Class(it->second);
    ReleaseSRWLockShared(&g_class_lock);
    return c;
}

void drop_frame_refs() {
    if (g_scene_rt) g_scene_rt->Release();
    if (g_scene_ds) g_scene_ds->Release();
    g_scene_rt = g_scene_ds = nullptr;
}

ao::Calls raw_calls() {
    ao::Calls c;
    c.set_rt = gfx_raw_set_render_target;
    c.set_texture = gfx_raw_set_texture;
    c.set_sampler = gfx_raw_set_sampler;
    c.set_viewport = gfx_raw_set_viewport;
    c.set_vs = gfx_raw_set_vertex_shader;
    c.set_ps = gfx_raw_set_pixel_shader;
    c.stretch_rect = gfx_raw_stretch_rect;
    return c;
}

bool make_renderer(IDirect3DDevice9* d, UINT w, UINT h) {
    if (g_ao.ready() && g_ao.width() == w && g_ao.height() == h) return true;
    if (g_failed) return false;
    HMODULE dx = GetModuleHandleW(L"d3dx9_27.dll");
    auto compile = dx ? reinterpret_cast<ao::Compile_t>(::GetProcAddress(dx, "D3DXCompileShader")) : nullptr;
    std::string why;
    if (!g_ao.init(d, compile, raw_calls(), w, h, &why)) {
        g_failed = true;
        RLOG(CH_D3D, L_WARN, "ssao: off - %.400s", why.c_str());
        return false;
    }
    RLOG(CH_D3D, L_INFO, "ssao: depth (INTZ), mask, AO targets (G16R16F) and shaders made for %ux%u", w, h);
    return true;
}

// The sharpening and vibrance pass for the scene's target (its size and format).
bool make_post(IDirect3DDevice9* d) {
    D3DSURFACE_DESC sd = {};
    if (!g_scene_rt || FAILED(g_scene_rt->GetDesc(&sd))) return false;
    if (g_post.ready() && g_post.width() == sd.Width && g_post.height() == sd.Height && g_post.format() == sd.Format)
        return true;
    if (g_post_failed) return false;
    HMODULE dx = GetModuleHandleW(L"d3dx9_27.dll");
    auto compile = dx ? reinterpret_cast<ao::Compile_t>(::GetProcAddress(dx, "D3DXCompileShader")) : nullptr;
    std::string why;
    if (!g_post.init(d, compile, raw_calls(), sd.Width, sd.Height, sd.Format, &why)) {
        g_post_failed = true;
        RLOG(CH_D3D, L_WARN, "sharpening / vibrance: off - %.400s", why.c_str());
        return false;
    }
    RLOG(CH_D3D, L_INFO, "sharpening / vibrance: resolve texture (format %u) and shader made for %ux%u", unsigned(sd.Format),
         sd.Width, sd.Height);
    return true;
}

// The scene starts: the terrain's first draw into the backbuffer. The INTZ depth and the mask are cleared.
bool begin_scene(IDirect3DDevice9* d) {
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *ds = nullptr;
    d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    d->GetRenderTarget(0, &rt);
    d->GetDepthStencilSurface(&ds);
    const bool ok = bb && rt == bb && ds;
    if (bb) bb->Release();
    if (!ok && !g_told_target) {
        g_told_target = true;
        RLOG(CH_D3D, L_INFO, "ssao: the terrain drawn into a render target other than the backbuffer (%p) - no AO there",
             (void*)rt);
    }
    if (!ok) {
        if (rt) rt->Release();
        if (ds) ds->Release();
        return false;
    }
    g_scene_rt = rt;
    g_scene_ds = ds;
    g_rt0 = rt;
    gpu_stamp(d, 0);
    if (!int(g_ssao_level)) return true;   // the GPU timing or the sharpening / vibrance only
    D3DSURFACE_DESC sd = {};
    rt->GetDesc(&sd);
    if (!make_renderer(d, sd.Width, sd.Height)) return true;
    D3DVIEWPORT9 vp;
    d->GetViewport(&vp);
    gfx_raw_set_render_target(d, 0, g_ao.mask());
    d->SetDepthStencilSurface(g_ao.depth());
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    gfx_raw_set_render_target(d, 0, g_scene_rt);
    d->SetDepthStencilSurface(g_scene_ds);
    gfx_raw_set_viewport(d, &vp);
    return true;
}

// ---- the states the passes change, saved and put back one by one (a D3DSBT_ALL state block cost 0.7 ms a frame)
const D3DRENDERSTATETYPE kStates[] = {
    D3DRS_ZENABLE,      D3DRS_ZWRITEENABLE,     D3DRS_STENCILENABLE,  D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE,
    D3DRS_SRGBWRITEENABLE, D3DRS_FOGENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_FILLMODE, D3DRS_MULTISAMPLEMASK,
    D3DRS_BLENDOP,      D3DRS_SRCBLEND,         D3DRS_DESTBLEND};
const D3DSAMPLERSTATETYPE kSamplers[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU,
                                        D3DSAMP_ADDRESSV,  D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXMIPLEVEL};
constexpr int kNs = sizeof(kStates) / sizeof(kStates[0]), kNsamp = sizeof(kSamplers) / sizeof(kSamplers[0]);

struct Saved {
    DWORD rs[kNs] = {};
    DWORD samp[2][kNsamp] = {};
    IDirect3DBaseTexture9* tex[2] = {};
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT vb_off = 0, vb_stride = 0;
    float psc[11][4] = {};
    D3DVIEWPORT9 vp = {};
};

void save(IDirect3DDevice9* d, Saved& s) {
    for (int i = 0; i < kNs; ++i) d->GetRenderState(kStates[i], &s.rs[i]);
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < kNsamp; ++i) d->GetSamplerState(t, kSamplers[i], &s.samp[t][i]);
        d->GetTexture(t, &s.tex[t]);
    }
    d->GetVertexShader(&s.vs);
    d->GetPixelShader(&s.ps);
    d->GetVertexDeclaration(&s.decl);
    d->GetFVF(&s.fvf);
    d->GetStreamSource(0, &s.vb, &s.vb_off, &s.vb_stride);
    d->GetPixelShaderConstantF(0, &s.psc[0][0], 11);
    d->GetViewport(&s.vp);
}

void restore(IDirect3DDevice9* d, Saved& s) {
    for (int i = 0; i < kNs; ++i) d->SetRenderState(kStates[i], s.rs[i]);
    for (int t = 0; t < 2; ++t) {
        for (int i = 0; i < kNsamp; ++i) gfx_raw_set_sampler(d, t, kSamplers[i], s.samp[t][i]);
        gfx_raw_set_texture(d, t, s.tex[t]);
        if (s.tex[t]) s.tex[t]->Release();
    }
    gfx_raw_set_vertex_shader(d, s.vs);
    gfx_raw_set_pixel_shader(d, s.ps);
    if (s.fvf) d->SetFVF(s.fvf);
    else d->SetVertexDeclaration(s.decl);
    d->SetStreamSource(0, s.vb, s.vb_off, s.vb_stride);
    d->SetPixelShaderConstantF(0, &s.psc[0][0], 11);
    gfx_raw_set_viewport(d, &s.vp);
    for (IUnknown* u : {static_cast<IUnknown*>(s.vs), static_cast<IUnknown*>(s.ps), static_cast<IUnknown*>(s.decl),
                        static_cast<IUnknown*>(s.vb)})
        if (u) u->Release();
}

void probe(IDirect3DDevice9* d, float far_plane);

// AO from the scene's depth, the scene multiplied by it but where the mask covers it; then the sharpening and
// vibrance over the scene; everything put back.
void composite(IDirect3DDevice9* d, bool ui) {
    g_phase = 2;
    gpu_stamp(d, 1);
    const int level = int(g_ssao_level);
    float n = 0, f = 0, tx = 0, ty = 0;
    const bool ao_on = level > 0 && g_ao.ready() && view_projection(&n, &f, &tx, &ty);
    const bool post = post_on() && make_post(d);
    if (ao_on || post) {
        const double t0 = qpc_ms();
        Saved s;
        save(d, s);
        if (ao_on) {
            ao::View v = {n, f, tx, ty};
            ao::Params p;
            level_params(level, p);
            g_ao.compute(d, v, p);
            if (g_probe < 0) g_probe = config_int(L"debug", L"ssao_probe", 0, true);
            if (g_probe > 0 && ++g_probe_count == 600) probe(d, f);
            g_ao.composite(d, g_scene_rt, p);
            g_win.composited++;
        }
        if (post) {
            g_post.apply(d, g_scene_rt, post_params());
            g_win.posted++;
        }
        gfx_raw_set_render_target(d, 0, g_scene_rt);
        d->SetDepthStencilSurface(g_scene_ds);
        restore(d, s);
        g_win.pass_ms += qpc_ms() - t0;
        if (ui) g_win.by_ui++;
        else g_win.by_target++;
    }
    gpu_stamp(d, 2);
}

// A fixed-function draw of pretransformed vertices (XYZRHW, POSITIONT): the interface.
bool pretransformed(IDirect3DDevice9* d) {
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    if (fvf) return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    IDirect3DVertexDeclaration9* decl = nullptr;
    d->GetVertexDeclaration(&decl);
    if (!decl) return false;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT k = 0;
    bool pt = false;
    if (SUCCEEDED(decl->GetDeclaration(el, &k)))
        for (UINT i = 0; i < k && el[i].Stream != 0xFF; ++i) pt |= el[i].Usage == D3DDECLUSAGE_POSITIONT;
    decl->Release();
    return pt;
}

float half_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    if (e == 0) return (s ? -1.0f : 1.0f) * std::ldexp(float(m), -24);
    if (e == 31) return m ? NAN : (s ? -INFINITY : INFINITY);
    uint32_t b = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

// [debug] ssao_probe=1: once, 600 composites in (a few seconds of the battle), the AO read back: view depths along the
// screen's middle column against a flat ground at the look-at point's height (the camera's eye and target), the AO's
// mean, low tenth and share of darkened pixels, and pixels with nothing drawn.
void probe(IDirect3DDevice9* d, float far_plane) {
    IDirect3DSurface9 *src = nullptr, *sys = nullptr;
    const UINT w = g_ao.width(), h = g_ao.height();
    if (FAILED(g_ao.result()->GetSurfaceLevel(0, &src)) ||
        FAILED(d->CreateOffscreenPlainSurface(w, h, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) ||
        FAILED(d->GetRenderTargetData(src, sys))) {
        RLOG(CH_D3D, L_WARN, "ssao probe: the AO could not be read back");
        if (src) src->Release();
        if (sys) sys->Release();
        return;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        uint64_t hist[101] = {}, total = 0, far_px = 0, dark = 0;
        double sum = 0;
        for (UINT y = 0; y < h; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < w; ++x) {
                const float a = half_float(row[x * 2]), z = half_float(row[x * 2 + 1]);
                if (z >= 0.999f) {
                    ++far_px;
                    continue;
                }
                const float c = a < 0 ? 0 : (a > 1 ? 1 : a);
                hist[int(c * 100 + 0.5f)]++;
                sum += c;
                ++total;
                if (c < 0.95f) ++dark;
            }
        }
        double p10 = 1;
        uint64_t acc = 0;
        for (int i = 0; i <= 100; ++i) {
            acc += hist[i];
            if (acc >= total / 10) {
                p10 = i / 100.0;
                break;
            }
        }
        auto depth_at = [&](UINT x, UINT y) {
            const uint16_t* r = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch);
            return half_float(r[x * 2 + 1]) * far_plane;
        };
        // the middle column at 10 heights: measured depth, and a flat ground's at the target's height
        float eye[3], target[3], n = 0, f = 0, tx = 0, ty = 0;
        char col[600] = "";
        if (view_eye_target(eye, target) && view_projection(&n, &f, &tx, &ty)) {
            double fw[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
            const double fl = sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
            for (double& c : fw) c /= fl;
            double up[3] = {-fw[2] * fw[0], -fw[2] * fw[1], 1 - fw[2] * fw[2]};   // world z up, made square to fw
            const double ul = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            for (double& c : up) c /= ul;
            int len = 0;
            for (int k = 1; k <= 9; ++k) {
                const UINT y = UINT(h * k / 10.0);
                const double ndc_y = 1 - 2 * (y + 0.5) / h;
                const double dz = up[2] * ndc_y * ty + fw[2];   // the ray's z per unit of view depth
                const double expect = dz < 0 ? (target[2] - eye[2]) / dz : -1;
                len += _snprintf_s(col + len, sizeof(col) - len, _TRUNCATE, " %d%%: %.0f/%.0f", k * 10,
                                   depth_at(w / 2, y), expect);
            }
        }
        RLOG(CH_D3D, L_INFO,
             "ssao probe: view depth measured / flat ground at the target's height, down the middle column:%s; AO mean "
             "%.3f, low tenth below %.2f, %.1f%% of the pixels darker than 0.95; %.1f%% with nothing drawn",
             col, total ? sum / total : 1.0, p10, total ? 100.0 * dark / total : 0.0, 100.0 * far_px / (double(w) * h));
        sys->UnlockRect();
    }
    src->Release();
    sys->Release();
}

void log_window(bool final) {
    Win& w = final ? g_total : g_win;
    GpuSum& gs = final ? g_gpu_total : g_gpu_win;
    if (!w.frames) return;
    const double f = w.frames;
    char gpu[200] = "";
    if (gs.n)
        _snprintf_s(gpu, _TRUNCATE, "; GPU per frame: scene %.3f ms, passes (AO, sharpening / vibrance) %.3f, the rest "
                    "%.3f, all %.3f (%u frames)",
                    gs.scene / gs.n, gs.passes / gs.n, gs.rest / gs.n, gs.frame / gs.n, gs.n);
    RLOG(CH_D3D, L_INFO,
         "ssao%s: level %ld, sharpening %ld, vibrance %ld; %u frames, AO in %u, sharpening / vibrance in %u (leaving the "
         "scene's target %u, at the interface %u); per frame %.1f draws redrawn into the depth, %.1f into the mask (CPU "
         "%.3f ms), passes CPU %.3f ms; %.1f depth-writing draws per frame after the AO%s",
         final ? " (whole session)" : "", long(g_ssao_level), long(g_sharpen), long(g_vibrance), w.frames, w.composited,
         w.posted, w.by_target, w.by_ui, w.depth / f, w.masked / f, w.redraw_ms / f, w.pass_ms / f, w.late / f, gpu);
}

}  // namespace

void ssao_shader_created(void* sh, const char* effect) {
    const Class c = classify(effect);
    AcquireSRWLockExclusive(&g_class_lock);
    if (c == C_NONE) g_class.erase(sh);
    else g_class[sh] = c;
    ReleaseSRWLockExclusive(&g_class_lock);
}

void ssao_set_vs(void* vs) {
    g_vs_set = vs != nullptr;
    g_vs_class = class_of(vs);
}

void ssao_set_ps(void* ps) { g_ps_class = class_of(ps); }

void ssao_set_rt(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    if (i != 0) return;
    if (g_phase == 1 && g_rt0 == g_scene_rt && s != g_scene_rt) composite(d, false);
    g_rt0 = s;
}

int ssao_before_draw(IDirect3DDevice9* d) {
    if (g_phase == 2) {
        if (g_scene_rt && g_rt0 == g_scene_rt && int(g_ssao_level) > 0) {
            DWORD zw = 0;
            d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
            if (zw) g_win.late++;
        }
        return A_NONE;
    }
    if (!int(g_ssao_level) && g_gpu_timing <= 0 && !post_on()) return A_NONE;
    const uint8_t cls = g_vs_class > g_ps_class ? g_vs_class : g_ps_class;
    if (g_phase == 0) {
        if (cls != C_TERRAIN || !g_vs_set) return A_NONE;
        if (!begin_scene(d)) return A_NONE;
        g_phase = 1;
    }
    if (g_rt0 != g_scene_rt) return A_NONE;
    if (!g_vs_set && pretransformed(d)) {   // the interface begins: the scene is done
        composite(d, true);
        return A_NONE;
    }
    if (cls == C_SKIP || !int(g_ssao_level) || !g_ao.ready()) return A_NONE;
    DWORD ze = 0, zw = 0;
    d->GetRenderState(D3DRS_ZENABLE, &ze);
    d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    if (ze && zw) return A_DEPTH;
    if (cls == C_FX || !g_vs_set) {   // over the scene: masked, unless it leaves the colour as it is
        DWORD cw = 0, ab = 0, sb = 0, db = 0;
        d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
        d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        d->GetRenderState(D3DRS_SRCBLEND, &sb);
        d->GetRenderState(D3DRS_DESTBLEND, &db);
        if (!(cw & 7) || (ab && sb == D3DBLEND_ZERO && db == D3DBLEND_ONE)) return A_NONE;
        return A_MASK;
    }
    return A_NONE;
}

void ssao_redraw(IDirect3DDevice9* d, int action, const SsaoDraw& dr) {
    const double t0 = qpc_ms();
    D3DVIEWPORT9 vp;
    d->GetViewport(&vp);
    DWORD scissor = 0, atoc = 0;
    RECT sr = {};
    d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
    if (scissor) d->GetScissorRect(&sr);
    d->GetRenderState(D3DRS_ADAPTIVETESS_Y, &atoc);
    const bool atoc_on = atoc == DWORD(MAKEFOURCC('A', 'T', 'O', 'C'));
    const bool mask = action == A_MASK;
    DWORD cw = 0, ab = 0, sep = 0, sba = 0, dba = 0, boa = 0;
    if (mask) {   // the draw's alpha, accumulated 'over' into the mask's alpha
        d->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
        d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        d->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &sep);
        d->GetRenderState(D3DRS_SRCBLENDALPHA, &sba);
        d->GetRenderState(D3DRS_DESTBLENDALPHA, &dba);
        d->GetRenderState(D3DRS_BLENDOPALPHA, &boa);
    }
    gfx_raw_set_render_target(d, 0, mask ? g_ao.mask() : g_ao.null_target());
    d->SetDepthStencilSurface(g_ao.depth());
    gfx_raw_set_viewport(d, &vp);
    if (scissor) d->SetScissorRect(&sr);
    if (atoc_on) d->SetRenderState(D3DRS_ADAPTIVETESS_Y, D3DFMT_UNKNOWN);   // single-sampled: plain alpha test
    if (mask) {
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_ALPHA);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    }
    if (dr.up_vertices && dr.indexed)
        gfx_raw_draw_indexed_up(d, dr.type, dr.min_index, dr.num_vertices, dr.count, dr.up_indices, dr.up_index_format,
                                dr.up_vertices, dr.up_stride);
    else if (dr.up_vertices)
        gfx_raw_draw_up(d, dr.type, dr.count, dr.up_vertices, dr.up_stride);
    else if (dr.indexed)
        gfx_raw_draw_indexed(d, dr.type, dr.base_vertex, dr.min_index, dr.num_vertices, dr.start, dr.count);
    else
        gfx_raw_draw(d, dr.type, dr.start, dr.count);
    if (mask) {
        d->SetRenderState(D3DRS_COLORWRITEENABLE, cw);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, ab);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, sep);
        d->SetRenderState(D3DRS_SRCBLENDALPHA, sba);
        d->SetRenderState(D3DRS_DESTBLENDALPHA, dba);
        d->SetRenderState(D3DRS_BLENDOPALPHA, boa);
    }
    if (atoc_on) d->SetRenderState(D3DRS_ADAPTIVETESS_Y, atoc);
    gfx_raw_set_render_target(d, 0, g_scene_rt);
    d->SetDepthStencilSurface(g_scene_ds);
    gfx_raw_set_viewport(d, &vp);
    if (scissor) d->SetScissorRect(&sr);
    if (mask) g_win.masked++;
    else g_win.depth++;
    g_win.redraw_ms += qpc_ms() - t0;
}

void ssao_present(IDirect3DDevice9* d) {
    if (g_phase == 1) gpu_stamp(d, 1), gpu_stamp(d, 2);   // the scene's target never left: no AO this frame
    if (g_phase != 0) gpu_end_frame(d);
    g_phase = 0;
    drop_frame_refs();
    if (g_gpu_timing < 0) g_gpu_timing = config_int(L"debug", L"gpu_timing", 0, true);
    if (!int(g_ssao_level) && g_gpu_timing <= 0 && !post_on()) return;
    g_win.frames++;
    const double t = qpc_ms();
    if (g_win_start < 0) g_win_start = t;
    if (t - g_win_start >= g_cfg.frame_interval_s * 1000.0) {
        log_window(false);
        add_win(g_total, g_win);
        g_win = Win();
        g_gpu_win = GpuSum();
        g_win_start = t;
    }
}

void ssao_reset() {
    drop_frame_refs();
    g_phase = 0;
    g_rt0 = nullptr;
    g_ao.release();
    g_post.release();
    gpu_release();
    g_failed = false;
    g_post_failed = false;
}

void ssao_apply(int level) {
    level = level < 0 ? 0 : (level > 3 ? 3 : level);
    g_radius_set = config_int(L"graphics", L"ssao_radius", 0, true);
    g_intensity_set = config_int(L"graphics", L"ssao_intensity", 0, true);
    if (level == int(g_ssao_level)) return;
    InterlockedExchange(&g_ssao_level, level);
    static const char* const names[] = {"off", "light (radius 60)", "medium (radius 90)", "strong (radius 120)"};
    RLOG(CH_D3D, L_INFO, "ssao: %s", names[level]);
}

void post_apply(int sharpen, int vibrance) {
    sharpen = sharpen < 0 ? 0 : (sharpen > 3 ? 3 : sharpen);
    vibrance = vibrance < 0 ? 0 : (vibrance > 3 ? 3 : vibrance);
    if (sharpen == int(g_sharpen) && vibrance == int(g_vibrance)) return;
    InterlockedExchange(&g_sharpen, sharpen);
    InterlockedExchange(&g_vibrance, vibrance);
    static const char* const names[] = {"off", "light", "medium", "strong"};
    const post::Params p = post_params();
    RLOG(CH_D3D, L_INFO, "sharpening %s (CAS amount %.2f), vibrance %s (%.2f)", names[sharpen], p.sharpen,
         names[vibrance], p.vibrance);
}

void ssao_summary() {
    add_win(g_total, g_win);
    g_win = Win();
    log_window(true);
}

}  // namespace rol
