// UI scale (prototype): the whole engine runs on a virtual screen of W/s x H/s pixels; the D3D layer maps every
// screen-space quantity on the backbuffer to the real size. The interface is laid out for the small screen and
// drawn s times bigger; the 3D view keeps rendering at the full resolution (its viewport is scaled, the projection
// depends only on the aspect ratio, which does not change).
//
// Why the whole engine: the interface takes the screen size from the renderer's own fields ([0x014649B8] + 0xA0 /
// + 0xA4, about 90 readers: window layout, game windows, camera, screen object), windows are laid out once when
// they are created, and the camera, the mouse and the screen-space overlays (health bars, selection box) must agree
// with the interface. So the renderer is told the virtual size and everything stays consistent:
//  * legends.exe 1.0 (image base 0x400000, every patched place checked against its bytes): the two places that write
//    the screen size — display setup 0x00E5E190 (device creation) and mode change 0x00E5A230 — store W/s, H/s
//    instead (the D3D present parameters next to them keep the real size);
//  * the game window's mouse messages and ScreenToClient / ClientToScreen speak virtual pixels.
// D3D side, only while the render target is the backbuffer: viewports and scissor rectangles inside the virtual
// screen are scaled when set, Clear and StretchRect rectangles too, and pretransformed (POSITIONT / XYZRHW) draws go
// through uiscale_vs.hlsl (x_real = (x + 0.5) * s - 0.5) with the clip moved to a scissor rectangle. Offscreen
// targets the engine sizes from the screen (post-processing) are simply smaller. Magnified atlas pieces are sampled
// with POINT filtering ([ui] filter=point): bilinear magnification reaches into the atlases' magenta separators.
//
// [ui] scale = percent (100 = off; takes effect at the game's start), filter = point | linear,
// scale_all_pt = 1: scale every pretransformed backbuffer draw without touching the engine (smoke test).
#include "rolhook.h"

#include <d3d9.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "uiscale_vs.h"

namespace rol {
namespace {

// ---------------------------------------------------------------- engine: screen size writers
struct SizeWrite {
    uintptr_t at;
    unsigned char bytes[6];
};
const SizeWrite kWrites[4] = {
    {0x00E5E1AD, {0x89, 0x8B, 0xA0, 0x00, 0x00, 0x00}},   // display setup: mov [ebx + 0xA0], ecx   (width)
    {0x00E5E1B6, {0x89, 0x93, 0xA4, 0x00, 0x00, 0x00}},   //                mov [ebx + 0xA4], edx   (height)
    {0x00E5A355, {0x89, 0x86, 0xA4, 0x00, 0x00, 0x00}},   // mode change:   mov [esi + 0xA4], eax   (height)
    {0x00E5A36E, {0x89, 0xAE, 0xA0, 0x00, 0x00, 0x00}},   //                mov [esi + 0xA0], ebp   (width)
};

int g_pct = 100;           // scale in percent, fixed for the session
bool g_all_pt = false;
bool g_point = true;
bool g_engine_ok = false;
int g_real_w = 0, g_real_h = 0;   // what the engine wanted to store
int g_virt_w = 0, g_virt_h = 0;   // what it got

// Each replaces one 6-byte "mov [reg + ofs], src" with "call stub; nop": the stub stores src * 100 / scale and keeps
// every register (the code around reads none of the flags these change).
__declspec(naked) void w_ebx_ecx() {
    __asm {
        push eax
        push edx
        mov dword ptr [g_real_w], ecx
        mov eax, ecx
        imul eax, eax, 100
        cdq
        idiv dword ptr [g_pct]
        mov dword ptr [ebx + 0xA0], eax
        mov dword ptr [g_virt_w], eax
        pop edx
        pop eax
        ret
    }
}
__declspec(naked) void h_ebx_edx() {
    __asm {
        push eax
        push edx
        mov dword ptr [g_real_h], edx
        mov eax, edx
        imul eax, eax, 100
        cdq
        idiv dword ptr [g_pct]
        mov dword ptr [ebx + 0xA4], eax
        mov dword ptr [g_virt_h], eax
        pop edx
        pop eax
        ret
    }
}
__declspec(naked) void h_esi_eax() {
    __asm {
        push eax
        push edx
        mov dword ptr [g_real_h], eax
        imul eax, eax, 100
        cdq
        idiv dword ptr [g_pct]
        mov dword ptr [esi + 0xA4], eax
        mov dword ptr [g_virt_h], eax
        pop edx
        pop eax
        ret
    }
}
__declspec(naked) void w_esi_ebp() {
    __asm {
        push eax
        push edx
        mov dword ptr [g_real_w], ebp
        mov eax, ebp
        imul eax, eax, 100
        cdq
        idiv dword ptr [g_pct]
        mov dword ptr [esi + 0xA0], eax
        mov dword ptr [g_virt_w], eax
        pop edx
        pop eax
        ret
    }
}

bool patch_writes() {
    void* stubs[4] = {(void*)w_ebx_ecx, (void*)h_ebx_edx, (void*)h_esi_eax, (void*)w_esi_ebp};
    for (const auto& w : kWrites)
        if (memcmp(reinterpret_cast<const void*>(w.at), w.bytes, 6) != 0) return false;
    for (int i = 0; i < 4; ++i) {
        unsigned char call[6] = {0xE8, 0, 0, 0, 0, 0x90};
        *reinterpret_cast<int32_t*>(call + 1) = int32_t(reinterpret_cast<uintptr_t>(stubs[i]) - (kWrites[i].at + 5));
        if (!patch_code(kWrites[i].at, kWrites[i].bytes, call, 6)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- mouse
HWND g_hwnd = nullptr;
WNDPROC g_orig_proc = nullptr;
bool g_unicode = true;
typedef BOOL(WINAPI* PtConv_t)(HWND, LPPOINT);
PtConv_t o_ScreenToClient = nullptr, o_ClientToScreen = nullptr;

LRESULT CALLBACK ui_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_pct != 100 && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);   // client pixels (screen pixels for the wheel)
        l = MAKELPARAM(WORD(int16_t(x * 100 / g_pct)), WORD(int16_t(y * 100 / g_pct)));
    }
    return g_unicode ? CallWindowProcW(g_orig_proc, h, m, w, l) : CallWindowProcA(g_orig_proc, h, m, w, l);
}

BOOL WINAPI h_ScreenToClient(HWND h, LPPOINT p) {
    BOOL r = o_ScreenToClient(h, p);
    if (r && p && g_pct != 100 && h == g_hwnd) {
        p->x = p->x * 100 / g_pct;
        p->y = p->y * 100 / g_pct;
    }
    return r;
}

BOOL WINAPI h_ClientToScreen(HWND h, LPPOINT p) {
    if (p && g_pct != 100 && h == g_hwnd) {
        p->x = p->x * g_pct / 100;
        p->y = p->y * g_pct / 100;
    }
    return o_ClientToScreen(h, p);
}

// ---------------------------------------------------------------- D3D: the backbuffer
UINT g_bb_w = 0, g_bb_h = 0;
IDirect3DSurface9* g_bb = nullptr;   // identity only (no reference held)
volatile LONG g_draws = 0, g_viewports = 0, g_skipped = 0;

float scale() { return g_pct / 100.0f; }

// The backbuffer, or any target of its size: offscreen targets the engine created before the screen size became
// virtual (post-processing) are real-sized too, and the engine draws into them with virtual viewports.
bool screen_sized(IDirect3DSurface9* s) {
    if (!s) return false;
    if (s == g_bb) return true;
    D3DSURFACE_DESC sd = {};
    s->GetDesc(&sd);
    return sd.Width == g_bb_w && sd.Height == g_bb_h;
}

bool rt_is_backbuffer(IDirect3DDevice9* d) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
    bool bb = screen_sized(rt);
    rt->Release();
    return bb;
}

bool inside_virtual(LONG right, LONG bottom) {
    return g_virt_w > 0 && right <= g_virt_w + 1 && bottom <= g_virt_h + 1;
}

RECT scaled(const RECT& r) {
    const float s = scale();
    RECT o = {LONG(r.left * s + 0.5f), LONG(r.top * s + 0.5f), LONG(r.right * s + 0.5f), LONG(r.bottom * s + 0.5f)};
    o.right = std::min<LONG>(o.right, LONG(g_bb_w));
    o.bottom = std::min<LONG>(o.bottom, LONG(g_bb_h));
    return o;
}

// ---------------------------------------------------------------- D3D: pretransformed draws
struct Converted {
    std::vector<D3DVERTEXELEMENT9> src;            // the engine declaration's elements (to detect a recycled pointer)
    IDirect3DVertexDeclaration9* decl = nullptr;   // same elements, POSITIONT -> POSITION; null = not convertible
    IDirect3DVertexShader9* vs = nullptr;
};
std::unordered_map<IDirect3DVertexDeclaration9*, Converted> g_by_decl;
std::unordered_map<DWORD, Converted> g_by_fvf;
IDirect3DVertexShader9* g_vs[2][2][5] = {};
IDirect3DDevice9* g_dev = nullptr;
bool g_warned_layout = false;

void forget_device(IDirect3DDevice9* d) {
    if (g_dev == d) return;   // a new device: shaders and declarations of the old one are gone with it
    memset(g_vs, 0, sizeof(g_vs));
    g_by_decl.clear();
    g_by_fvf.clear();
    g_dev = d;
}

IDirect3DVertexShader9* shader_for(IDirect3DDevice9* d, int col0, int col1, int tex) {
    IDirect3DVertexShader9*& vs = g_vs[col0][col1][tex];
    if (!vs) {
        for (const auto& v : kUiVsVariants)
            if (v.col0 == col0 && v.col1 == col1 && v.tex == tex) d->CreateVertexShader(v.code, &vs);
    }
    return vs;
}

// Builds the POSITION twin of a POSITIONT layout and picks the shader variant; leaves c.decl null when the layout
// has anything but POSITIONT/COLOR0-1/TEXCOORD0-3.
void convert(IDirect3DDevice9* d, Converted& c) {
    int col0 = 0, col1 = 0, tex = 0, pos = 0;
    bool ok = true;
    std::vector<D3DVERTEXELEMENT9> out = c.src;
    for (auto& e : out) {
        if (e.Stream == 0xFF) break;
        switch (e.Usage) {
        case D3DDECLUSAGE_POSITIONT:
            ok &= e.UsageIndex == 0 && e.Type == D3DDECLTYPE_FLOAT4;
            e.Usage = D3DDECLUSAGE_POSITION;
            ++pos;
            break;
        case D3DDECLUSAGE_COLOR:
            if (e.UsageIndex == 0) col0 = 1;
            else if (e.UsageIndex == 1) col1 = 1;
            else ok = false;
            break;
        case D3DDECLUSAGE_TEXCOORD:
            if (e.UsageIndex < 4) tex = std::max(tex, int(e.UsageIndex) + 1);
            else ok = false;
            break;
        default:
            ok = false;
        }
    }
    if (!ok || pos != 1) return;
    c.vs = shader_for(d, col0, col1, tex);
    if (c.vs) d->CreateVertexDeclaration(out.data(), &c.decl);
}

// null when the declaration has no POSITIONT
const Converted* converted_for_decl(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl) {
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = 0;
    if (FAILED(decl->GetDeclaration(el, &n)) || !n) return nullptr;
    bool pt = false;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) pt |= el[i].Usage == D3DDECLUSAGE_POSITIONT;
    if (!pt) return nullptr;
    forget_device(d);
    auto it = g_by_decl.find(decl);
    if (it != g_by_decl.end()) {
        const auto& s = it->second.src;
        if (s.size() == n && !memcmp(s.data(), el, n * sizeof(el[0]))) return &it->second;
        if (it->second.decl) it->second.decl->Release();
        g_by_decl.erase(it);
    }
    Converted& c = g_by_decl[decl];
    c.src.assign(el, el + n);
    convert(d, c);
    if (!c.decl && !g_warned_layout) {
        g_warned_layout = true;
        RLOG(CH_D3D, L_WARN, "ui scale: pretransformed layout not handled (%u elements), drawn unscaled", n - 1);
    }
    return &c;
}

// null when the FVF is not XYZRHW
const Converted* converted_for_fvf(IDirect3DDevice9* d, DWORD fvf) {
    if ((fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return nullptr;
    forget_device(d);
    auto it = g_by_fvf.find(fvf);
    if (it != g_by_fvf.end()) return &it->second;
    Converted& c = g_by_fvf[fvf];
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        c.src.push_back({0, off, type, D3DDECLMETHOD_DEFAULT, usage, index});
        off = WORD(off + size);
    };
    add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
    bool ok = !(fvf & (D3DFVF_NORMAL | D3DFVF_PSIZE | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_LASTBETA_D3DCOLOR));
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    int ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (int i = 0; i < ntex; ++i) {
        int code = (fvf >> (16 + 2 * i)) & 3;   // D3DFVF_TEXTUREFORMAT1..4 per set
        int dims = code == D3DFVF_TEXTUREFORMAT1 ? 1 : code == D3DFVF_TEXTUREFORMAT3 ? 3
                 : code == D3DFVF_TEXTUREFORMAT4 ? 4 : 2;
        add(BYTE(D3DDECLTYPE_FLOAT1 + dims - 1), D3DDECLUSAGE_TEXCOORD, BYTE(i), WORD(4 * dims));
    }
    c.src.push_back(D3DDECL_END());
    if (ok) convert(d, c);
    return &c;
}

struct Saved {
    bool active;
    IDirect3DVertexShader9* vs;
    IDirect3DVertexDeclaration9* decl;
    DWORD fvf;
    D3DVIEWPORT9 vp;
    float c250[4];
    DWORD scissor_on;
    RECT scissor;
    bool filter;
    DWORD mag, min;
} g_saved;

}  // namespace

void uiscale_install(HMODULE exe) {
    g_pct = config_int(L"ui", L"scale", 100, true);
    if (g_pct < 100 || g_pct > 400) g_pct = 100;
    g_all_pt = config_int(L"ui", L"scale_all_pt", 0, true) != 0;
    g_point = _wcsicmp(config_str(L"ui", L"filter", L"point").c_str(), L"linear") != 0;
    if (g_pct == 100 && !g_all_pt) return;   // nothing asked for: no engine patches at all

    if (g_all_pt) return;   // smoke test: D3D side only
    g_engine_ok = reinterpret_cast<uintptr_t>(exe) == 0x00400000 && patch_writes();
    if (!g_engine_ok) {
        g_pct = 100;
        RLOG(CH_CORE, L_WARN, "UI scale off: unknown legends.exe build");
        return;
    }
    bool m = patch_iat(exe, "USER32.dll", "ScreenToClient", (void*)h_ScreenToClient, (void**)&o_ScreenToClient) &&
             patch_iat(exe, "USER32.dll", "ClientToScreen", (void*)h_ClientToScreen, (void**)&o_ClientToScreen);
    RLOG(CH_CORE, L_INFO, "UI scale %d%%: the engine gets a virtual screen (size writers patched), mouse conversions %s, "
         "%s filtering", g_pct, m ? "hooked" : "NOT hooked", g_point ? "point" : "linear");
}

// The game window, once the device is created: its mouse messages go through ui_wndproc.
void uiscale_window(HWND w) {
    if (!g_engine_ok || !w || g_hwnd) return;
    g_hwnd = w;
    g_unicode = IsWindowUnicode(w) != FALSE;
    g_orig_proc = reinterpret_cast<WNDPROC>(g_unicode ? SetWindowLongPtrW(w, GWLP_WNDPROC, LONG_PTR(ui_wndproc))
                                                      : SetWindowLongPtrA(w, GWLP_WNDPROC, LONG_PTR(ui_wndproc)));
    RLOG(CH_UI, L_INFO, "ui scale: game window %p subclassed (%s)", (void*)w, g_unicode ? "unicode" : "ansi");
}

// After CreateDevice / Reset: the real backbuffer.
void uiscale_device(IDirect3DDevice9* d, UINT w, UINT h) {
    g_bb_w = w;
    g_bb_h = h;
    g_bb = nullptr;
    IDirect3DSurface9* bb = nullptr;
    if (d && SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_bb = bb;
        bb->Release();
    }
    if (g_all_pt && !g_virt_w) {   // smoke test: the virtual screen is simply the backbuffer / scale
        g_virt_w = int(w) * 100 / g_pct;
        g_virt_h = int(h) * 100 / g_pct;
    }
    if (g_engine_ok)
        RLOG(CH_UI, L_INFO, "ui scale %d%%: backbuffer %ux%u, engine screen %dx%d (it wanted %dx%d)", g_pct, w, h, g_virt_w,
             g_virt_h, g_real_w, g_real_h);
}

int uiscale_percent() { return g_engine_ok ? g_pct : 100; }

// SetViewport on the backbuffer: a viewport inside the virtual screen is in virtual pixels.
void uiscale_viewport(IDirect3DDevice9* d, D3DVIEWPORT9& vp) {
    if (g_pct == 100 || !(g_engine_ok || g_all_pt)) return;
    if (!inside_virtual(LONG(vp.X + vp.Width), LONG(vp.Y + vp.Height)) || !rt_is_backbuffer(d)) return;
    const float s = scale();
    DWORD x = DWORD(vp.X * s + 0.5f), y = DWORD(vp.Y * s + 0.5f);
    vp.Width = std::min<DWORD>(DWORD((vp.X + vp.Width) * s + 0.5f), g_bb_w) - x;
    vp.Height = std::min<DWORD>(DWORD((vp.Y + vp.Height) * s + 0.5f), g_bb_h) - y;
    vp.X = x;
    vp.Y = y;
    InterlockedIncrement(&g_viewports);
}

// SetScissorRect on the backbuffer
void uiscale_scissor(IDirect3DDevice9* d, RECT& r) {
    if (g_pct == 100 || !(g_engine_ok || g_all_pt)) return;
    if (!inside_virtual(r.right, r.bottom) || !rt_is_backbuffer(d)) return;
    r = scaled(r);
}

// Clear rectangles on the backbuffer (n of them, copied into out)
bool uiscale_clear_rects(IDirect3DDevice9* d, DWORD n, const D3DRECT* in, D3DRECT* out) {
    if (g_pct == 100 || !(g_engine_ok || g_all_pt) || !n || !in || n > 64 || !rt_is_backbuffer(d)) return false;
    for (DWORD i = 0; i < n; ++i) {
        RECT r = {in[i].x1, in[i].y1, in[i].x2, in[i].y2};
        if (inside_virtual(r.right, r.bottom)) r = scaled(r);
        out[i] = {r.left, r.top, r.right, r.bottom};
    }
    return true;
}

// StretchRect: a rectangle on the backbuffer side is in virtual pixels
void uiscale_stretch(IDirect3DSurface9* src, const RECT*& sr, RECT& sbuf, IDirect3DSurface9* dst, const RECT*& dr,
                     RECT& dbuf) {
    if (g_pct == 100 || !g_engine_ok) return;
    if (sr && inside_virtual(sr->right, sr->bottom) && screen_sized(src)) {
        sbuf = scaled(*sr);
        sr = &sbuf;
    }
    if (dr && inside_virtual(dr->right, dr->bottom) && screen_sized(dst)) {
        dbuf = scaled(*dr);
        dr = &dbuf;
    }
}

// True when the draw about to be issued is pretransformed geometry on the backbuffer: the state is switched to the
// scaling shader and uiscale_end() restores it.
bool uiscale_begin(IDirect3DDevice9* d) {
    Saved& sv = g_saved;
    sv.active = false;
    sv.filter = false;
    if (g_pct == 100 || !(g_engine_ok || g_all_pt)) return false;
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    IDirect3DVertexDeclaration9* decl = nullptr;
    const Converted* c = nullptr;
    if (fvf) {
        c = converted_for_fvf(d, fvf);
    } else if (SUCCEEDED(d->GetVertexDeclaration(&decl)) && decl) {
        c = converted_for_decl(d, decl);
    }
    if (!c || !c->decl || !rt_is_backbuffer(d)) {
        if (decl) decl->Release();
        if (c && !c->decl) InterlockedIncrement(&g_skipped);
        return false;
    }
    sv.decl = decl;   // keeps the reference until uiscale_end
    sv.fvf = fvf;
    sv.vs = nullptr;
    d->GetVertexShader(&sv.vs);   // pretransformed draws normally have none; an effect's is put back after
    d->GetViewport(&sv.vp);
    d->GetVertexShaderConstantF(250, sv.c250, 1);
    d->GetRenderState(D3DRS_SCISSORTESTENABLE, &sv.scissor_on);
    d->GetScissorRect(&sv.scissor);

    const float s = scale(), W = float(g_bb_w), H = float(g_bb_h);
    const float xf[4] = {2 * s / W, -2 * s / H, s / W - 1, 1 - s / H};
    d->SetVertexShaderConstantF(250, xf, 1);
    d->SetVertexDeclaration(c->decl);
    d->SetVertexShader(c->vs);
    D3DVIEWPORT9 vp = {0, 0, g_bb_w, g_bb_h, sv.vp.MinZ, sv.vp.MaxZ};
    d->SetViewport(&vp);
    // pretransformed geometry was confined to the viewport (and the scissor rectangle): both already real-sized
    RECT clip = {LONG(sv.vp.X), LONG(sv.vp.Y), LONG(sv.vp.X + sv.vp.Width), LONG(sv.vp.Y + sv.vp.Height)};
    if (sv.scissor_on) {
        clip.left = std::max(clip.left, sv.scissor.left);
        clip.top = std::max(clip.top, sv.scissor.top);
        clip.right = std::min(clip.right, sv.scissor.right);
        clip.bottom = std::min(clip.bottom, sv.scissor.bottom);
    }
    d->SetScissorRect(&clip);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    if (g_point) {   // bilinear magnification would pull in the atlases' magenta separators
        d->GetSamplerState(0, D3DSAMP_MAGFILTER, &sv.mag);
        d->GetSamplerState(0, D3DSAMP_MINFILTER, &sv.min);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        sv.filter = true;
    }
    sv.active = true;
    InterlockedIncrement(&g_draws);
    return true;
}

void uiscale_end(IDirect3DDevice9* d) {
    Saved& sv = g_saved;
    if (!sv.active) return;
    d->SetVertexShader(sv.vs);
    if (sv.vs) sv.vs->Release();
    if (sv.fvf) d->SetFVF(sv.fvf);
    else d->SetVertexDeclaration(sv.decl);
    if (sv.decl) sv.decl->Release();
    sv.decl = nullptr;
    sv.vs = nullptr;
    d->SetVertexShaderConstantF(250, sv.c250, 1);
    d->SetViewport(&sv.vp);
    d->SetScissorRect(&sv.scissor);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, sv.scissor_on);
    if (sv.filter) {
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, sv.mag);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, sv.min);
        sv.filter = false;
    }
    sv.active = false;
}

void uiscale_summary() {
    if (!g_engine_ok && !g_all_pt) return;
    RLOG(CH_D3D, L_INFO, "ui scale: %ld pretransformed draws scaled, %ld viewports scaled, %ld pretransformed draws "
         "skipped (layout not handled)", (long)g_draws, (long)g_viewports, (long)g_skipped);
}

}  // namespace rol
