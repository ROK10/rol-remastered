// Module loading, Direct3D 9 device and D3DX (through the game's d3d8xstub.dll) tracing.
#include "rolhook.h"
#include "gfxraw.h"

#include <d3d9.h>
#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
int rol_vt_d3d9_CreateDevice(void);
int rol_vt_dev_Reset(void);
int rol_vt_dev_Present(void);
int rol_vt_dev_CreateTexture(void);
int rol_vt_dev_BeginScene(void);
int rol_vt_dev_SetRenderTarget(void);
int rol_vt_dev_SetTexture(void);
int rol_vt_dev_SetSamplerState(void);
int rol_vt_dev_DrawPrimitive(void);
int rol_vt_dev_DrawIndexedPrimitive(void);
int rol_vt_dev_DrawPrimitiveUP(void);
int rol_vt_dev_DrawIndexedPrimitiveUP(void);
int rol_vt_dev_CreateVertexShader(void);
int rol_vt_dev_CreatePixelShader(void);
int rol_vt_dev_SetPixelShader(void);
int rol_vt_dev_Clear(void);
int rol_vt_dev_SetViewport(void);
int rol_vt_dev_SetScissorRect(void);
int rol_vt_dev_StretchRect(void);
int rol_vt_dev_SetVertexShader(void);
int rol_vt_tex_LockRect(void);
}

namespace rol {
namespace {

// ---------------------------------------------------------------- originals
typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR);
typedef HMODULE(WINAPI* LoadLibraryA_t)(LPCSTR);
typedef FARPROC(WINAPI* GetProcAddress_t)(HMODULE, LPCSTR);
LoadLibraryW_t o_LoadLibraryW;
LoadLibraryA_t o_LoadLibraryA;
GetProcAddress_t o_GetProcAddress;

typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef void*(WINAPI* GetStub_t)();
Direct3DCreate9_t o_Direct3DCreate9;
GetStub_t o_GetStub;

typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* BeginScene_t)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* SetRenderTarget_t)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* DrawIndexedPrimitiveUP_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
typedef HRESULT(STDMETHODCALLTYPE* LockRect_t)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* CreateVertexShader_t)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
typedef HRESULT(STDMETHODCALLTYPE* CreatePixelShader_t)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
typedef HRESULT(STDMETHODCALLTYPE* Clear_t)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
CreateVertexShader_t o_CreateVertexShader;
CreatePixelShader_t o_CreatePixelShader;
typedef HRESULT(STDMETHODCALLTYPE* SetPixelShader_t)(IDirect3DDevice9*, IDirect3DPixelShader9*);
SetPixelShader_t o_SetPixelShader;
typedef HRESULT(STDMETHODCALLTYPE* SetVertexShader_t)(IDirect3DDevice9*, IDirect3DVertexShader9*);
SetVertexShader_t o_SetVertexShader;
Clear_t o_Clear;
typedef HRESULT(STDMETHODCALLTYPE* SetViewport_t)(IDirect3DDevice9*, const D3DVIEWPORT9*);
typedef HRESULT(STDMETHODCALLTYPE* SetScissorRect_t)(IDirect3DDevice9*, const RECT*);
typedef HRESULT(STDMETHODCALLTYPE* StretchRect_t)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
SetViewport_t o_SetViewport;
SetScissorRect_t o_SetScissorRect;
StretchRect_t o_StretchRect;
CreateDevice_t o_CreateDevice;
Reset_t o_Reset;
Present_t o_Present;
CreateTexture_t o_CreateTexture;
BeginScene_t o_BeginScene;
SetRenderTarget_t o_SetRenderTarget;
SetTexture_t o_SetTexture;
SetSamplerState_t o_SetSamplerState;
DrawPrimitive_t o_DrawPrimitive;
DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
DrawPrimitiveUP_t o_DrawPrimitiveUP;
DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
LockRect_t o_LockRect;

// D3DX entry points reached through the d3d8xstub vtable: plain __stdcall, no 'this'.
typedef HRESULT(WINAPI* TexFromFileW_t)(IDirect3DDevice9*, LPCWSTR, IDirect3DTexture9**);
typedef HRESULT(WINAPI* TexFromMem_t)(IDirect3DDevice9*, LPCVOID, UINT, IDirect3DTexture9**);
typedef HRESULT(WINAPI* TexFromMemEx_t)(IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);
typedef HRESULT(WINAPI* TexFromFileExW_t)(IDirect3DDevice9*, LPCWSTR, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, DWORD, DWORD, D3DCOLOR, void*, PALETTEENTRY*, IDirect3DTexture9**);
typedef HRESULT(WINAPI* CreateEffect_t)(IDirect3DDevice9*, LPCVOID, UINT, const void*, void*, DWORD, void*, void**, IUnknown**);
typedef HRESULT(WINAPI* CreateEffectFromFileW_t)(IDirect3DDevice9*, LPCWSTR, const void*, void*, DWORD, void*, void**, IUnknown**);
typedef HRESULT(WINAPI* EffectCompilerFromFileW_t)(LPCWSTR, const void*, void*, DWORD, void**, IUnknown**);
TexFromFileW_t o_TexFromFileW;
TexFromMem_t o_TexFromMem;
TexFromMemEx_t o_TexFromMemEx;
TexFromFileExW_t o_TexFromFileExW;
CreateEffect_t o_CreateEffect;
CreateEffectFromFileW_t o_CreateEffectFromFileW;
EffectCompilerFromFileW_t o_EffectCompilerFromFileW;

// ---------------------------------------------------------------- state
struct FrameWin {
    uint32_t frames = 0;
    double sum = 0, mn = 1e30, mx = 0;
    uint64_t draws = 0, prims = 0, settex = 0, scenes = 0, rts = 0;
    uint32_t tex_created = 0;
};
FrameWin g_fw;
double g_last_present = -1, g_win_start = 0, g_next_vram = 0;
uint64_t g_total_frames = 0;
uint32_t g_hitches = 0;

struct TexStats {
    uint32_t count = 0, failed = 0;
    uint64_t src_bytes = 0, gpu_bytes = 0;
    double ms = 0;
    std::map<std::string, uint32_t> by_kind;
};
TexStats g_ts;
CRITICAL_SECTION g_tcs;
CRITICAL_SECTION g_vcs;
std::unordered_set<void*> g_vtables;       // every vtable we have patched
void* g_dev_vtable = nullptr;
void* g_tex_vtable = nullptr;
void* g_d3d_vtable = nullptr;
volatile LONG g_lock_fail = 0, g_lock_fixed = 0, g_effects = 0;
uint32_t g_sampler[4][16] = {};            // MAG/MIN/MIP filter + MAXANISOTROPY value histogram
thread_local const char* t_effect = nullptr;   // effect being created: names the shaders it creates
bool g_rt_msaa = false;                    // render target 0 is multisampled (set from the present parameters)
bool g_atoc_on = false;                    // NVIDIA alpha-to-coverage currently switched on by rolhook
const DWORD kAtoc = MAKEFOURCC('A', 'T', 'O', 'C');
// Shadow maps: the engine renders depth into square R32F render-target textures (2048 by default) and samples them
// with POINT filtering, so every shadow texel shows as a block. [graphics] shadow_filter=linear samples them with
// LINEAR filtering: the stored depth is interpolated before the shader's comparison, and the shadow edge follows a
// smooth contour instead of a staircase (still a hard edge; a soft penumbra needs PCF in the shaders).
std::unordered_set<void*> g_shadow_tex;
bool g_shadow_stage[16] = {};
int g_shadow_size = 0;
// With our shadow map fit ([graphics] shadow_fit) a lookup can fall outside the map (a surface outside the fitted
// volume): BORDER addressing with the clear value 1.0 (nothing in front) makes it read "lit". The engine's own
// address modes on the stage are kept and put back when another texture takes the stage.
bool g_border_on[16] = {};
DWORD g_eng_addr[16][3];   // ADDRESSU, ADDRESSV, BORDERCOLOR as the engine last set them
bool g_eng_addr_init = false;

void eng_addr_defaults() {
    for (auto& s : g_eng_addr) s[0] = D3DTADDRESS_WRAP, s[1] = D3DTADDRESS_WRAP, s[2] = 0;
    for (bool& b : g_border_on) b = false;
    g_eng_addr_init = true;
}

// ---------------------------------------------------------------- helpers
const char* fmt_name(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_DXT1: return "DXT1";
    case D3DFMT_DXT2: return "DXT2";
    case D3DFMT_DXT3: return "DXT3";
    case D3DFMT_DXT4: return "DXT4";
    case D3DFMT_DXT5: return "DXT5";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R8G8B8: return "R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_A1R5G5B5: return "A1R5G5B5";
    case D3DFMT_A4R4G4B4: return "A4R4G4B4";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8: return "A8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_UNKNOWN: return "UNKNOWN";
    default: {
        static thread_local char b[24];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "fmt%d", int(f));
        return b;
    }
    }
}

const char* pool_name(D3DPOOL p) {
    switch (p) {
    case D3DPOOL_DEFAULT: return "DEFAULT";
    case D3DPOOL_MANAGED: return "MANAGED";
    case D3DPOOL_SYSTEMMEM: return "SYSTEMMEM";
    case D3DPOOL_SCRATCH: return "SCRATCH";
    default: return "?";
    }
}

uint64_t tex_bytes(D3DFORMAT f, UINT w, UINT h, DWORD levels) {
    double bpp;
    switch (f) {
    case D3DFMT_DXT1: bpp = 0.5; break;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: case D3DFMT_L8: case D3DFMT_A8: bpp = 1; break;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_D16: bpp = 2; break;
    case D3DFMT_A16B16G16R16F: bpp = 8; break;
    default: bpp = 4; break;
    }
    double total = 0;
    for (DWORD i = 0; i < std::max<DWORD>(levels, 1); ++i) {
        UINT lw = std::max(1u, w >> i), lh = std::max(1u, h >> i);
        if (bpp < 2 && f != D3DFMT_L8 && f != D3DFMT_A8) { lw = std::max(4u, lw); lh = std::max(4u, lh); }
        total += lw * double(lh) * bpp;
    }
    return uint64_t(total);
}

struct DdsInfo {
    bool ok = false;
    uint32_t w = 0, h = 0, mips = 0;
    char fmt[16] = "-";
};

DdsInfo dds_info(const void* p, UINT size) {
    DdsInfo d;
    if (!p || size < 128) return d;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    if (memcmp(b, "DDS ", 4) != 0) {
        strcpy_s(d.fmt, "not-DDS");
        return d;
    }
    d.ok = true;
    memcpy(&d.h, b + 12, 4);
    memcpy(&d.w, b + 16, 4);
    memcpy(&d.mips, b + 28, 4);
    uint32_t pf, bpp;
    memcpy(&pf, b + 80, 4);
    memcpy(&bpp, b + 88, 4);
    if (pf & 4) {
        memcpy(d.fmt, b + 84, 4);
        d.fmt[4] = 0;
    } else {
        _snprintf_s(d.fmt, sizeof(d.fmt), _TRUNCATE, "RGB%u%s", bpp, (pf & 1) ? "A" : "");
    }
    return d;
}

// Which .big entry these bytes came from: the last entry read by this thread with the same unpacked size.
void attribute(char* out, size_t cap, UINT size) {
    const EntryRef* le = last_entry();
    if (le && le->usize == size && now_ms() - le->t < 10000.0)
        _snprintf_s(out, cap, _TRUNCATE, "%s :: %s", le->archive, le->name);
    else
        _snprintf_s(out, cap, _TRUNCATE, "(unattributed, %u bytes)", size);
}

void* follow_jmps(void* p) {
    for (int i = 0; i < 4 && p; ++i) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        if (b[0] == 0xE9) {
            int32_t rel;
            memcpy(&rel, b + 1, 4);
            p = const_cast<uint8_t*>(b) + 5 + rel;
        } else if (b[0] == 0xFF && b[1] == 0x25) {
            void** slot;
            memcpy(&slot, b + 2, 4);
            p = *slot;
        } else {
            break;
        }
    }
    return p;
}

std::string d3dx_buffer_text(IUnknown* buf) {
    if (!buf) return {};
    void** vt = *reinterpret_cast<void***>(buf);
    auto get_ptr = reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>(vt[3]);
    auto get_size = reinterpret_cast<DWORD(STDMETHODCALLTYPE*)(IUnknown*)>(vt[4]);
    const char* p = static_cast<const char*>(get_ptr(buf));
    DWORD n = get_size(buf);
    std::string s(p ? p : "", p ? strnlen(p, n) : 0);
    for (auto& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
}

void log_texture(const char* api, const void* src, UINT size, const wchar_t* file, bool has_req, UINT rw, UINT rh,
                 UINT rmips, DWORD usage, D3DFORMAT rfmt, D3DPOOL rpool, DWORD filter, DWORD mipfilter, HRESULT hr,
                 IDirect3DTexture9* tex, double ms, const EntryRef* entry = nullptr, const void* srcinfo = nullptr) {
    char who[320];
    if (file) _snprintf_s(who, sizeof(who), _TRUNCATE, "file %s", rel_path(file).c_str());
    else if (entry) _snprintf_s(who, sizeof(who), _TRUNCATE, "%s :: %s", entry->archive, entry->name);
    else attribute(who, sizeof(who), size);
    DdsInfo d = dds_info(src, size);
    D3DSURFACE_DESC desc = {};
    DWORD levels = 0;
    uint64_t gpu = 0;
    if (SUCCEEDED(hr) && tex) {
        tex->GetLevelDesc(0, &desc);
        levels = tex->GetLevelCount();
        gpu = tex_bytes(desc.Format, desc.Width, desc.Height, levels);
    }
    EnterCriticalSection(&g_tcs);
    g_ts.count++;
    if (FAILED(hr)) g_ts.failed++;
    g_ts.src_bytes += size;
    g_ts.gpu_bytes += gpu;
    g_ts.ms += ms;
    if (SUCCEEDED(hr)) {
        char key[64];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "%-8s %4ux%-4u", fmt_name(desc.Format), desc.Width, desc.Height);
        g_ts.by_kind[key]++;
    }
    LeaveCriticalSection(&g_tcs);

    if (FAILED(hr)) {
        RLOG(CH_TEXTURES, L_ERROR, "%-8s FAILED hr=0x%08lx  %s | src %s %ux%u", api, (unsigned long)hr, who, d.fmt, d.w, d.h);
        return;
    }
    RLOG(CH_TEXTURES, L_INFO, "%-8s %s | src %s %ux%u mips %u | gpu %s %ux%u L%lu %s %.0f KB | %.2f ms", api, who,
         d.fmt, d.w, d.h, d.mips, fmt_name(desc.Format), desc.Width, desc.Height, levels, pool_name(desc.Pool),
         gpu / 1024.0, ms);
    if (has_req)
        RLOG(CH_TEXTURES, L_DEBUG, "         request w %d h %d mips %d usage 0x%lx fmt %s pool %s filter 0x%lx mipfilter 0x%lx srcinfo %s",
             int(rw), int(rh), int(rmips), usage, fmt_name(rfmt), pool_name(rpool), filter, mipfilter,
             srcinfo ? "requested" : "-");
    RLOG(CH_TEXTURES, L_TRACE, "         from %s", stack_desc(2, 12).c_str());
}

void log_effect(const char* api, const char* who, UINT len, DWORD flags, HRESULT hr, IUnknown** errs, double ms) {
    InterlockedIncrement(&g_effects);
    std::string text = (errs && *errs) ? d3dx_buffer_text(*errs) : std::string();
    if (FAILED(hr)) {
        RLOG(CH_D3D, L_ERROR, "%s FAILED hr=0x%08lx %s | %.1500s", api, (unsigned long)hr, who, text.c_str());
        return;
    }
    RLOG(CH_D3D, L_INFO, "%s %s (%u bytes, flags 0x%lx) %.1f ms", api, who, len, flags, ms);
    if (!text.empty()) RLOG(CH_D3D, L_DEBUG, "  compiler output: %.1500s", text.c_str());
}

void log_pp(const char* what, const D3DPRESENT_PARAMETERS* pp, DWORD behavior) {
    if (!pp) return;
    RLOG(CH_D3D, L_INFO,
         "%s: %ux%u %s, %u backbuffer(s), %s, refresh %u Hz, interval 0x%lx (%s), MSAA %d q%lu, swap %d, "
         "depth %s auto %d, behavior 0x%lx, flags 0x%lx",
         what, pp->BackBufferWidth, pp->BackBufferHeight, fmt_name(pp->BackBufferFormat), pp->BackBufferCount,
         pp->Windowed ? "windowed" : "fullscreen", pp->FullScreen_RefreshRateInHz, pp->PresentationInterval,
         pp->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? "vsync OFF" : "vsync ON", int(pp->MultiSampleType),
         pp->MultiSampleQuality, int(pp->SwapEffect), fmt_name(pp->AutoDepthStencilFormat), pp->EnableAutoDepthStencil,
         behavior, pp->Flags);
}

// Patch a set of vtable slots once; refuse a second, different implementation (the originals are global).
bool claim_vtable(void* vt, void*& owner, const char* what) {
    EnterCriticalSection(&g_vcs);
    bool fresh = g_vtables.insert(vt).second;
    bool allowed = fresh && (owner == nullptr);
    if (allowed) owner = vt;
    LeaveCriticalSection(&g_vcs);
    if (fresh && !allowed) RLOG(CH_D3D, L_WARN, "second %s implementation (vtable %p) left unhooked", what, vt);
    return allowed;
}

// ---------------------------------------------------------------- texture / device hooks
HRESULT STDMETHODCALLTYPE h_LockRect(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* r, const RECT* rc, DWORD flags) {
    HRESULT hr = o_LockRect(t, level, r, rc, flags);
    if (SUCCEEDED(hr)) return hr;
    D3DSURFACE_DESC d = {};
    t->GetLevelDesc(level, &d);
    InterlockedIncrement(&g_lock_fail);
    RLOG(CH_D3D, L_WARN, "LockRect FAILED hr=0x%08lx level %u flags 0x%lx | %ux%u %s pool %s usage 0x%lx | caller %s",
         (unsigned long)hr, level, flags, d.Width, d.Height, fmt_name(d.Format), pool_name(d.Pool), d.Usage,
         addr_desc(_ReturnAddress()).c_str());
    if (g_cfg.fix_lockrect && (flags & D3DLOCK_DISCARD) && !(d.Usage & D3DUSAGE_DYNAMIC)) {
        hr = o_LockRect(t, level, r, rc, flags & ~DWORD(D3DLOCK_DISCARD));
        RLOG(CH_D3D, L_WARN, "  retried without D3DLOCK_DISCARD -> 0x%08lx", (unsigned long)hr);
        if (SUCCEEDED(hr)) InterlockedIncrement(&g_lock_fixed);
    }
    return hr;
}

void hook_texture_vtable(IDirect3DTexture9* t) {
    void** vt = *reinterpret_cast<void***>(t);
    if (!claim_vtable(vt, g_tex_vtable, "texture")) return;
    patch_slot(&vt[rol_vt_tex_LockRect()], (void*)h_LockRect, (void**)&o_LockRect);
    RLOG(CH_D3D, L_DEBUG, "texture vtable %p hooked (LockRect)", vt);
}

void on_frame(IDirect3DDevice9* d) {
    double t = now_ms();
    if (g_last_present >= 0) {
        double dt = t - g_last_present;
        g_fw.frames++;
        g_fw.sum += dt;
        g_fw.mn = std::min(g_fw.mn, dt);
        g_fw.mx = std::max(g_fw.mx, dt);
        if (dt > 100.0) {
            g_hitches++;
            RLOG(CH_FRAME, L_DEBUG, "hitch: frame took %.1f ms", dt);
        }
    } else {
        g_win_start = t;
    }
    g_last_present = t;
    InterlockedIncrement(&g_frame);
    g_total_frames++;
    double win = t - g_win_start;
    if (win >= g_cfg.frame_interval_s * 1000.0) {
        if (g_fw.frames) {
            double f = g_fw.frames;
            RLOG(CH_FRAME, L_INFO,
                 "fps %6.1f | frame ms avg %6.2f min %6.2f max %7.2f | per frame: draws %5.0f prims %8.0f "
                 "setTexture %5.0f scenes %.1f rtSwitch %.1f | textures created %u",
                 f * 1000.0 / win, g_fw.sum / f, g_fw.mn, g_fw.mx, g_fw.draws / f, g_fw.prims / f, g_fw.settex / f,
                 g_fw.scenes / f, g_fw.rts / f, g_fw.tex_created);
        }
        g_fw = FrameWin();
        g_win_start = t;
    }
    if (t >= g_next_vram) {
        g_next_vram = t + g_cfg.memory_interval_s * 1000.0;
        RLOG(CH_MEMORY, L_INFO, "D3D GetAvailableTextureMem: %u MB", d->GetAvailableTextureMem() >> 20);
    }
}

HRESULT STDMETHODCALLTYPE h_Present(IDirect3DDevice9* d, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    ssao_present(d);
    on_frame(d);
    shots_on_present(d);   // first: a shot that waits for a frame capture is taken before the capture is closed
    capture_on_present();
    probe_on_present();
    motion_on_present();
    calls_on_present();
    camera_trace();
    lod_tick();
    borderless_present();
    return o_Present(d, a, b, w, r);
}

// Alpha-to-coverage (NVIDIA's D3D9 switch: D3DRS_ADAPTIVETESS_Y = 'ATOC'): alpha-tested cut-outs (leaves,
// railings, ornaments) get MSAA-smoothed edges instead of the alpha test's hard stair-steps. Only on multisampled
// targets; atoc=1 limits it to alpha-tested draws without blending, atoc=2 takes the blended ones too.
// Only a real cut-out threshold qualifies (ALPHAREF >= 64; the trees use 132): the terrain's base pass alpha-tests
// "alpha > 0" on tiles whose alpha is 3/255, which coverage would turn into ~1% of the samples (terrain gone).
void atoc_for_draw(IDirect3DDevice9* d) {
    bool want = false;
    if (g_cfg.atoc > 0 && g_rt_msaa) {
        DWORD at = 0, ab = 0, ref = 0;
        d->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
        if (at) d->GetRenderState(D3DRS_ALPHAREF, &ref);
        if (at && ref >= 64) {
            d->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
            want = g_cfg.atoc >= 2 || !ab;
        }
    }
    if (want != g_atoc_on) {
        d->SetRenderState(D3DRS_ADAPTIVETESS_Y, want ? kAtoc : DWORD(D3DFMT_UNKNOWN));
        g_atoc_on = want;
    }
}

// Borderless fullscreen hands D3D a windowed copy of the parameters; what the runtime filled in (backbuffer size,
// count, format) goes back to the engine's copy, its Windowed / refresh rate stay as the engine set them.
void keep_outputs(D3DPRESENT_PARAMETERS* pp, const D3DPRESENT_PARAMETERS& used) {
    pp->BackBufferWidth = used.BackBufferWidth;
    pp->BackBufferHeight = used.BackBufferHeight;
    pp->BackBufferFormat = used.BackBufferFormat;
    pp->BackBufferCount = used.BackBufferCount;
}

HRESULT STDMETHODCALLTYPE h_Reset(IDirect3DDevice9* d, D3DPRESENT_PARAMETERS* pp) {
    log_pp("Reset", pp, 0);
    g_atoc_on = false;   // Reset restores default render states and the backbuffer as render target
    eng_addr_defaults();   // ... and default sampler states
    g_rt_msaa = pp && pp->MultiSampleType != D3DMULTISAMPLE_NONE;
    ssao_reset();   // its targets, state block and queries
    D3DPRESENT_PARAMETERS bl;
    bool borderless = borderless_params(pp, bl);
    HRESULT hr = o_Reset(d, borderless ? &bl : pp);
    if (borderless) keep_outputs(pp, bl);
    RLOG(CH_D3D, FAILED(hr) ? L_ERROR : L_INFO, "Reset -> 0x%08lx%s", (unsigned long)hr, borderless ? " (borderless)" : "");
    if (SUCCEEDED(hr) && pp) {
        uiscale_device(d, pp->BackBufferWidth, pp->BackBufferHeight);
        if (borderless) borderless_window(bl.hDeviceWindow, pp->BackBufferWidth, pp->BackBufferHeight);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE h_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT f,
                                          D3DPOOL pool, IDirect3DTexture9** out, HANDLE* sh) {
    HRESULT hr = o_CreateTexture(d, w, h, lv, usage, f, pool, out, sh);
    g_fw.tex_created++;
    if (FAILED(hr)) {
        RLOG(CH_D3D, L_ERROR, "CreateTexture %ux%u L%u %s %s usage 0x%lx FAILED 0x%08lx | caller %s", w, h, lv,
             fmt_name(f), pool_name(pool), usage, (unsigned long)hr, addr_desc(_ReturnAddress()).c_str());
        return hr;
    }
    RLOG(CH_D3D, L_DEBUG, "CreateTexture %ux%u L%u %s %s usage 0x%lx | caller %s", w, h, lv, fmt_name(f),
         pool_name(pool), usage, addr_desc(_ReturnAddress()).c_str());
    if (out && *out) {
        hook_texture_vtable(*out);
        // engine-made textures get a descriptive name for frame captures (D3DX loads rename theirs afterwards)
        char nm[64];
        _snprintf_s(nm, sizeof(nm), _TRUNCATE, "%s %ux%u %s%s", (usage & D3DUSAGE_RENDERTARGET) ? "rt" : "tex", w, h,
                    fmt_name(f), (usage & D3DUSAGE_DYNAMIC) ? " dyn" : "");
        name_texture(*out, nm);
        if ((usage & D3DUSAGE_RENDERTARGET) && f == D3DFMT_R32F && w == h && w >= 512) {
            g_shadow_tex.insert(*out);
            g_shadow_size = int(w);
            name_texture(*out, ("shadow map " + std::to_string(w)).c_str());
            RLOG(CH_D3D, L_INFO, "shadow map render target %ux%u R32F (%s filtering)", w, h,
                 g_cfg.shadow_linear ? "LINEAR, [graphics] shadow_filter" : "the engine's POINT");
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE h_BeginScene(IDirect3DDevice9* d) {
    g_fw.scenes++;
    capture_event("BeginScene");
    return o_BeginScene(d);
}

HRESULT STDMETHODCALLTYPE h_SetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    g_fw.rts++;
    ssao_set_rt(d, i, s);   // leaving the scene's target: its AO first
    if (i == 0 && s) {
        D3DSURFACE_DESC sd = {};
        s->GetDesc(&sd);
        g_rt_msaa = sd.MultiSampleType != D3DMULTISAMPLE_NONE;
        capture_event("SetRenderTarget 0 -> %ux%u%s @%p", sd.Width, sd.Height, g_rt_msaa ? " msaa" : "", (void*)s);
    } else {
        capture_event("SetRenderTarget %lu -> %p", i, (void*)s);
    }
    return o_SetRenderTarget(d, i, s);
}

HRESULT STDMETHODCALLTYPE h_Clear(IDirect3DDevice9* d, DWORD n, const D3DRECT* r, DWORD flags, D3DCOLOR c, float z, DWORD s) {
    capture_event("Clear flags 0x%lx color 0x%08lx z %.2f rects %lu", flags, c, z, n);
    D3DRECT scaled[64];
    if (uiscale_clear_rects(d, n, r, scaled)) r = scaled;
    return o_Clear(d, n, r, flags, c, z, s);
}

// Viewports, scissor rectangles and StretchRect rectangles on the backbuffer follow the interface scale (uiscale.cpp).
HRESULT STDMETHODCALLTYPE h_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp) {
    if (!vp) return o_SetViewport(d, vp);
    D3DVIEWPORT9 v = *vp;
    uiscale_viewport(d, v);
    return o_SetViewport(d, &v);
}

HRESULT STDMETHODCALLTYPE h_SetScissorRect(IDirect3DDevice9* d, const RECT* r) {
    if (!r) return o_SetScissorRect(d, r);
    RECT v = *r;
    uiscale_scissor(d, v);
    return o_SetScissorRect(d, &v);
}

HRESULT STDMETHODCALLTYPE h_StretchRect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr, IDirect3DSurface9* dst,
                                        const RECT* dr, D3DTEXTUREFILTERTYPE f) {
    RECT sb, db;
    uiscale_stretch(src, sr, sb, dst, dr, db);
    return o_StretchRect(d, src, sr, dst, dr, f);
}

// [debug] dump_shaders=1: every shader the effects create, disassembled (D3DXDisassembleShader of d3dx9_27.dll, with
// the constant table: the names of the parameters in their registers) to <session>\shaders\<effect>.<vs|ps>.<n>.txt,
// each distinct bytecode once - what the effects compute, for working out what can be improved in them.
int g_dump_shaders = -1;   // -1: not read yet
std::unordered_map<uint64_t, int> g_dumped;   // bytecode hash -> its file's number
long g_dump_count = 0;
typedef HRESULT(WINAPI* Disasm_t)(const DWORD*, BOOL, LPCSTR, IUnknown**);
Disasm_t g_disasm = nullptr;

void dump_shader(const DWORD* fn, bool pixel, const char* effect) {
    if (g_dump_shaders < 0) g_dump_shaders = config_int(L"debug", L"dump_shaders", 0, true);
    if (!g_dump_shaders || !fn) return;
    size_t n = 1;   // tokens up to and with the end token
    while (n < 65536 && fn[n - 1] != 0x0000FFFF) ++n;
    if (n >= 65536) return;
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(fn);
    for (size_t i = 0; i < n * 4; ++i) h = (h ^ b[i]) * 1099511628211ull;
    if (g_dumped.count(h)) return;
    const int num = int(g_dumped.size());
    g_dumped[h] = num;
    if (!g_disasm) {
        HMODULE dx = GetModuleHandleW(L"d3dx9_27.dll");
        if (dx) g_disasm = reinterpret_cast<Disasm_t>(::GetProcAddress(dx, "D3DXDisassembleShader"));
        if (!g_disasm) return;
    }
    IUnknown* buf = nullptr;
    if (FAILED(g_disasm(fn, FALSE, nullptr, &buf)) || !buf) return;
    void** vt = *reinterpret_cast<void***>(buf);
    auto get_ptr = reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>(vt[3]);
    auto get_size = reinterpret_cast<DWORD(STDMETHODCALLTYPE*)(IUnknown*)>(vt[4]);
    std::string name = effect ? effect : "no-effect";
    for (char& c : name)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') c = '+';
    const std::wstring dir = g_cfg.session_dir + L"shaders\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t file[400];
    _snwprintf_s(file, _countof(file), _TRUNCATE, L"%s%S.%s.%03d.txt", dir.c_str(), name.c_str(), pixel ? L"ps" : L"vs",
                 num);
    FILE* f = nullptr;
    if (_wfopen_s(&f, file, L"wb") == 0 && f) {
        fprintf(f, "// %s shader of effect %s, %u bytes\n", pixel ? "pixel" : "vertex", effect ? effect : "-",
                unsigned(n * 4));
        fwrite(get_ptr(buf), 1, strnlen(static_cast<const char*>(get_ptr(buf)), get_size(buf)), f);
        fclose(f);
        ++g_dump_count;
    }
    reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>(vt[2])(buf);
}

HRESULT STDMETHODCALLTYPE h_CreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** out) {
    HRESULT hr = o_CreateVertexShader(d, fn, out);
    if (SUCCEEDED(hr) && out && t_effect) name_shader(*out, t_effect);
    if (SUCCEEDED(hr)) dump_shader(fn, false, t_effect);
    if (SUCCEEDED(hr) && out && *out) ssao_shader_created(*out, t_effect);
    return hr;
}

HRESULT STDMETHODCALLTYPE h_CreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** out) {
    HRESULT hr = o_CreatePixelShader(d, fn, out);
    if (SUCCEEDED(hr) && out && t_effect) name_shader(*out, t_effect);
    if (SUCCEEDED(hr)) dump_shader(fn, true, t_effect);
    if (SUCCEEDED(hr) && out && *out) shaderfx_created(fn, *out, t_effect);   // its shadow test / lighting, if any
    if (SUCCEEDED(hr) && out && *out) ssao_shader_created(*out, t_effect);
    return hr;
}

// The shader set may be a copy rolhook made of it (soft shadows, gamma-correct light: shaderfx.cpp).
HRESULT STDMETHODCALLTYPE h_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
    ssao_set_ps(ps);
    return o_SetPixelShader(d, shaderfx_swap(d, ps));
}

HRESULT STDMETHODCALLTYPE h_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs) {
    ssao_set_vs(vs);
    return o_SetVertexShader(d, vs);
}

HRESULT STDMETHODCALLTYPE h_SetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    g_fw.settex++;
    HRESULT hr = o_SetTexture(d, stage, t);
    if (stage < 16 && !g_shadow_tex.empty()) {
        if (!g_eng_addr_init) eng_addr_defaults();
        g_shadow_stage[stage] = t && g_shadow_tex.count(t) != 0;
        if (g_shadow_stage[stage] && g_cfg.shadow_linear) {
            o_SetSamplerState(d, stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            o_SetSamplerState(d, stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        }
        const bool border = g_shadow_stage[stage] && g_cfg.shadow_fit > 0;
        if (border && !g_border_on[stage]) {
            o_SetSamplerState(d, stage, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
            o_SetSamplerState(d, stage, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
            o_SetSamplerState(d, stage, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
            g_border_on[stage] = true;
        } else if (!border && g_border_on[stage]) {
            o_SetSamplerState(d, stage, D3DSAMP_ADDRESSU, g_eng_addr[stage][0]);
            o_SetSamplerState(d, stage, D3DSAMP_ADDRESSV, g_eng_addr[stage][1]);
            o_SetSamplerState(d, stage, D3DSAMP_BORDERCOLOR, g_eng_addr[stage][2]);
            g_border_on[stage] = false;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE h_SetSamplerState(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE type, DWORD v) {
    int row = type == D3DSAMP_MAGFILTER ? 0 : type == D3DSAMP_MINFILTER ? 1 : type == D3DSAMP_MIPFILTER ? 2
            : type == D3DSAMP_MAXANISOTROPY ? 3 : -1;
    if (row >= 0) g_sampler[row][std::min<DWORD>(v, 15)]++;
    // the shadow map's stage under our fit: remember the engine's address mode, keep BORDER while the map is there
    if (s < 16 && (type == D3DSAMP_ADDRESSU || type == D3DSAMP_ADDRESSV || type == D3DSAMP_BORDERCOLOR)) {
        if (!g_eng_addr_init) eng_addr_defaults();
        g_eng_addr[s][type == D3DSAMP_ADDRESSU ? 0 : type == D3DSAMP_ADDRESSV ? 1 : 2] = v;
        if (g_border_on[s]) return D3D_OK;
    }
    // the shadow map's stage: LINEAR instead of POINT, and never anisotropic (it is a depth lookup, not a picture)
    if (s < 16 && g_shadow_stage[s] && (type == D3DSAMP_MINFILTER || type == D3DSAMP_MAGFILTER)) {
        if (g_cfg.shadow_linear && v == D3DTEXF_POINT) v = D3DTEXF_LINEAR;
        return o_SetSamplerState(d, s, type, v);
    }
    // The engine only ever uses POINT/LINEAR. Upgrading LINEAR minification to anisotropic keeps textures sharp
    // at the RTS camera's oblique angles; POINT samplers (UI, lookups) are left alone.
    if (g_cfg.anisotropy > 1 && type == D3DSAMP_MINFILTER && v == D3DTEXF_LINEAR) {
        o_SetSamplerState(d, s, D3DSAMP_MAXANISOTROPY, DWORD(g_cfg.anisotropy));
        v = D3DTEXF_ANISOTROPIC;
    }
    return o_SetSamplerState(d, s, type, v);
}

inline void before_draw(IDirect3DDevice9* d, const char* kind, UINT n, const void* up, UINT stride) {
    g_fw.draws++;
    g_fw.prims += n;
    if (g_cfg.atoc > 0 || g_atoc_on) atoc_for_draw(d);
    if (capturing()) capture_draw(d, kind, n, up, stride);
    if (g_shots_watch) shots_before_draw(d);
}

// Each draw: statistics / ATOC / capture, then the interface scale wraps pretransformed UI draws (uiscale.cpp).
// The scene's AO (ssao.cpp): a draw that writes the scene's depth is drawn again into the AO's depth after the game's,
// a particle or an overlay into its mask; the interface's first pretransformed draw may composite it first.
HRESULT STDMETHODCALLTYPE h_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) {
    const int redraw = ssao_before_draw(d);
    before_draw(d, "DP", n, nullptr, 0);
    bool sc = uiscale_begin(d);
    HRESULT hr = o_DrawPrimitive(d, t, s, n);
    if (sc) uiscale_end(d);
    if (redraw && SUCCEEDED(hr)) ssao_redraw(d, redraw, {false, t, 0, 0, 0, s, n, nullptr, 0, nullptr, D3DFMT_UNKNOWN});
    return hr;
}

HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mi, UINT nv, UINT si, UINT n) {
    const int redraw = ssao_before_draw(d);
    before_draw(d, "DIP", n, nullptr, 0);
    if (g_motion_on) motion_draw(d, bv, mi, nv);
    bool sc = uiscale_begin(d);
    HRESULT hr = o_DrawIndexedPrimitive(d, t, bv, mi, nv, si, n);
    if (sc) uiscale_end(d);
    if (redraw && SUCCEEDED(hr)) ssao_redraw(d, redraw, {true, t, bv, mi, nv, si, n, nullptr, 0, nullptr, D3DFMT_UNKNOWN});
    return hr;
}

HRESULT STDMETHODCALLTYPE h_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT st) {
    const int redraw = ssao_before_draw(d);
    before_draw(d, "DPUP", n, v, st);
    bool sc = uiscale_begin(d);
    HRESULT hr = o_DrawPrimitiveUP(d, t, n, v, st);
    if (sc) uiscale_end(d);
    if (redraw && SUCCEEDED(hr)) ssao_redraw(d, redraw, {false, t, 0, 0, 0, 0, n, v, st, nullptr, D3DFMT_UNKNOWN});
    return hr;
}

HRESULT STDMETHODCALLTYPE h_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n,
                                                   const void* idx, D3DFORMAT f, const void* v, UINT st) {
    const int redraw = ssao_before_draw(d);
    before_draw(d, "DIPUP", n, v, st);
    bool sc = uiscale_begin(d);
    HRESULT hr = o_DrawIndexedPrimitiveUP(d, t, mi, nv, n, idx, f, v, st);
    if (sc) uiscale_end(d);
    if (redraw && SUCCEEDED(hr)) ssao_redraw(d, redraw, {true, t, 0, mi, nv, 0, n, v, st, idx, f});
    return hr;
}

void hook_device(IDirect3DDevice9* dev) {
    void** vt = *reinterpret_cast<void***>(dev);
    if (!claim_vtable(vt, g_dev_vtable, "device")) return;
    struct S { int idx; void* hook; void** orig; const char* name; } slots[] = {
        {rol_vt_dev_Reset(), (void*)h_Reset, (void**)&o_Reset, "Reset"},
        {rol_vt_dev_Present(), (void*)h_Present, (void**)&o_Present, "Present"},
        {rol_vt_dev_CreateTexture(), (void*)h_CreateTexture, (void**)&o_CreateTexture, "CreateTexture"},
        {rol_vt_dev_BeginScene(), (void*)h_BeginScene, (void**)&o_BeginScene, "BeginScene"},
        {rol_vt_dev_SetRenderTarget(), (void*)h_SetRenderTarget, (void**)&o_SetRenderTarget, "SetRenderTarget"},
        {rol_vt_dev_SetTexture(), (void*)h_SetTexture, (void**)&o_SetTexture, "SetTexture"},
        {rol_vt_dev_SetSamplerState(), (void*)h_SetSamplerState, (void**)&o_SetSamplerState, "SetSamplerState"},
        {rol_vt_dev_DrawPrimitive(), (void*)h_DrawPrimitive, (void**)&o_DrawPrimitive, "DrawPrimitive"},
        {rol_vt_dev_DrawIndexedPrimitive(), (void*)h_DrawIndexedPrimitive, (void**)&o_DrawIndexedPrimitive, "DrawIndexedPrimitive"},
        {rol_vt_dev_DrawPrimitiveUP(), (void*)h_DrawPrimitiveUP, (void**)&o_DrawPrimitiveUP, "DrawPrimitiveUP"},
        {rol_vt_dev_DrawIndexedPrimitiveUP(), (void*)h_DrawIndexedPrimitiveUP, (void**)&o_DrawIndexedPrimitiveUP, "DrawIndexedPrimitiveUP"},
        {rol_vt_dev_CreateVertexShader(), (void*)h_CreateVertexShader, (void**)&o_CreateVertexShader, "CreateVertexShader"},
        {rol_vt_dev_CreatePixelShader(), (void*)h_CreatePixelShader, (void**)&o_CreatePixelShader, "CreatePixelShader"},
        {rol_vt_dev_SetPixelShader(), (void*)h_SetPixelShader, (void**)&o_SetPixelShader, "SetPixelShader"},
        {rol_vt_dev_SetVertexShader(), (void*)h_SetVertexShader, (void**)&o_SetVertexShader, "SetVertexShader"},
        {rol_vt_dev_Clear(), (void*)h_Clear, (void**)&o_Clear, "Clear"},
        {rol_vt_dev_SetViewport(), (void*)h_SetViewport, (void**)&o_SetViewport, "SetViewport"},
        {rol_vt_dev_SetScissorRect(), (void*)h_SetScissorRect, (void**)&o_SetScissorRect, "SetScissorRect"},
        {rol_vt_dev_StretchRect(), (void*)h_StretchRect, (void**)&o_StretchRect, "StretchRect"},
    };
    int ok = 0;
    for (auto& s : slots) {
        if (patch_slot(&vt[s.idx], s.hook, s.orig)) ++ok;
        else RLOG(CH_D3D, L_WARN, "cannot hook device::%s", s.name);
    }
    RLOG(CH_D3D, L_INFO, "device vtable %p (%s): %d methods hooked", vt, addr_desc(vt[0]).c_str(), ok);
}

HRESULT STDMETHODCALLTYPE h_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND w, DWORD flags,
                                         D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    log_pp("CreateDevice", pp, flags);
    g_rt_msaa = pp && pp->MultiSampleType != D3DMULTISAMPLE_NONE;   // render target 0 starts as the backbuffer
    D3DPRESENT_PARAMETERS bl;
    bool borderless = borderless_params(pp, bl);
    HRESULT hr = o_CreateDevice(d3d, adapter, type, w, flags, borderless ? &bl : pp, out);
    if (borderless) keep_outputs(pp, bl);
    if (FAILED(hr) || !out || !*out) {
        RLOG(CH_D3D, L_ERROR, "CreateDevice FAILED 0x%08lx", (unsigned long)hr);
        return hr;
    }
    IDirect3DDevice9* dev = *out;
    D3DCAPS9 c = {};
    dev->GetDeviceCaps(&c);
    RLOG(CH_D3D, L_INFO,
         "caps: max texture %lux%lu, max anisotropy %lu, VS %lu.%lu PS %lu.%lu, %lu texture stages, %lu MRT, "
         "textureCaps 0x%lx",
         c.MaxTextureWidth, c.MaxTextureHeight, c.MaxAnisotropy, D3DSHADER_VERSION_MAJOR(c.VertexShaderVersion),
         D3DSHADER_VERSION_MINOR(c.VertexShaderVersion), D3DSHADER_VERSION_MAJOR(c.PixelShaderVersion),
         D3DSHADER_VERSION_MINOR(c.PixelShaderVersion), c.MaxSimultaneousTextures, c.NumSimultaneousRTs, c.TextureCaps);
    RLOG(CH_D3D, L_INFO, "device created; D3D available texture memory estimate %u MB", dev->GetAvailableTextureMem() >> 20);
    uiscale_device(dev, pp->BackBufferWidth, pp->BackBufferHeight);   // filled in by the runtime when 0 (windowed)
    uiscale_window(pp->hDeviceWindow ? pp->hDeviceWindow : w);
    if (borderless) borderless_window(pp->hDeviceWindow ? pp->hDeviceWindow : w, pp->BackBufferWidth, pp->BackBufferHeight);
    hook_device(dev);
    motion_device(dev);
    return hr;
}

IDirect3D9* WINAPI h_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d = o_Direct3DCreate9(sdk);
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)o_Direct3DCreate9, &m);
    wchar_t path[MAX_PATH] = {};
    if (m) GetModuleFileNameW(m, path, MAX_PATH);
    RLOG(CH_D3D, L_INFO, "Direct3DCreate9(sdk %u) -> %p via %s", sdk, (void*)d, rel_path(path).c_str());
    if (!d) return d;
    D3DADAPTER_IDENTIFIER9 id = {};
    if (SUCCEEDED(d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id)))
        RLOG(CH_D3D, L_INFO, "adapter: %s | driver %s %u.%u.%u.%u | vendor 0x%04lx device 0x%04lx", id.Description,
             id.Driver, HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart),
             HIWORD(id.DriverVersion.LowPart), LOWORD(id.DriverVersion.LowPart), id.VendorId, id.DeviceId);
    D3DDISPLAYMODE dm = {};
    if (SUCCEEDED(d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &dm)))
        RLOG(CH_D3D, L_INFO, "desktop mode %ux%u @ %u Hz %s", dm.Width, dm.Height, dm.RefreshRate, fmt_name(dm.Format));
    void** vt = *reinterpret_cast<void***>(d);
    if (claim_vtable(vt, g_d3d_vtable, "IDirect3D9"))
        patch_slot(&vt[rol_vt_d3d9_CreateDevice()], (void*)h_CreateDevice, (void**)&o_CreateDevice);
    return d;
}

// ---------------------------------------------------------------- HD texture override
volatile LONG g_ovr_hits = 0, g_ovr_fail = 0;
uint64_t g_ovr_bytes = 0;

// Logical sizes. The interface draws its pictures by texel rectangles of the original textures: UIRenderMgr's sprite
// (0x00F72BB0) turns them into UVs with the texture's size, asked through 0x00F6F580 / 0x00F6F5A0 (their only calls,
// at 0x00F73261 / 0x00F7327E; eax = the texture manager's record, +4 the renderer's texture object, its methods
// +0x4C / +0x50 width and height). An HD replacement reports the original size there, so the same rectangles cover
// the same pictures, now with more texels; everything else sees the real size. Which IDirect3DTexture9 a renderer
// object holds is found once at run time (the first pointer in the object whose vtable is D3D9's texture vtable).
struct Logical {
    uint32_t hd_w, hd_h, w, h;
};
SRWLOCK g_logical_lock = SRWLOCK_INIT;
std::unordered_map<void*, Logical> g_logical;   // IDirect3DTexture9* -> its HD and original sizes
int g_tex_ptr_off = -1;                          // where a renderer texture object keeps its IDirect3DTexture9*
volatile LONG g_logical_served = 0;
float g_hd_factor[2] = {1.0f, 1.0f};             // HD texels per logical texel of the sprite being drawn

constexpr uintptr_t AT_SPRITE_TEX_W = 0x00F73261, AT_SPRITE_TEX_H = 0x00F7327E;
constexpr uintptr_t FN_TEX_W = 0x00F6F580, FN_TEX_H = 0x00F6F5A0;
uintptr_t g_fn_tex_w = FN_TEX_W, g_fn_tex_h = FN_TEX_H;

void set_logical(IDirect3DTexture9* t, const DdsInfo& hd, const DdsInfo& orig) {
    if (!t) return;
    AcquireSRWLockExclusive(&g_logical_lock);
    if (hd.ok && orig.ok && (hd.w != orig.w || hd.h != orig.h) && orig.w && orig.h)
        g_logical[t] = {hd.w, hd.h, orig.w, orig.h};
    else
        g_logical.erase(t);   // a texture made at the address of a released HD one
    ReleaseSRWLockExclusive(&g_logical_lock);
}

bool read_ptr(uintptr_t a, uintptr_t* v) {
    __try {
        *v = *reinterpret_cast<const uintptr_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t texture_of(uintptr_t obj) {
    uintptr_t v, vt;
    if (g_tex_ptr_off >= 0) return read_ptr(obj + g_tex_ptr_off, &v) ? v : 0;
    if (!g_tex_vtable) return 0;
    for (int off = 0; off < 0x100; off += 4) {
        if (read_ptr(obj + off, &v) && v > 0x10000 && read_ptr(v, &vt) && vt == uintptr_t(g_tex_vtable)) {
            g_tex_ptr_off = off;
            RLOG(CH_TEXTURES, L_INFO, "logical sizes: the renderer's texture object keeps its IDirect3DTexture9 at +0x%x",
                 off);
            return v;
        }
    }
    return 0;
}

// axis 0 width, 1 height; actual = what the renderer object answered
int __stdcall logical_dim(uintptr_t record, int axis, int actual) {
    g_hd_factor[axis] = 1.0f;
    if (axis == 0) spritelog_texture(record);
    if (g_logical.empty() || actual <= 0) return actual;
    uintptr_t obj;
    if (!read_ptr(record + 4, &obj) || !obj) return actual;
    const uintptr_t tex = texture_of(obj);
    if (!tex) return actual;
    AcquireSRWLockShared(&g_logical_lock);
    auto it = g_logical.find(reinterpret_cast<void*>(tex));
    int out = actual;
    if (it != g_logical.end() && int(axis ? it->second.hd_h : it->second.hd_w) == actual) {
        out = int(axis ? it->second.h : it->second.w);
        g_hd_factor[axis] = float(actual) / float(out);
    }
    ReleaseSRWLockShared(&g_logical_lock);
    if (out != actual) InterlockedIncrement(&g_logical_served);
    return out;
}

__declspec(naked) void h_sprite_tex_w() {
    __asm {
        push eax                       // the record
        call dword ptr [g_fn_tex_w]    // its renderer object's width (eax = the record)
        push eax
        push 0
        push dword ptr [esp + 8]
        call logical_dim
        add esp, 4
        ret
    }
}

__declspec(naked) void h_sprite_tex_h() {
    __asm {
        push eax
        call dword ptr [g_fn_tex_h]
        push eax
        push 1
        push dword ptr [esp + 8]
        call logical_dim
        add esp, 4
        ret
    }
}

void install_logical_sizes() {
    struct Site { uintptr_t at, fn; void* hook; const char* what; } sites[] = {
        {AT_SPRITE_TEX_W, FN_TEX_W, (void*)h_sprite_tex_w, "width"},
        {AT_SPRITE_TEX_H, FN_TEX_H, (void*)h_sprite_tex_h, "height"},
    };
    int ok = 0;
    for (auto& s : sites) {
        unsigned char expect[5] = {0xE8, 0, 0, 0, 0}, call[5] = {0xE8, 0, 0, 0, 0};
        *reinterpret_cast<int32_t*>(expect + 1) = int32_t(s.fn - (s.at + 5));
        *reinterpret_cast<int32_t*>(call + 1) = int32_t(reinterpret_cast<uintptr_t>(s.hook) - (s.at + 5));
        if (patch_code(s.at, expect, call, 5)) ++ok;
    }
    RLOG(CH_TEXTURES, ok == 2 ? L_INFO : L_WARN, "logical sizes: %s (interface sprites see an HD replacement's original "
         "size)", ok == 2 ? "installed" : "NOT installed (code differs)");
}

bool read_dds(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(f, &sz);
    bool ok = sz.QuadPart > 128 && sz.QuadPart < (512ll << 20);
    if (ok) {
        out.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size() &&
             memcmp(out.data(), "DDS ", 4) == 0;
    }
    CloseHandle(f);
    if (!ok) RLOG(CH_TEXTURES, L_ERROR, "HD file unreadable or not a DDS: %s", rel_path(path.c_str()).c_str());
    return ok;
}

// "art\buildings\vinc\textures\x.tga" -> "<override_dir>art\buildings\vinc\textures\x.dds"
std::wstring override_path(const char* entry) {
    int n = MultiByteToWideChar(CP_UTF8, 0, entry, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, entry, -1, &w[0], n);
    size_t dot = w.find_last_of(L'.'), slash = w.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) w.resize(dot);
    return g_cfg.tex_override_dir + w + L".dds";
}

// The archive entry that produced 'size' bytes, if it was read by this thread just before the D3DX call.
const EntryRef* entry_for(UINT size) {
    const EntryRef* le = last_entry();
    return (le && le->usize == size && now_ms() - le->t < 10000.0) ? le : nullptr;
}

// Names a texture created from an archive entry; terrain tiles also tell a test run that the match is coming.
void named(IDirect3DTexture9* t, const EntryRef* entry, bool hd) {
    if (!t || !entry) return;
    name_texture(t, hd ? (std::string(entry->name) + " [HD]").c_str() : entry->name);
    if (g_shots_watch && !_strnicmp(entry->name, "art\\terrain\\", 12)) shots_terrain_texture(t);
}

// Loads <override_dir>\<entry>.dds into 'hd' when the HD pack has a replacement for this entry.
bool find_override(const EntryRef* entry, std::vector<uint8_t>& hd, std::wstring& path) {
    if (!entry || !g_cfg.tex_override || g_cfg.tex_override_dir.empty()) return false;
    path = override_path(entry->name);
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    return read_dds(path, hd);
}

// ---------------------------------------------------------------- D3DX hooks
HRESULT WINAPI h_TexFromMemEx(IDirect3DDevice9* dev, LPCVOID src, UINT size, UINT w, UINT h, UINT mips, DWORD usage,
                              D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR ck, void* info,
                              PALETTEENTRY* pal, IDirect3DTexture9** out) {
    double t0 = now_ms();
    const EntryRef* entry = entry_for(size);
    std::vector<uint8_t> hd;
    std::wstring hd_path;
    if (find_override(entry, hd, hd_path)) {
        // Keep the mip chain stored in the HD file (the engine's texture_manifest.xml caps it at 1-5 levels)
        // and its pixel format (no re-encoding at load time).
        DdsInfo d = dds_info(hd.data(), UINT(hd.size()));
        UINT levels = d.mips > 1 ? d.mips : mips;
        HRESULT hr = o_TexFromMemEx(dev, hd.data(), UINT(hd.size()), w, h, levels, usage, D3DFMT_UNKNOWN, pool,
                                    filter, mipfilter, ck, info, pal, out);
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_ovr_hits);
            g_ovr_bytes += hd.size();
            if (out) named(*out, entry, true);
            if (out) set_logical(*out, d, dds_info(src, size));
            log_texture("HD", hd.data(), UINT(hd.size()), nullptr, true, w, h, levels, usage, D3DFMT_UNKNOWN, pool,
                        filter, mipfilter, hr, out ? *out : nullptr, now_ms() - t0, entry, info);
            RLOG(CH_TEXTURES, L_DEBUG, "         replaced from %s (engine asked for %d mip levels, %s)",
                 rel_path(hd_path.c_str()).c_str(), int(mips), fmt_name(fmt));
            return hr;
        }
        InterlockedIncrement(&g_ovr_fail);
        RLOG(CH_TEXTURES, L_ERROR, "HD override FAILED hr=0x%08lx for %s - loading the original",
             (unsigned long)hr, rel_path(hd_path.c_str()).c_str());
    }
    HRESULT hr = o_TexFromMemEx(dev, src, size, w, h, mips, usage, fmt, pool, filter, mipfilter, ck, info, pal, out);
    if (SUCCEEDED(hr) && out) named(*out, entry, false);
    if (SUCCEEDED(hr) && out) set_logical(*out, DdsInfo(), DdsInfo());
    log_texture("memEx", src, size, nullptr, true, w, h, mips, usage, fmt, pool, filter, mipfilter, hr,
                out ? *out : nullptr, now_ms() - t0, entry, info);
    return hr;
}

HRESULT WINAPI h_TexFromMem(IDirect3DDevice9* dev, LPCVOID src, UINT size, IDirect3DTexture9** out) {
    double t0 = now_ms();
    const EntryRef* entry = entry_for(size);
    std::vector<uint8_t> hd;
    std::wstring hd_path;
    if (find_override(entry, hd, hd_path)) {
        HRESULT hr = o_TexFromMem(dev, hd.data(), UINT(hd.size()), out);
        if (SUCCEEDED(hr)) {
            InterlockedIncrement(&g_ovr_hits);
            g_ovr_bytes += hd.size();
            if (out) named(*out, entry, true);
            if (out) set_logical(*out, dds_info(hd.data(), UINT(hd.size())), dds_info(src, size));
            log_texture("HD", hd.data(), UINT(hd.size()), nullptr, false, 0, 0, 0, 0, D3DFMT_UNKNOWN,
                        D3DPOOL_MANAGED, 0, 0, hr, out ? *out : nullptr, now_ms() - t0, entry);
            return hr;
        }
        InterlockedIncrement(&g_ovr_fail);
        RLOG(CH_TEXTURES, L_ERROR, "HD override FAILED hr=0x%08lx for %s - loading the original",
             (unsigned long)hr, rel_path(hd_path.c_str()).c_str());
    }
    HRESULT hr = o_TexFromMem(dev, src, size, out);
    if (SUCCEEDED(hr) && out) named(*out, entry, false);
    if (SUCCEEDED(hr) && out) set_logical(*out, DdsInfo(), DdsInfo());
    log_texture("mem", src, size, nullptr, false, 0, 0, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 0, 0, hr,
                out ? *out : nullptr, now_ms() - t0, entry);
    return hr;
}

HRESULT WINAPI h_TexFromFileW(IDirect3DDevice9* dev, LPCWSTR file, IDirect3DTexture9** out) {
    double t0 = now_ms();
    HRESULT hr = o_TexFromFileW(dev, file, out);
    if (SUCCEEDED(hr) && out && file) name_texture(*out, rel_path(file).c_str());
    if (SUCCEEDED(hr) && out) set_logical(*out, DdsInfo(), DdsInfo());
    log_texture("file", nullptr, 0, file ? file : L"?", false, 0, 0, 0, 0, D3DFMT_UNKNOWN, D3DPOOL_MANAGED, 0, 0,
                hr, out ? *out : nullptr, now_ms() - t0);
    return hr;
}

HRESULT WINAPI h_TexFromFileExW(IDirect3DDevice9* dev, LPCWSTR file, UINT w, UINT h, UINT mips, DWORD usage,
                                D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipfilter, D3DCOLOR ck, void* info,
                                PALETTEENTRY* pal, IDirect3DTexture9** out) {
    double t0 = now_ms();
    HRESULT hr = o_TexFromFileExW(dev, file, w, h, mips, usage, fmt, pool, filter, mipfilter, ck, info, pal, out);
    if (SUCCEEDED(hr) && out && file) name_texture(*out, rel_path(file).c_str());
    if (SUCCEEDED(hr) && out) set_logical(*out, DdsInfo(), DdsInfo());
    log_texture("fileEx", nullptr, 0, file ? file : L"?", true, w, h, mips, usage, fmt, pool, filter, mipfilter, hr,
                out ? *out : nullptr, now_ms() - t0);
    return hr;
}

HRESULT WINAPI h_CreateEffect(IDirect3DDevice9* dev, LPCVOID src, UINT len, const void* defs, void* inc, DWORD flags,
                              void* pool, void** eff, IUnknown** errs) {
    double t0 = now_ms();
    char who[320];
    // by content: a loose shader file that gave exactly this buffer; then fxobjects.big's entries of this unpacked size
    // (the effects come from there, the cache .fxv are stubs); then the archive entry read last with this size; then
    // the shader file read last
    if (!fx_file_of(src, len, who, sizeof(who)) && !fx_entry_of_size(len, who, sizeof(who))) {
        attribute(who, sizeof(who), len);
        const char* fx = last_fx_file();
        if (fx && !strncmp(who, "(unattributed", 13)) _snprintf_s(who, sizeof(who), _TRUNCATE, "%s (read last)", fx);
    }
    t_effect = who;   // shaders the effect creates are named after it (h_CreateVertexShader / h_CreatePixelShader)
    HRESULT hr = o_CreateEffect(dev, src, len, defs, inc, flags, pool, eff, errs);
    t_effect = nullptr;
    log_effect("effect", who, len, flags, hr, errs, now_ms() - t0);
    return hr;
}

HRESULT WINAPI h_CreateEffectFromFileW(IDirect3DDevice9* dev, LPCWSTR file, const void* defs, void* inc, DWORD flags,
                                       void* pool, void** eff, IUnknown** errs) {
    double t0 = now_ms();
    HRESULT hr = o_CreateEffectFromFileW(dev, file, defs, inc, flags, pool, eff, errs);
    std::string who = "file " + rel_path(file);
    log_effect("effectFile", who.c_str(), 0, flags, hr, errs, now_ms() - t0);
    return hr;
}

HRESULT WINAPI h_EffectCompilerFromFileW(LPCWSTR file, const void* defs, void* inc, DWORD flags, void** comp,
                                         IUnknown** errs) {
    double t0 = now_ms();
    HRESULT hr = o_EffectCompilerFromFileW(file, defs, inc, flags, comp, errs);
    std::string who = "file " + rel_path(file);
    log_effect("fxCompile", who.c_str(), 0, flags, hr, errs, now_ms() - t0);
    return hr;
}

void hook_d3dx_stub(void* obj) {
    void** vt = *reinterpret_cast<void***>(obj);
    HMODULE dx = GetModuleHandleW(L"d3dx9_27.dll");
    struct S { int off; const char* fn; void* hook; void** orig; } slots[] = {
        {0x20, "D3DXCreateEffect", (void*)h_CreateEffect, (void**)&o_CreateEffect},
        {0x24, "D3DXCreateEffectFromFileW", (void*)h_CreateEffectFromFileW, (void**)&o_CreateEffectFromFileW},
        {0x28, "D3DXCreateEffectCompilerFromFileW", (void*)h_EffectCompilerFromFileW, (void**)&o_EffectCompilerFromFileW},
        {0x40, "D3DXCreateTextureFromFileW", (void*)h_TexFromFileW, (void**)&o_TexFromFileW},
        {0x44, "D3DXCreateTextureFromFileInMemory", (void*)h_TexFromMem, (void**)&o_TexFromMem},
        {0x50, "D3DXCreateTextureFromFileInMemoryEx", (void*)h_TexFromMemEx, (void**)&o_TexFromMemEx},
        {0x5c, "D3DXCreateTextureFromFileExW", (void*)h_TexFromFileExW, (void**)&o_TexFromFileExW},
    };
    int ok = 0;
    for (auto& s : slots) {
        void* target = follow_jmps(vt[s.off / 4]);
        void* expect = dx ? reinterpret_cast<void*>(::GetProcAddress(dx, s.fn)) : nullptr;
        if (!expect || target != expect) {
            RLOG(CH_TEXTURES, L_WARN, "d3dx stub slot +0x%02x is not %s (resolves to %s) - left unhooked", s.off, s.fn,
                 addr_desc(target).c_str());
            continue;
        }
        if (patch_slot(&vt[s.off / 4], s.hook, s.orig)) ++ok;
    }
    RLOG(CH_TEXTURES, L_INFO, "d3dx stub object %p: %d/%d D3DX entry points hooked (d3dx9_27 at %p)", obj, ok,
         int(sizeof(slots) / sizeof(slots[0])), (void*)dx);
}

void* WINAPI h_GetStub() {
    void* obj = o_GetStub();
    static volatile LONG once = 0;
    if (obj && InterlockedExchange(&once, 1) == 0) hook_d3dx_stub(obj);
    return obj;
}

// ---------------------------------------------------------------- module loading
std::string loaded_from(HMODULE m) {
    if (!m) return "FAILED";
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    return rel_path(p);
}

HMODULE WINAPI h_LoadLibraryW(LPCWSTR name) {
    HMODULE m = o_LoadLibraryW(name);
    DWORD err = GetLastError();
    RLOG(CH_CORE, m ? L_INFO : L_WARN, "LoadLibrary %s -> %s", rel_path(name).c_str(), loaded_from(m).c_str());
    SetLastError(err);
    return m;
}

HMODULE WINAPI h_LoadLibraryA(LPCSTR name) {
    HMODULE m = o_LoadLibraryA(name);
    DWORD err = GetLastError();
    std::wstring w = a2w(name);
    RLOG(CH_CORE, m ? L_INFO : L_WARN, "LoadLibraryA %s -> %s", rel_path(w.c_str()).c_str(), loaded_from(m).c_str());
    SetLastError(err);
    return m;
}

FARPROC WINAPI h_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = o_GetProcAddress(m, name);
    if (!p || IS_INTRESOURCE(name)) return p;
    if (strcmp(name, "Direct3DCreate9") == 0) {
        RLOG(CH_CORE, L_INFO, "GetProcAddress Direct3DCreate9 from %s", module_name(m).c_str());
        if (!o_Direct3DCreate9) o_Direct3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(p);
        return reinterpret_cast<FARPROC>(p) == reinterpret_cast<FARPROC>(o_Direct3DCreate9)
                   ? reinterpret_cast<FARPROC>(h_Direct3DCreate9) : p;
    }
    if (strcmp(name, "get_d3dx_stub") == 0) {
        RLOG(CH_CORE, L_INFO, "GetProcAddress get_d3dx_stub from %s", module_name(m).c_str());
        if (!o_GetStub) o_GetStub = reinterpret_cast<GetStub_t>(p);
        return reinterpret_cast<FARPROC>(p) == reinterpret_cast<FARPROC>(o_GetStub)
                   ? reinterpret_cast<FARPROC>(h_GetStub) : p;
    }
    if (strcmp(name, "Direct3DCreate9Ex") == 0) RLOG(CH_CORE, L_INFO, "GetProcAddress Direct3DCreate9Ex (not wrapped)");
    RLOG(CH_CORE, L_TRACE, "GetProcAddress %s!%s", module_name(m).c_str(), name);
    return p;
}

}  // namespace

void gfx_install(HMODULE exe) {
    InitializeCriticalSection(&g_tcs);
    InitializeCriticalSection(&g_vcs);
    struct Hook { const char* fn; void* hook; void** orig; } hooks[] = {
        {"LoadLibraryW", (void*)h_LoadLibraryW, (void**)&o_LoadLibraryW},
        {"LoadLibraryA", (void*)h_LoadLibraryA, (void**)&o_LoadLibraryA},
        {"GetProcAddress", (void*)h_GetProcAddress, (void**)&o_GetProcAddress},
    };
    int ok = 0;
    for (auto& h : hooks) {
        if (patch_iat(exe, "KERNEL32.dll", h.fn, h.hook, h.orig)) ++ok;
        else RLOG(CH_CORE, L_WARN, "gfx: %s is not imported by the host, not traced", h.fn);
    }
    if (!o_GetProcAddress) o_GetProcAddress = ::GetProcAddress;
    RLOG(CH_CORE, L_INFO, "gfx: %d/3 loader APIs hooked (D3D9 and D3DX are wrapped when the game resolves them)", ok);
    if (g_cfg.tex_override && reinterpret_cast<uintptr_t>(exe) == 0x00400000) install_logical_sizes();
}

float texture_hd_factor(int axis) { return g_hd_factor[axis & 1]; }

void* record_texture(uintptr_t record) {
    uintptr_t obj;
    if (!record || !read_ptr(record + 4, &obj) || !obj) return nullptr;
    return reinterpret_cast<void*>(texture_of(obj));
}

int shadow_map_size() { return g_shadow_size; }

HRESULT STDMETHODCALLTYPE gfx_raw_set_render_target(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    return o_SetRenderTarget(d, i, s);
}
HRESULT STDMETHODCALLTYPE gfx_raw_set_texture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    return o_SetTexture(d, stage, t);
}
HRESULT STDMETHODCALLTYPE gfx_raw_set_sampler(IDirect3DDevice9* d, DWORD s, D3DSAMPLERSTATETYPE type, DWORD v) {
    return o_SetSamplerState(d, s, type, v);
}
HRESULT STDMETHODCALLTYPE gfx_raw_set_viewport(IDirect3DDevice9* d, const D3DVIEWPORT9* vp) { return o_SetViewport(d, vp); }
HRESULT STDMETHODCALLTYPE gfx_raw_set_vertex_shader(IDirect3DDevice9* d, IDirect3DVertexShader9* vs) {
    return o_SetVertexShader(d, vs);
}
HRESULT STDMETHODCALLTYPE gfx_raw_set_pixel_shader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
    return o_SetPixelShader(d, ps);
}
HRESULT STDMETHODCALLTYPE gfx_raw_stretch_rect(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT* sr,
                                               IDirect3DSurface9* dst, const RECT* dr, D3DTEXTUREFILTERTYPE f) {
    return o_StretchRect ? o_StretchRect(d, src, sr, dst, dr, f) : d->StretchRect(src, sr, dst, dr, f);
}
HRESULT gfx_raw_draw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT start, UINT count) {
    return o_DrawPrimitive(d, t, start, count);
}
HRESULT gfx_raw_draw_up(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT count, const void* vertices, UINT stride) {
    return o_DrawPrimitiveUP(d, t, count, vertices, stride);
}
HRESULT gfx_raw_draw_indexed_up(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT min_index, UINT num_vertices, UINT count,
                                const void* indices, D3DFORMAT index_format, const void* vertices, UINT stride) {
    return o_DrawIndexedPrimitiveUP(d, t, min_index, num_vertices, count, indices, index_format, vertices, stride);
}
HRESULT gfx_raw_draw_indexed(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT base_vertex, UINT min_index, UINT num_vertices,
                             UINT start, UINT count) {
    return o_DrawIndexedPrimitive(d, t, base_vertex, min_index, num_vertices, start, count);
}

HRESULT gfx_create_pixel_shader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** out) {
    return o_CreatePixelShader ? o_CreatePixelShader(d, fn, out) : d->CreatePixelShader(fn, out);
}

void gfx_summary() {
    RLOG(CH_FRAME, L_INFO, "summary: %llu frames presented, %u hitches > 100 ms", g_total_frames, g_hitches);
    if (g_dump_count) RLOG(CH_D3D, L_INFO, "summary: %ld distinct shaders disassembled to shaders\\", g_dump_count);
    EnterCriticalSection(&g_tcs);
    RLOG(CH_TEXTURES, L_INFO, "summary: %u D3DX texture loads (%u failed), source %.1f MB, GPU estimate %.1f MB, %.0f ms total",
         g_ts.count, g_ts.failed, g_ts.src_bytes / 1048576.0, g_ts.gpu_bytes / 1048576.0, g_ts.ms);
    if (g_cfg.tex_override)
        RLOG(CH_TEXTURES, L_INFO, "summary: HD overrides %ld served (%.1f MB read), %ld failed; folder %s; %zu HD "
             "textures with a logical size, %ld interface sprite sizes answered with it", g_ovr_hits,
             g_ovr_bytes / 1048576.0, g_ovr_fail, rel_path(g_cfg.tex_override_dir.c_str()).c_str(), g_logical.size(),
             g_logical_served);
    std::vector<std::pair<std::string, uint32_t>> kinds(g_ts.by_kind.begin(), g_ts.by_kind.end());
    LeaveCriticalSection(&g_tcs);
    std::sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < kinds.size() && i < 30; ++i)
        RLOG(CH_TEXTURES, L_INFO, "  %5u x %s", kinds[i].second, kinds[i].first.c_str());
    RLOG(CH_D3D, L_INFO, "summary: %ld effects created, %ld LockRect failures (%ld fixed by retry)", g_effects, g_lock_fail, g_lock_fixed);
    static const char* rows[4] = {"MAGFILTER", "MINFILTER", "MIPFILTER", "MAXANISOTROPY"};
    for (int r = 0; r < 4; ++r) {
        std::string s;
        for (int v = 0; v < 16; ++v)
            if (g_sampler[r][v]) {
                char b[48];
                _snprintf_s(b, sizeof(b), _TRUNCATE, " %d:%u", v, g_sampler[r][v]);
                s += b;
            }
        if (!s.empty()) RLOG(CH_D3D, L_INFO, "  sampler %s values seen (value:count):%s", rows[r], s.c_str());
    }
}

}  // namespace rol
