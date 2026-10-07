// Frame capture: one line per draw call of one frame, for studying the render passes (where the UI starts and in
// which coordinates it is drawn, which effects draw what). Press the capture key ([debug] capture_key, Num * by
// default) and the next frame goes to <session>\capture_<frame>.log; a test run's shots ask for one too.
//
// Names: textures are named by the archive entry or file they were created from (gfx.cpp), shaders by the effect
// file they were created for (the .fxv cache file whose content D3DXCreateEffect was given, files.cpp; else the
// archive entry of that size read just before).
#include "rolhook.h"

#include <d3d9.h>
#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace rol {
namespace {

SRWLOCK g_names_lock = SRWLOCK_INIT;
std::unordered_map<void*, std::string> g_tex_names, g_shader_names, g_decl_desc;

enum { IDLE, ARMED, ACTIVE } g_state = IDLE;
bool g_key_down = false;
FILE* g_out = nullptr;
int g_draw = 0, g_events = 0;
int g_captures = 0;

std::string lookup(std::unordered_map<void*, std::string>& m, void* p) {
    if (!p) return "-";
    AcquireSRWLockShared(&g_names_lock);
    auto it = m.find(p);
    std::string s = it == m.end() ? std::string() : it->second;
    ReleaseSRWLockShared(&g_names_lock);
    if (s.empty()) {
        char b[24];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "%p", p);
        return b;
    }
    return s;
}

const char* blend_name(DWORD b) {
    static const char* const n[] = {"?", "0", "1", "SC", "ISC", "SA", "ISA", "DA", "IDA", "DC", "IDC", "SAS", "BSA", "BISA", "BF", "IBF"};
    return b < 16 ? n[b] : "?";
}

const char* fmt_short(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_D24S8: return "D24S8";
    default: {
        static char b[16];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "fmt%u", unsigned(f));
        return b;
    }
    }
}

// "PT4 C T2" style summary of a vertex declaration (PT = pretransformed screen position).
std::string decl_desc(IDirect3DVertexDeclaration9* decl) {
    if (!decl) return "-";
    AcquireSRWLockShared(&g_names_lock);
    auto it = g_decl_desc.find(decl);
    std::string cached = it == g_decl_desc.end() ? std::string() : it->second;
    ReleaseSRWLockShared(&g_names_lock);
    if (!cached.empty()) return cached;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    std::string s;
    if (SUCCEEDED(decl->GetDeclaration(el, &n))) {
        static const char* const usage[] = {"P", "BW", "BI", "N", "PS", "T", "TG", "BN", "TF", "PT", "C", "FOG", "D", "S"};
        // components per D3DDECLTYPE: FLOAT1..4, D3DCOLOR, UBYTE4, SHORT2, SHORT4, UBYTE4N, SHORT2N, SHORT4N,
        // USHORT2N, USHORT4N, UDEC3, DEC3N, FLOAT16_2, FLOAT16_4
        static const int comps[] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4, 0};
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
            if (!s.empty()) s += ' ';
            s += el[i].Usage < 14 ? usage[el[i].Usage] : "?";
            if (el[i].Type < 18) s += std::to_string(comps[el[i].Type]);
        }
    }
    AcquireSRWLockExclusive(&g_names_lock);
    g_decl_desc[decl] = s;
    ReleaseSRWLockExclusive(&g_names_lock);
    return s;
}

std::string fvf_desc(DWORD fvf) {
    if (!fvf) return "";
    std::string s;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: s = "P3"; break;
    case D3DFVF_XYZRHW: s = "PT4"; break;
    default: s = "P?"; break;
    }
    if (fvf & D3DFVF_NORMAL) s += " N3";
    if (fvf & D3DFVF_DIFFUSE) s += " C";
    int t = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (t) s += " T" + std::to_string(t);
    return s;
}

}  // namespace

void name_texture(void* tex, const char* name) {
    if (!tex || !name) return;
    AcquireSRWLockExclusive(&g_names_lock);
    g_tex_names[tex] = name;
    ReleaseSRWLockExclusive(&g_names_lock);
}

std::string texture_name(void* tex) {
    if (!tex) return "-";
    AcquireSRWLockShared(&g_names_lock);
    auto it = g_tex_names.find(tex);
    std::string n = it == g_tex_names.end() ? "?" : it->second;
    ReleaseSRWLockShared(&g_names_lock);
    return n;
}

void name_shader(void* sh, const char* name) {
    if (!sh || !name) return;
    AcquireSRWLockExclusive(&g_names_lock);
    g_shader_names[sh] = name;
    ReleaseSRWLockExclusive(&g_names_lock);
}

bool capturing() { return g_state == ACTIVE && g_out; }

bool capture_request() {
    if (g_state != IDLE) return false;
    g_state = ARMED;
    return true;
}

void capture_event(const char* fmt, ...) {
    if (!capturing()) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    fprintf(g_out, "   -- %s\n", msg);
    g_events++;
}

void capture_draw(IDirect3DDevice9* d, const char* kind, UINT prims, const void* up, UINT up_stride) {
    if (!capturing()) return;
    char rt_s[96] = "-";
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(d->GetRenderTarget(0, &rt)) && rt) {
        D3DSURFACE_DESC sd = {};
        rt->GetDesc(&sd);
        _snprintf_s(rt_s, sizeof(rt_s), _TRUNCATE, "%ux%u %s%s@%p", sd.Width, sd.Height, fmt_short(sd.Format),
                    sd.MultiSampleType ? " msaa" : "", (void*)rt);
        rt->Release();
    }
    D3DVIEWPORT9 vp = {};
    d->GetViewport(&vp);
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    IDirect3DVertexDeclaration9* decl = nullptr;
    d->GetVertexDeclaration(&decl);
    std::string layout = fvf ? fvf_desc(fvf) : decl_desc(decl);
    IDirect3DBaseTexture9* t[16] = {};
    for (DWORD i = 0; i < 16; ++i) d->GetTexture(i, &t[i]);
    // stages 2..15 only when something is bound there: "| t2+ 2:name 9:name" (or "-")
    std::string more;
    for (int i = 2; i < 16; ++i)
        if (t[i]) more += " " + std::to_string(i) + ":" + lookup(g_tex_names, t[i]);
    if (more.empty()) more = " -";
    // MIN/MAG/MIP filters of the stages in use: N none, P point, L linear, A anisotropic
    std::string samp;
    for (DWORD i = 0; i < 16; ++i) {
        if (!t[i]) continue;
        DWORD f[3] = {};
        d->GetSamplerState(i, D3DSAMP_MINFILTER, &f[0]);
        d->GetSamplerState(i, D3DSAMP_MAGFILTER, &f[1]);
        d->GetSamplerState(i, D3DSAMP_MIPFILTER, &f[2]);
        samp += " " + std::to_string(i) + ":";
        for (DWORD x : f) samp += x < 4 ? "NPLA"[x] : '?';
    }
    if (samp.empty()) samp = " -";
    DWORD ab = 0, sb = 0, db = 0, at = 0, af = 0, ar = 0, ze = 0, zw = 0, cull = 0;
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    d->GetRenderState(D3DRS_SRCBLEND, &sb);
    d->GetRenderState(D3DRS_DESTBLEND, &db);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    d->GetRenderState(D3DRS_ALPHAFUNC, &af);
    d->GetRenderState(D3DRS_ALPHAREF, &ar);
    d->GetRenderState(D3DRS_ZENABLE, &ze);
    d->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
    d->GetRenderState(D3DRS_CULLMODE, &cull);
    float c[16] = {};
    if (vs) d->GetVertexShaderConstantF(0, c, 4);
    else {
        D3DMATRIX m = {};
        d->GetTransform(D3DTS_PROJECTION, &m);
        memcpy(c, &m, sizeof(c));
    }
    char vert[96] = "";
    if (up) {
        const float* f = static_cast<const float*>(up);
        _snprintf_s(vert, sizeof(vert), _TRUNCATE, " | v0 %.1f %.1f %.3f %.3f (stride %u)", f[0], f[1], f[2], f[3], up_stride);
    }
    // which engine functions issued the draw: return addresses into legends.exe on the stack
    uint32_t callers[4];
    int nc = game_callers(_AddressOfReturnAddress(), callers, 4);
    char by[48] = " | by";
    for (int i = 0; i < nc; ++i) _snprintf_s(by + strlen(by), sizeof(by) - strlen(by), _TRUNCATE, " %06x", callers[i]);
    if (!nc) by[0] = 0;
    fprintf(g_out,
            "%4d %-6s prims %5u | rt %s vp %lu,%lu %lux%lu | vs %s | ps %s | %s | t0 %s | t1 %s | t2+%s | samp%s | blend %lu %s/%s test %lu f%lu>%lu z %lu/%lu cull %lu | %s %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f%s%s\n",
            g_draw++, kind, prims, rt_s, vp.X, vp.Y, vp.Width, vp.Height, lookup(g_shader_names, vs).c_str(),
            lookup(g_shader_names, ps).c_str(), layout.c_str(), lookup(g_tex_names, t[0]).c_str(),
            lookup(g_tex_names, t[1]).c_str(), more.c_str(), samp.c_str(), ab, blend_name(sb), blend_name(db), at, af,
            ar, ze, zw, cull, vs ? "c0-1" : "proj", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], vert, by);
    if (vs) vs->Release();
    if (ps) ps->Release();
    if (decl) decl->Release();
    for (auto* x : t)
        if (x) x->Release();
}

// Called at every Present (before the original): starts a capture on the key press, ends it after one frame.
void capture_on_present() {
    if (g_state == ACTIVE) {
        fprintf(g_out, "end of frame %ld: %d draws, %d events\n", (long)g_frame, g_draw, g_events);
        fclose(g_out);
        g_out = nullptr;
        g_state = IDLE;
        RLOG(CH_D3D, L_INFO, "frame capture #%d done: %d draws", g_captures, g_draw);
    } else if (g_state == ARMED) {
        wchar_t name[64];
        swprintf_s(name, L"capture_%ld.log", (long)g_frame);
        if (_wfopen_s(&g_out, (g_cfg.session_dir + name).c_str(), L"w") == 0 && g_out) {
            g_state = ACTIVE;
            g_draw = g_events = 0;
            ++g_captures;
            fprintf(g_out, "frame %ld capture (draw index, call, primitives | render target | viewport | shaders | "
                           "vertex layout | textures t0, t1, t2+ (stages 2-15 in use) | sampler MIN/MAG/MIP filters of "
                           "those stages (N P L A) | states | vs constants c0-c1 or projection | first vertex | engine "
                           "callers)\n",
                    (long)g_frame);
            fprintf(g_out, "%s\n", camera_describe().c_str());
            fprintf(g_out, "%s\n", shadow_matrix_describe().c_str());
        } else {
            g_state = IDLE;
        }
    }
    if (g_cfg.capture_vk) {
        bool down = (GetAsyncKeyState(g_cfg.capture_vk) & 0x8000) != 0;
        if (down && !g_key_down && g_state == IDLE) {
            g_state = ARMED;
            RLOG(CH_D3D, L_INFO, "frame capture requested (next frame)");
        }
        g_key_down = down;
    }
    if (g_cfg.capture_at > 0 && g_frame == g_cfg.capture_at && g_state == IDLE) g_state = ARMED;
}

}  // namespace rol
