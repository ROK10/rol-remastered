// Motion probe ([debug] motion_probe = start, seconds of the test clock): how often the units on screen change from
// one frame to the next - the engine's update rates as the player sees them, against the frame rate.
//
// RoL skins its units on the CPU, in model space: their draws carry plain positions (vertex layout P3 N3 T2, no bone
// indices) that the game writes into vertex buffers, and the vertex shader places the model with its constants
// c0..c3 (world x view x projection, one register per output component: the 4th components are where the model's
// origin lands in clip space). While the probe runs it keeps a shadow copy of every vertex buffer range the game
// writes (IDirect3DVertexBuffer9 Lock / Unlock, on every buffer class's vtable, hooked through CreateVertexBuffer)
// and, at each indexed draw whose texture 0 is a unit's (art\units\...), takes the unit's matrix, the pass it belongs
// to (the render target's width: the main view draws into a target as wide as the backbuffer; the shadow map and
// other passes draw the same unit with other matrices) and - when its vertices came from a buffer written in the
// window - its pose (the vertices' offsets from their centre). A unit's matrix relative to a fixed draw's of the same
// pass, T_unit^T (T_fixed^T)^-1, does not depend on the camera: the first terrain and building draws of the main pass
// are logged as those fixed points (the terrain's vertices are in world space: its matrix is the camera's). Units
// are followed from frame to frame by pass, texture, vertex count and the nearest clip-space position.
//
// motion.log in the session folder: "T id name" once per texture; per frame "F frame t_ms dt_ms units main_width
// vp0..vp15" (vp = the view camera's view-projection from engine memory, "-" when missing); "B frame tex nv m0..m15"
// for up to 4 fixed draws of the main pass; per unit "U frame tex nv unit rt m0..m15 posed" (rt = the target's
// width, m = its c0..c3, posed = the largest change of a vertex's offset from the centre since the last frame, -1
// unknown); with [debug] motion_buildings=1 also "P frame tex nv dyn m0..m15" for every building part of the main pass
// (texture 1 is the part's, texture 0 the light map; dyn = its vertices come from a dynamic buffer, written by the CPU
// like a unit's, rather than from a static one placed by the matrix alone).
// modkit\tools\motion.py reads it. The busiest writers of dynamic vertex buffers in the window go to d3d.log.
#include "rolhook.h"

#include <d3d9.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int rol_vt_vb_Lock(void);
extern "C" int rol_vt_vb_Unlock(void);
extern "C" int rol_vt_dev_CreateVertexBuffer(void);

namespace rol {

bool g_motion_on = false;

namespace {

typedef HRESULT(STDMETHODCALLTYPE* VbLock_t)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* VbUnlock_t)(IDirect3DVertexBuffer9*);
typedef HRESULT(STDMETHODCALLTYPE* CreateVB_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                               IDirect3DVertexBuffer9**, HANDLE*);
CreateVB_t o_CreateVB = nullptr;
bool g_hooked = false;

// The runtime has several vertex buffer classes (managed, default pool, dynamic...), each with its own vtable: every
// vtable a created buffer has gets its Lock / Unlock hooked, the originals kept per vtable.
struct VbVtable {
    void** vt;
    VbLock_t lock;
    VbUnlock_t unlock;
};
VbVtable g_vts[8];
int g_nvts = 0;
SRWLOCK g_vt_lock = SRWLOCK_INIT;

const VbVtable* vtable_of(IDirect3DVertexBuffer9* vb) {
    void** vt = *reinterpret_cast<void***>(vb);
    for (int i = 0; i < g_nvts; ++i)
        if (g_vts[i].vt == vt) return &g_vts[i];
    return nullptr;
}

double g_start = -1, g_len = 0;   // the window, seconds of the test clock (start < 0: no probe)
bool g_done = false;
FILE* g_out = nullptr;

struct Shadow {
    std::vector<uint8_t> data, written;   // the buffer's bytes as the game wrote them; which bytes were written
};
std::unordered_map<IDirect3DVertexBuffer9*, Shadow> g_shadow;
struct Pending {
    UINT off, size;
    const uint8_t* ptr;
};
std::unordered_map<IDirect3DVertexBuffer9*, Pending> g_locks;

struct Rec {
    void* tex;
    UINT nv;
    UINT rt;                  // the render target's width: which pass drew it
    long id;
    float m[16];              // c0..c3
    std::vector<float> rel;   // positions minus their centre (empty: the vertices were not read)
};
std::vector<Rec> g_prev, g_cur;
struct Anchor {                // a building drawn in the frame's main pass: a fixed point to measure units against
    void* tex;
    UINT nv;
    float m[16];
};
std::vector<Anchor> g_anchors;
// [debug] motion_buildings=1: building parts (a mill's sails, a mine's engines) - how often their matrices change
bool g_buildings = false;
struct Part {
    void* tex;   // texture 1: the part's own
    UINT nv;
    bool dyn;
    float m[16];
};
std::vector<Part> g_parts;
long g_part_draws = 0;
UINT g_main_w = 0;            // the backbuffer's width: draws into a target this wide are the main pass
float g_vp[16];               // the view camera's view-projection (engine memory, row vectors) at the frame's first unit
bool g_have_vp = false;

UINT target_width(IDirect3DDevice9* d) {
    IDirect3DSurface9* s = nullptr;
    UINT w = 0;
    if (SUCCEEDED(d->GetRenderTarget(0, &s)) && s) {
        D3DSURFACE_DESC sd = {};
        if (SUCCEEDED(s->GetDesc(&sd))) w = sd.Width;
        s->Release();
    }
    return w;
}

// The view camera ([[0x01464D58] + 8], or the camera at 0x01475420): its view-projection matrix at +0x178.
bool read_view_vp(float* out) {
    __try {
        const uintptr_t app = *reinterpret_cast<const uintptr_t*>(0x01464D58);
        const uintptr_t cp = app ? *reinterpret_cast<const uintptr_t*>(app + 8) : 0;
        memcpy(out, reinterpret_cast<const uint8_t*>(cp ? cp : 0x01475420) + 0x178, 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::unordered_map<void*, int> g_tex_ids;
std::unordered_map<IDirect3DVertexDeclaration9*, bool> g_pos_first;   // declaration starts with POSITION FLOAT3 @0
long g_frames = 0, g_units = 0, g_posed = 0, g_matched = 0, g_next_id = 0;
double g_last_t = -1;

// [debug] motion_source=1: where the game copies a unit's vertices from. 30 frames into the window, the first vertex
// of a unit draw read from the shadow copy (its position, 12 bytes) is looked for in the process's writable memory
// (the probe's own copies left out) and checked against the second vertex at a stride of the source's own; the
// places found - the engine's posed-vertex cache - get write watchpoints (probe.cpp): at the next tick the code that
// poses the unit writes there, and probe.log names it with the return addresses above it. With motion_buildings the
// first building part posed on the CPU is looked for instead of a unit.
bool g_source = false, g_source_done = false;
bool g_copy = true;   // [debug] motion_vertices=0: no vertex copies (the probe only follows matrices and times)
long g_window_frames = 0;

bool equal_safe(const uint8_t* a, const uint8_t* b, size_t n) {
    __try {
        return memcmp(a, b, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool in_shadow(uintptr_t a) {
    for (const auto& kv : g_shadow) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(kv.second.data.data());
        const uintptr_t lo2 = reinterpret_cast<uintptr_t>(kv.second.written.data());
        if ((a >= lo && a < lo + kv.second.data.size()) || (a >= lo2 && a < lo2 + kv.second.written.size())) return true;
    }
    return false;
}

// One region: the places of the 12-byte position v (the next vertex, v + stride, looked for 12..64 bytes after it:
// that distance is the source's stride, 0 when it is not there). No C++ objects here: __try.
int scan_region(const uint8_t* p, size_t size, const uint8_t* v, UINT stride, uintptr_t* addr, int* step, int cap) {
    int n = 0;
    __try {
        for (size_t i = 0; i + 12 <= size && n < cap; i += 4) {
            if (*reinterpret_cast<const uint32_t*>(p + i) != *reinterpret_cast<const uint32_t*>(v)) continue;
            if (memcmp(p + i, v, 12) != 0 || in_shadow(reinterpret_cast<uintptr_t>(p + i))) continue;
            int s = 0;
            for (int k = 12; k <= 64 && !s; k += 4)
                if (i + k + 12 <= size && memcmp(p + i + k, v + stride, 12) == 0) s = k;
            addr[n] = reinterpret_cast<uintptr_t>(p + i);
            step[n] = s;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void find_source(const uint8_t* v, UINT stride, UINT nv, const std::string& name) {
    g_source_done = true;
    if (nv < 2) return;
    const double t0 = now_ms();
    constexpr int kCap = 32;
    uintptr_t addr[kCap];
    int step[kCap];
    int found = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    long regions = 0;
    uint64_t bytes = 0;
    for (uintptr_t a = 0x10000; a < 0xFFFE0000u && found < kCap;) {
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), size = mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY)) {
            ++regions;
            bytes += size;
            found += scan_region(reinterpret_cast<const uint8_t*>(base), size, v, stride, addr + found, step + found,
                                 kCap - found);
        }
        if (base + size <= a) break;
        a = base + size;
    }
    RLOG(CH_D3D, L_INFO, "motion probe: the first vertex of %s (stride %u, %u vertices) found at %d places in %ld "
         "writable regions (%llu MB) in %.0f ms", name.c_str(), stride, nv, found, regions,
         (unsigned long long)(bytes >> 20), now_ms() - t0);
    int watched = 0;
    for (int i = 0; i < found; ++i) {
        RLOG(CH_D3D, L_INFO, "  %08X: the next vertex %s%d", unsigned(addr[i]), step[i] ? "at +" : "not 12..64 bytes on ",
             step[i]);
        if (step[i] && watched < 2) {
            char label[40];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "posed vertex %08X", unsigned(addr[i]));
            if (probe_watch_now(addr[i], 4, true, label)) ++watched;
        }
    }
}

// [debug] motion_matsrc = <part of a texture name>[:<vertex count>]: where the engine keeps the world matrix of a drawn
// unit or building part, and what moves it. From 30 frames into the window, the first main-pass draw of that texture
// (and vertex count) whose matrix differs from its draw in the frame before has its world matrix worked out at the
// frame's end - W = T^T (A^T)^-1, T its c0..c3, A the frame's first terrain draw's (the terrain's vertices are in
// world space: A is the camera's) - and its translation looked for in the process's writable memory (within 0.5 world
// units); d3d.log lists each place with what lies before it (the axes as rows 4x3 or 4x4, a rotation without the
// scale as rows or columns) and the floats around it. Up to four places, those with a matrix first, get a write watch
// on the translation's component that changed most since the frame before (probe.cpp): probe.log names the code that
// moves it and how often it ran. (The Alin mine's head turns about the building's axis: its origin moves too.)
std::string g_ms_name;
UINT g_ms_nv = 0;
bool g_ms_done = false;
bool g_ms_pending = false;   // this frame's matching draw, to be looked for at the frame's end
float g_ms_t[16];            // its c0..c3
double g_ms_prev[16];        // its world matrix the frame before
long g_ms_prev_frame = -1;

void transpose4(const double* m, double* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m[c * 4 + r];
}

void mul4(const double* a, const double* b, double* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

bool inverse4(const double* m, double* out) {   // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c)
            for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][4 + c];
    return true;
}

// One region: the places of w's translation (within 0.5 world units), each with what lies before it - kind 1: the
// three axes as rows (12 floats: 4x3), 2: rows of a 4x4, 3: the axes as rows without their scale (a rotation, the
// scale kept apart), 4: that rotation transposed (columns), 0: none of these. No C++ objects here: __try.
int scan_matrix(const uint8_t* p, size_t size, const double* w, const double* rn, double tol_r, uintptr_t* out,
                int* kind, int cap) {
    int n = 0;
    __try {
        const float t0 = float(w[12]);
        for (size_t i = 48; i + 16 <= size && n < cap; i += 4) {
            const float* f = reinterpret_cast<const float*>(p + i);
            // written so that a NaN (any comparison false) never matches
            if (!(std::fabs(f[0] - t0) <= 0.5f) || !(std::fabs(f[1] - w[13]) <= 0.5) || !(std::fabs(f[2] - w[14]) <= 0.5))
                continue;
            if (in_shadow(reinterpret_cast<uintptr_t>(f))) continue;
            int k = 0;
            for (int cand = 1; cand <= 4 && !k; ++cand) {
                const int stride = cand == 2 ? 4 : 3;
                const float* m = f - 3 * stride;
                bool ok = true;
                for (int r = 0; r < 3 && ok; ++r)
                    for (int c = 0; c < 3 && ok; ++c) {
                        const double want = cand <= 2 ? w[r * 4 + c] : cand == 3 ? rn[r * 3 + c] : rn[c * 3 + r];
                        ok = std::fabs(m[r * stride + c] - want) <= (cand <= 2 ? tol_r : 0.02);   // NaN: false
                    }
                if (ok) k = cand;
            }
            out[n] = reinterpret_cast<uintptr_t>(f);
            kind[n] = k;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void context_floats(uintptr_t a, char* buf, size_t cap) {   // the 12 floats before a translation and the 4 from it
    buf[0] = 0;
    __try {
        const float* f = reinterpret_cast<const float*>(a);
        size_t len = 0;
        for (int i = -12; i < 4 && len + 16 < cap; ++i) {
            const int k = _snprintf_s(buf + len, cap - len, _TRUNCATE, "%s%.4g", i == 0 ? " | " : (i == -12 ? "" : " "),
                                      f[i]);
            if (k > 0) len += size_t(k);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void matrix_source(const double* w, const double* prev) {
    g_ms_done = true;
    double scale = 0, rn[9];   // the axes' length (the tolerance follows it); the axes without it
    for (int r = 0; r < 3; ++r) {
        const double len = std::sqrt(w[r * 4] * w[r * 4] + w[r * 4 + 1] * w[r * 4 + 1] + w[r * 4 + 2] * w[r * 4 + 2]);
        scale = std::max(scale, len);
        for (int c = 0; c < 3; ++c) rn[r * 3 + c] = len > 0 ? w[r * 4 + c] / len : 0.0;
    }
    const double tol_r = 0.02 * std::max(1.0, scale);
    int tc = 0;   // the translation's component that changed most since the frame before: watched
    for (int c = 1; c < 3; ++c)
        if (std::fabs(w[12 + c] - prev[12 + c]) > std::fabs(w[12 + tc] - prev[12 + tc])) tc = c;
    const double t0 = now_ms();
    constexpr int kCap = 16;
    uintptr_t addr[kCap];
    int kind[kCap];
    int found = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    for (uintptr_t a = 0x10000; a < 0xFFFE0000u && found < kCap;) {
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi))) break;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress), size = mbi.RegionSize;
        const DWORD prot = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_WRITECOPY))
            found += scan_matrix(reinterpret_cast<const uint8_t*>(base), size, w, rn, tol_r, addr + found, kind + found,
                                 kCap - found);
        if (base + size <= a) break;
        a = base + size;
    }
    RLOG(CH_D3D, L_INFO, "motion probe: the world matrix of %s (%u vertices) | %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f "
         "%.4f %.4f / %.2f %.2f %.2f | scale %.4f | its translation found at %d places in %.0f ms; changed since the "
         "frame before by %.3f %.3f %.3f", g_ms_name.c_str(), g_ms_nv, w[0], w[1], w[2], w[4], w[5], w[6], w[8], w[9],
         w[10], w[12], w[13], w[14], scale, found, now_ms() - t0, w[12] - prev[12], w[13] - prev[13], w[14] - prev[14]);
    static const char* const kKinds[] = {"translation only", "4x3 rows", "4x4 rows", "rotation rows + translation",
                                         "rotation columns + translation"};
    int watched = 0;
    for (int pass = 0; pass < 2; ++pass)   // the places with a matrix before them first
        for (int i = 0; i < found; ++i) {
            if ((kind[i] != 0) != (pass == 0)) continue;
            char ctx[400];
            context_floats(addr[i], ctx, sizeof(ctx));
            RLOG(CH_D3D, L_INFO, "  %08X: %s | %s", unsigned(addr[i]), kKinds[kind[i]], ctx);
            if (watched < 4) {   // every hardware slot: the copies the render makes and their source
                char label[40];
                _snprintf_s(label, sizeof(label), _TRUNCATE, "translation %c at %08X", "xyz"[tc], unsigned(addr[i]));
                if (probe_watch_now(addr[i] + uintptr_t(4 * tc), 4, true, label)) ++watched;
            }
        }
}

// At the frame's end: the matching draw's world matrix against the frame before's.
void matrix_source_frame() {
    if (!g_ms_pending) return;
    g_ms_pending = false;
    if (g_anchors.empty()) return;
    double a[16], at[16], ainv[16], t[16], tt[16], w[16];
    for (int i = 0; i < 16; ++i) a[i] = g_anchors[0].m[i], t[i] = g_ms_t[i];
    transpose4(a, at);
    if (!inverse4(at, ainv)) return;
    transpose4(t, tt);
    mul4(tt, ainv, w);
    double change = 0;
    for (int i = 0; i < 16; ++i) change = std::max(change, std::fabs(w[i] - g_ms_prev[i]));
    const bool consecutive = g_ms_prev_frame == g_frame - 1;
    if (consecutive && g_window_frames >= 30 && change > 1e-3) matrix_source(w, g_ms_prev);
    memcpy(g_ms_prev, w, sizeof(w));
    g_ms_prev_frame = g_frame;
}

void matrix_source_draw(void* tex, UINT nv, const float* m) {
    if (g_ms_name.empty() || g_ms_done || g_ms_pending) return;
    if (g_ms_nv && nv != g_ms_nv) return;
    if (texture_name(tex).find(g_ms_name) == std::string::npos) return;
    memcpy(g_ms_t, m, sizeof(g_ms_t));
    g_ms_pending = true;
}

// [debug] motion_anim=1: the animation time each model's render update samples at. 0x00E72150 (once per drawn object
// and frame) copies the time from the object's animation state (0x00EE94E0: [state + 0x38] -> object + 0x14, the
// animation handle -> + 0x10) and evaluates the animation again only when they differ from the last evaluated
// (+ 0x18 / + 0x1C). Right after the copy (0x00E72166: edi = the object, ebx = the state) the probe logs, while its
// window is open, "A frame object handle time last_handle last_time state c1 c2 c3" for up to 256 animated objects a
// frame (c = the first legends.exe return addresses on the stack: which render update asked - a unit's, a building's).
const unsigned char kAnimTimeSig[10] = {0x85, 0xDB,                                       // test ebx, ebx
                                        0xC7, 0x44, 0x24, 0x0C, 0x00, 0x00, 0x00, 0x00};   // mov [esp + 0xc], 0
void* g_anim_tramp = nullptr;
int g_anim_logged = 0;
long g_anim_frame = -1;

// The game world ([0x013E50AC], a 0xC262-byte object built by 0x006D86C0): many animation states take their time
// from its +0x38 / +0x3C (e.g. 0x00A40767: the state's time before = its time - (+0x38 - +0x3C)); "W frame +0x34
// +0x38 +0x3C +0x40 part given left ticks" each frame of the window shows how that clock runs against the frames: the
// tick's elapsed part [world + 0x90] as the engine computed it and as it was given (anim.cpp), the tick timer's time
// left as 0x006D9BA0 saw it (signed; 2147483647 = not seen), the ticks done ([0x012CF5A8]).
bool read_world_clock(int* out) {
    __try {
        const uintptr_t w = *reinterpret_cast<const uintptr_t*>(0x013E50AC);
        if (!w) return false;
        memcpy(out, reinterpret_cast<const void*>(w + 0x34), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __stdcall on_anim_time(const uint32_t* r) {   // the pushad block: edi, esi, ebp, esp, ebx, edx, ecx, eax
    if (!g_motion_on || !g_out) return;
    if (g_anim_frame != g_frame) {
        g_anim_frame = g_frame;
        g_anim_logged = 0;
    }
    if (g_anim_logged >= 256) return;
    const uint32_t* o = reinterpret_cast<const uint32_t*>(uintptr_t(r[0]));
    uint32_t c[3] = {0, 0, 0};
    game_callers(r + 9, c, 3);   // the stack at the hook: above the pushad block and the flags
    __try {
        if (o[4] == 0xFFFFFFFFu) return;   // no animation
        ++g_anim_logged;
        fprintf(g_out, "A %ld %08X %08X %d %08X %d %08X %06x %06x %06x\n", (long)g_frame, r[0], o[4], int(o[5]), o[6],
                int(o[7]), r[4], c[0], c[1], c[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void h_anim_time() {
    __asm {
        pushfd
        pushad
        push esp
        call on_anim_time
        popad
        popfd
        jmp dword ptr [g_anim_tramp]
    }
}

// [debug] motion_xform=1: every drawn object's transform as the render takes it. 0x00E71AF0 copies a request's
// transform (ecx: a rotation quaternion, the translation, ... - 12 floats; the request is made anew every frame) into
// the render object (eax) and the objects chained to it; two of its callers do it for every model each frame:
// 0x00A6AD5E (the unit render update's path, 0x00A6AD30) and 0x00E7F016 (0x00E7EF30). Their calls are pointed at a
// logger that goes on to 0x00E71AF0: "X frame robj site qx qy qz qw tx ty tz" for up to 1024 copies a frame (site:
// the call's return address) - with the W lines' ticks, which objects move only when a tick runs.
constexpr uintptr_t kXformFn = 0x00E71AF0;
const uintptr_t kXformSites[] = {0x00A6AD5E, 0x00E7F016};
int g_xform_sites = 0;
int g_xform_logged = 0;
long g_xform_frame = -1;

void __stdcall on_xform(const uint32_t* r) {   // the pushad block: edi, esi, ebp, esp, ebx, edx, ecx, eax; then the return
    if (!g_motion_on || !g_out) return;
    if (g_xform_frame != g_frame) {
        g_xform_frame = g_frame;
        g_xform_logged = 0;
    }
    if (g_xform_logged >= 1024) return;
    ++g_xform_logged;
    __try {
        const float* s = reinterpret_cast<const float*>(uintptr_t(r[6]));
        fprintf(g_out, "X %ld %08X %06X %.6f %.6f %.6f %.6f %.3f %.3f %.3f\n", (long)g_frame, r[7], r[8] - 5, s[0], s[1],
                s[2], s[3], s[4], s[5], s[6]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void h_xform() {
    __asm {
        pushad
        push esp
        call on_xform
        popad
        push 0x00E71AF0   // on to the copy; it returns to the call site
        ret
    }
}

void xform_install() {
    for (uintptr_t site : kXformSites) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
        if (p[0] != 0xE8 || site + 5 + *reinterpret_cast<const int32_t*>(p + 1) != kXformFn) {
            RLOG(CH_D3D, L_WARN, "motion probe: the call at %08X is not to 0x00E71AF0 - not logged", unsigned(site));
            continue;
        }
        const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(&h_xform) - (site + 5));
        if (patch_code(site + 1, p + 1, &rel, 4)) ++g_xform_sites;
    }
    RLOG(CH_D3D, L_INFO, "motion probe: transforms logged at %d of 2 calls of 0x00E71AF0", g_xform_sites);
}

bool copy_safe(uint8_t* dst, const uint8_t* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Who writes the dynamic buffers while the probe runs: the chain of legends.exe return addresses above each Lock,
// counted, with the frames it was seen on.
struct Chain {
    long locks = 0, frames = 0, last_frame = -1;
};
std::unordered_map<std::string, Chain> g_chains;
long g_dyn_locks = 0;

void note_lock_chain(const void* sp) {
    uint32_t c[8];
    const int n = game_callers(sp, c, 8);
    char key[96] = "";
    for (int i = 0; i < n; ++i) _snprintf_s(key + strlen(key), sizeof(key) - strlen(key), _TRUNCATE, " %06x", c[i]);
    Chain& ch = g_chains[key];
    ++ch.locks;
    if (ch.last_frame != g_frame) {
        ++ch.frames;
        ch.last_frame = g_frame;
    }
    ++g_dyn_locks;
}

HRESULT STDMETHODCALLTYPE h_VbLock(IDirect3DVertexBuffer9* vb, UINT off, UINT size, void** p, DWORD flags) {
    const VbVtable* v = vtable_of(vb);
    if (!v) return E_FAIL;   // cannot happen: only hooked vtables lead here
    if (g_motion_on && g_copy && !(flags & D3DLOCK_READONLY)) {
        D3DVERTEXBUFFER_DESC d = {};
        vb->GetDesc(&d);
        if (d.Usage & D3DUSAGE_DYNAMIC) note_lock_chain(_AddressOfReturnAddress());
    }
    HRESULT hr = v->lock(vb, off, size, p, flags);
    if (g_motion_on && g_copy && SUCCEEDED(hr) && p && *p && !(flags & D3DLOCK_READONLY)) {
        if (size == 0) {
            D3DVERTEXBUFFER_DESC d = {};
            vb->GetDesc(&d);
            size = d.Size > off ? d.Size - off : 0;
        }
        g_locks[vb] = {off, size, static_cast<const uint8_t*>(*p)};
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE h_VbUnlock(IDirect3DVertexBuffer9* vb) {
    if (g_motion_on && !g_locks.empty()) {
        auto it = g_locks.find(vb);
        if (it != g_locks.end()) {
            const Pending pl = it->second;
            g_locks.erase(it);
            Shadow& s = g_shadow[vb];
            if (s.data.empty()) {
                D3DVERTEXBUFFER_DESC d = {};
                vb->GetDesc(&d);
                s.data.assign(d.Size, 0);
                s.written.assign(d.Size, 0);
            }
            if (size_t(pl.off) + pl.size <= s.data.size() && copy_safe(&s.data[pl.off], pl.ptr, pl.size))
                memset(&s.written[pl.off], 1, pl.size);
        }
    }
    const VbVtable* v = vtable_of(vb);
    return v ? v->unlock(vb) : E_FAIL;
}

void hook_vb_vtable(IDirect3DVertexBuffer9* vb) {
    void** vt = *reinterpret_cast<void***>(vb);
    AcquireSRWLockExclusive(&g_vt_lock);
    bool known = false;
    for (int i = 0; i < g_nvts; ++i) known = known || g_vts[i].vt == vt;
    if (!known && g_nvts < 8) {
        VbVtable& e = g_vts[g_nvts];
        e.vt = vt;
        e.lock = reinterpret_cast<VbLock_t>(vt[rol_vt_vb_Lock()]);
        e.unlock = reinterpret_cast<VbUnlock_t>(vt[rol_vt_vb_Unlock()]);
        ++g_nvts;   // before the slots point at the hooks: they look the originals up
        void* old;
        patch_slot(&vt[rol_vt_vb_Lock()], (void*)h_VbLock, &old);
        patch_slot(&vt[rol_vt_vb_Unlock()], (void*)h_VbUnlock, &old);
        D3DVERTEXBUFFER_DESC d = {};
        vb->GetDesc(&d);
        RLOG(CH_D3D, L_INFO, "motion probe: vertex buffer vtable %p hooked (first seen: %u bytes, usage 0x%lx, pool %d)",
             vt, d.Size, d.Usage, int(d.Pool));
    }
    ReleaseSRWLockExclusive(&g_vt_lock);
}

HRESULT STDMETHODCALLTYPE h_CreateVB(IDirect3DDevice9* d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                     IDirect3DVertexBuffer9** out, HANDLE* sh) {
    HRESULT hr = o_CreateVB(d, len, usage, fvf, pool, out, sh);
    if (SUCCEEDED(hr) && out && *out) hook_vb_vtable(*out);
    return hr;
}

bool position_first(IDirect3DDevice9* d) {
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(d->GetVertexDeclaration(&decl)) || !decl) return false;
    bool ok;
    auto it = g_pos_first.find(decl);
    if (it != g_pos_first.end()) {
        ok = it->second;
    } else {
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
        UINT n = 0;
        ok = SUCCEEDED(decl->GetDeclaration(el, &n)) && n > 0 && el[0].Stream == 0 && el[0].Offset == 0 &&
             el[0].Type == D3DDECLTYPE_FLOAT3 && el[0].Usage == D3DDECLUSAGE_POSITION;
        g_pos_first[decl] = ok;
    }
    decl->Release();
    return ok;
}

int tex_id(void* t) {
    auto it = g_tex_ids.find(t);
    if (it != g_tex_ids.end()) return it->second;
    const int id = int(g_tex_ids.size());
    g_tex_ids[t] = id;
    if (g_out) fprintf(g_out, "T %d %s\n", id, texture_name(t).c_str());
    return id;
}

void open_window() {
    if (_wfopen_s(&g_out, (g_cfg.session_dir + L"motion.log").c_str(), L"w") != 0 || !g_out) {
        g_out = nullptr;
        g_done = true;
        RLOG(CH_D3D, L_ERROR, "motion probe: cannot write motion.log");
        return;
    }
    fprintf(g_out, "motion probe: %g s from %g s of the test clock (frame %ld). T id texture; F frame t_ms dt_ms units "
                   "main_width vp0..vp15 (the view camera's view-projection from engine memory, - when missing); B frame "
                   "tex nv m0..m15 (up to 4 fixed draws of the main pass - terrain, buildings - their c0..c3); U frame tex "
                   "nv unit rt m0..m15 "
                   "posed (rt: the render target's width; the unit's c0..c3; posed: the largest change of a vertex's "
                   "offset from the centre since the last frame, -1 unknown); P frame tex nv dyn m0..m15 (a building "
                   "part of the main pass: texture 1, dyn 1 = dynamic vertex buffer)\n",
            g_len, g_start, (long)g_frame);
    g_motion_on = true;
    g_last_t = -1;
    g_anchors.clear();
    g_parts.clear();
    RLOG(CH_D3D, L_INFO, "motion probe: on for %g s (frame %ld)", g_len, (long)g_frame);
}

void close_window() {
    g_motion_on = false;
    g_done = true;
    if (g_out) fclose(g_out);
    g_out = nullptr;
    g_shadow.clear();
    g_locks.clear();
    g_prev.clear();
    g_cur.clear();
    RLOG(CH_D3D, L_INFO, "motion probe: off - %ld frames, %ld unit draws (%ld with their pose read, %ld matched to the "
         "frame before), %ld building part draws", g_frames, g_units, g_posed, g_matched, g_part_draws);
    std::vector<std::pair<long, std::string>> top;
    for (auto& kv : g_chains) top.push_back({kv.second.locks, kv.first});
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    RLOG(CH_D3D, L_INFO, "motion probe: %ld locks of dynamic vertex buffers by %u call chains; the busiest:", g_dyn_locks,
         unsigned(g_chains.size()));
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
        const Chain& ch = g_chains[top[i].second];
        RLOG(CH_D3D, L_INFO, "  %6ld locks on %4ld frames:%s", ch.locks, ch.frames, top[i].second.c_str());
    }
}

// The frame's unit draws against the last frame's: same texture and vertex count, nearest clip-space origin.
void end_frame(double t_ms) {
    const double dt = g_last_t < 0 ? 0.0 : t_ms - g_last_t;
    g_last_t = t_ms;
    ++g_frames;
    ++g_window_frames;
    matrix_source_frame();   // before the frame's anchors go
    fprintf(g_out, "F %ld %.3f %.3f %u %u", (long)g_frame, t_ms, dt, unsigned(g_cur.size()), g_main_w);
    if (g_have_vp)
        for (float v : g_vp) fprintf(g_out, " %.6g", v);
    else
        fputs(" -", g_out);
    fputc('\n', g_out);
    g_have_vp = false;
    if (g_anim_tramp) {   // with the animation times: the game world's clock ([0x013E50AC] + 0x38 / + 0x3C)
        int w[4], engine = 0, given = 0, left = 0;
        const bool sm = anim_clock_values(&engine, &given, &left);
        if (read_world_clock(w)) {
            fprintf(g_out, "W %ld %d %d %d %d", (long)g_frame, w[0], w[1], w[2], w[3]);
            // the tick's part as the engine had it and as given, the timer's time left, the ticks done
            if (sm) fprintf(g_out, " %d %d %d %d", engine, given, left, *reinterpret_cast<const int*>(0x012CF5A8));
            fputc('\n', g_out);
        }
    }
    for (const Anchor& a : g_anchors) {
        fprintf(g_out, "B %ld %d %u", (long)g_frame, tex_id(a.tex), a.nv);
        for (float v : a.m) fprintf(g_out, " %.9g", v);
        fputc('\n', g_out);
    }
    g_anchors.clear();
    for (const Part& pt : g_parts) {
        fprintf(g_out, "P %ld %d %u %d", (long)g_frame, tex_id(pt.tex), pt.nv, pt.dyn ? 1 : 0);
        for (float v : pt.m) fprintf(g_out, " %.9g", v);
        fputc('\n', g_out);
    }
    g_parts.clear();
    std::vector<char> taken(g_prev.size(), 0);
    for (Rec& r : g_cur) {
        int best = -1;
        double bd = 1e30;
        for (size_t k = 0; k < g_prev.size(); ++k) {
            const Rec& p = g_prev[k];
            if (taken[k] || p.tex != r.tex || p.nv != r.nv || p.rt != r.rt) continue;
            double dd = 0;
            for (int i : {3, 7, 15}) dd += double(r.m[i] - p.m[i]) * double(r.m[i] - p.m[i]);
            if (dd < bd) {
                bd = dd;
                best = int(k);
            }
        }
        double posed = -1;
        r.id = -1;
        if (best >= 0) {
            taken[best] = 1;
            ++g_matched;
            r.id = g_prev[best].id;
            const std::vector<float>& a = r.rel;
            const std::vector<float>& b = g_prev[best].rel;
            if (!a.empty() && a.size() == b.size()) {
                float m = 0;
                for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
                posed = m;
            }
        }
        if (r.id < 0) r.id = g_next_id++;
        fprintf(g_out, "U %ld %d %u %ld %u", (long)g_frame, tex_id(r.tex), r.nv, r.id, r.rt);
        for (float v : r.m) fprintf(g_out, " %.6g", v);
        fprintf(g_out, " %.5f\n", posed);
    }
    g_prev.swap(g_cur);
    g_cur.clear();
}

}  // namespace

void motion_install() {
    const std::wstring v = config_str(L"debug", L"motion_probe", L"");
    if (v.empty()) return;
    double a = -1, b = 0;
    if (swscanf_s(v.c_str(), L"%lf , %lf", &a, &b) < 1 || a < 0) {
        RLOG(CH_D3D, L_WARN, "motion probe: [debug] motion_probe=%s not understood (start, seconds)", w2u(v.c_str()).c_str());
        return;
    }
    g_start = a;
    g_len = b > 0 ? b : 3.0;
    g_source = config_int(L"debug", L"motion_source", 0, true) != 0;
    g_copy = config_int(L"debug", L"motion_vertices", 1, true) != 0;
    g_buildings = config_int(L"debug", L"motion_buildings", 0, true) != 0;
    {   // motion_matsrc = <part of a texture name>[:<vertex count>]
        const std::string ms = w2u(config_str(L"debug", L"motion_matsrc", L"").c_str());
        const size_t colon = ms.rfind(':');
        g_ms_name = colon == std::string::npos ? ms : ms.substr(0, colon);
        g_ms_nv = colon == std::string::npos ? 0 : UINT(atoi(ms.c_str() + colon + 1));
        if (!g_ms_name.empty())
            RLOG(CH_D3D, L_INFO, "motion probe: the world matrix of %s (%u vertices, 0 = any) will be looked for",
                 g_ms_name.c_str(), g_ms_nv);
    }
    if (config_int(L"debug", L"motion_xform", 0, true)) xform_install();
    if (config_int(L"debug", L"motion_anim", 0, true)) {
        g_anim_tramp = detour(0x00E72166, kAnimTimeSig, sizeof(kAnimTimeSig), (void*)h_anim_time);
        RLOG(CH_D3D, g_anim_tramp ? L_INFO : L_ERROR, "motion probe: animation times %s", g_anim_tramp ? "logged" :
             "NOT logged (code differs)");
    }
    RLOG(CH_D3D, L_INFO, "motion probe: %g s from %g s of the test clock%s", g_len, g_start,
         g_buildings ? ", building parts too" : "");
}

void motion_device(IDirect3DDevice9* d) {
    if (g_start < 0 || g_hooked || !d) return;
    void** vt = *reinterpret_cast<void***>(d);
    g_hooked = patch_slot(&vt[rol_vt_dev_CreateVertexBuffer()], (void*)h_CreateVB, (void**)&o_CreateVB);
    RLOG(CH_D3D, g_hooked ? L_INFO : L_ERROR, "motion probe: CreateVertexBuffer %s", g_hooked ? "hooked" : "NOT hooked");
    if (!g_hooked) g_start = -1;
}

void motion_draw(IDirect3DDevice9* d, INT bv, UINT mi, UINT nv) {
    if (!g_motion_on || !nv) return;
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(d->GetTexture(0, &t)) || !t) return;
    t->Release();
    const std::string name = texture_name(t);
    const bool unit = name.find("\\units\\") != std::string::npos;
    // fixed points of the main pass: the terrain (world-space vertices - its matrix is the camera's) and buildings
    const bool building = !unit && (name.find("\\terrain\\") != std::string::npos ||
                                    name.find("\\buildings\\") != std::string::npos);
    // a lit building part: the light map (a render target) as texture 0, the part's texture as texture 1
    const bool part = g_buildings && !unit && !building && name.compare(0, 3, "rt ") == 0;
    if (!unit && !part && !(building && g_anchors.size() < 4)) return;
    if (!g_main_w) {
        IDirect3DSurface9* bb = nullptr;
        D3DSURFACE_DESC sd = {};
        if (SUCCEEDED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            if (SUCCEEDED(bb->GetDesc(&sd))) g_main_w = sd.Width;
            bb->Release();
        }
    }
    const UINT rt = target_width(d);
    void* own_tex = t;
    if (part) {
        IDirect3DBaseTexture9* t1 = nullptr;
        if (rt != g_main_w || FAILED(d->GetTexture(1, &t1)) || !t1) return;
        t1->Release();
        if (texture_name(t1).find("\\buildings\\") == std::string::npos) return;
        IDirect3DVertexShader9* vs = nullptr;
        if (FAILED(d->GetVertexShader(&vs)) || !vs) return;   // a fixed-function draw (flares) keeps texture 1 bound
        vs->Release();
        Part pt;
        pt.tex = t1;
        pt.nv = nv;
        pt.dyn = false;
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        if (SUCCEEDED(d->GetStreamSource(0, &vb, &off, &stride)) && vb) {
            D3DVERTEXBUFFER_DESC vd = {};
            if (SUCCEEDED(vb->GetDesc(&vd))) pt.dyn = (vd.Usage & D3DUSAGE_DYNAMIC) != 0;
            vb->Release();
        }
        if (FAILED(d->GetVertexShaderConstantF(0, pt.m, 4))) return;
        matrix_source_draw(t1, nv, pt.m);
        g_parts.push_back(pt);
        ++g_part_draws;
        // a part posed on the CPU (its vertices in a dynamic buffer, like a unit's - a mill's sails, a flag): its pose
        // is measured like a unit's as well, a "U" line with texture 1
        if (!pt.dyn) return;
        own_tex = t1;
    }
    if (building) {
        Anchor a;
        a.tex = t;
        a.nv = nv;
        if (rt == g_main_w && SUCCEEDED(d->GetVertexShaderConstantF(0, a.m, 4))) g_anchors.push_back(a);
        return;
    }
    if (!g_have_vp) g_have_vp = read_view_vp(g_vp);
    Rec r;
    r.tex = own_tex;
    r.nv = nv;
    r.rt = rt;
    if (FAILED(d->GetVertexShaderConstantF(0, r.m, 4))) return;
    if (rt == g_main_w && own_tex == t) matrix_source_draw(t, nv, r.m);   // a unit (parts were offered above)
    ++g_units;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (SUCCEEDED(d->GetStreamSource(0, &vb, &off, &stride)) && vb) {
        vb->Release();
        auto it = g_shadow.find(vb);
        const size_t first = size_t(off) + size_t(int64_t(bv) + mi) * stride, end = first + size_t(nv) * stride;
        if (it != g_shadow.end() && stride >= 12 && end <= it->second.data.size() && it->second.written[first] &&
            it->second.written[end - 1] && position_first(d)) {
            const uint8_t* p = &it->second.data[first];
            double sum[3] = {0, 0, 0};
            for (UINT i = 0; i < nv; ++i) {
                const float* f = reinterpret_cast<const float*>(p + size_t(i) * stride);
                for (int k = 0; k < 3; ++k) sum[k] += f[k];
            }
            r.rel.resize(size_t(nv) * 3);
            for (UINT i = 0; i < nv; ++i) {
                const float* f = reinterpret_cast<const float*>(p + size_t(i) * stride);
                for (int k = 0; k < 3; ++k) r.rel[size_t(i) * 3 + k] = float(f[k] - sum[k] / nv);
            }
            ++g_posed;
            // the source of a building part's vertices with motion_buildings, else of a unit's
            if (g_source && !g_source_done && g_window_frames >= 30 && rt == g_main_w && (own_tex != t) == g_buildings)
                find_source(p, stride, nv, texture_name(own_tex));
        }
    }
    g_cur.push_back(std::move(r));
}

void motion_on_present() {
    if (g_start < 0 || g_done) return;
    const double t = test_clock();
    if (!g_motion_on) {
        if (t >= g_start) open_window();
        return;
    }
    end_frame(now_ms());
    if (t >= g_start + g_len) close_window();
}

void motion_summary() {
    if (g_start < 0) return;
    if (!g_done) RLOG(CH_D3D, L_WARN, "motion probe: the window %s", g_motion_on ? "was still open" : "never opened");
}

}  // namespace rol
