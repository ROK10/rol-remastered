// Game camera zoom range, applied to the live camera.
//
// GameCamera keeps the REGULATION values of data\gamecamera.xml in plain fields and reads them on every zoom
// step (0x006BBFEB: step = (out - in) / levels), so writing the fields changes the zoom range at once.
// When a game starts, the camera (0x006BC0B3) binds its settings sub-object (camera + 0xD40, vtable 0x0111B654)
// to a schema (slot 5), loads data\gamecamera.xml through it (0x006BFDA0) and then calls the sub-object's
// on_loaded (slot 8). rolhook overrides the fields right after on_loaded and remembers the camera so the
// options tab can change them in the middle of a game; the camera's deleting destructor (vtable 0x0111B688
// slot 0) forgets it. Only the GameCamera constructor/destructor use vtable 0x0111B654.
// The zoom animation is kept from overshooting the closest and farthest zoom ([camera] stop_at_limits) and its
// spring back after a wheel step is adjustable ([camera] zoom_spring), both below.
//
// Addresses are for legends.exe of patch 2.5, image base 0x400000; every hook is checked against code bytes.
#include "rolhook.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rol {

void camera_limits_install();   // below: the zoom held at its limits
void camera_forget_zoom();

namespace {

constexpr uintptr_t VT_CAM_SETTINGS = 0x0111B654;   // settings sub-object at camera + 0xD40
constexpr int SLOT_LOADED = 8;
constexpr uintptr_t FN_LOADED = 0x00D40F10;         // void on_loaded() — generic, shared with other classes'
                                                    // vtables; only this vtable's slot is patched
constexpr uintptr_t VT_CAMERA = 0x0111B688;
constexpr int SLOT_DTOR = 0;
constexpr uintptr_t FN_DTOR = 0x006B7140;           // scalar deleting destructor (flags)
constexpr int SETTINGS_OFS = 0xD40;
// fields of the camera object (REGULATION in gamecamera.xml)
constexpr int CAM_NUM_ZOOM_LEVELS = 0xD48;   // int
constexpr int CAM_ZOOM_IN_DIST = 0xD4C;      // float
constexpr int CAM_ZOOM_OUT_DIST = 0xD50;     // float
constexpr int CAM_ZOOM_OUT_PAN = 0xD58;      // float, scroll speed at the far zoom
constexpr int CAM_EXT_ZOOM_OUT_DIST = 0xD5C; // float

const unsigned char kLoadedSig[8] = {0x56, 0x8B, 0xF1, 0x8B, 0x06, 0xFF, 0x50, 0x24};
const unsigned char kDtorSig[8] = {0x56, 0x8B, 0xF1, 0xE8, 0x58, 0xFF, 0xFF, 0xFF};

typedef void(__thiscall* Loaded_t)(void* self);
typedef void*(__thiscall* Dtor_t)(void* self, unsigned flags);
Loaded_t o_loaded = nullptr;
Dtor_t o_dtor = nullptr;

void* volatile g_camera = nullptr;
bool g_installed = false;

template <typename T>
T* field(void* cam, int ofs) {
    return reinterpret_cast<T*>(static_cast<uint8_t*>(cam) + ofs);
}

// The engine's own pairs: 7500 -> 8600 extended, pan 450; modkit's camera_zoom: 11000 -> 14000, pan 660.
float extended_for(int out) { return out <= 7500 ? 8600.0f : std::round(out * (14000.0f / 11000.0f) / 100.0f) * 100.0f; }
float pan_for(int out) { return 450.0f * float(out) / 7500.0f; }

bool apply(void* cam, const char* why) {
    if (!cam || (g_cfg.zoom_out <= 0 && g_cfg.zoom_steps <= 0 && g_cfg.zoom_in <= 0)) return false;
    __try {
        if (*reinterpret_cast<uintptr_t*>(static_cast<uint8_t*>(cam) + SETTINGS_OFS) != VT_CAM_SETTINGS) return false;
        if (g_cfg.zoom_in > 0) *field<float>(cam, CAM_ZOOM_IN_DIST) = float(g_cfg.zoom_in);
        if (g_cfg.zoom_out > 0) {
            *field<float>(cam, CAM_ZOOM_OUT_DIST) = float(g_cfg.zoom_out);
            *field<float>(cam, CAM_EXT_ZOOM_OUT_DIST) = extended_for(g_cfg.zoom_out);
            *field<float>(cam, CAM_ZOOM_OUT_PAN) = pan_for(g_cfg.zoom_out);
        }
        if (g_cfg.zoom_steps > 0) *field<int>(cam, CAM_NUM_ZOOM_LEVELS) = g_cfg.zoom_steps;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    RLOG(CH_UI, L_INFO, "camera %p (%s): zoom in %.0f, out %.0f (extended %.0f, pan %.0f), %d steps", cam, why,
         *field<float>(cam, CAM_ZOOM_IN_DIST), *field<float>(cam, CAM_ZOOM_OUT_DIST),
         *field<float>(cam, CAM_EXT_ZOOM_OUT_DIST), *field<float>(cam, CAM_ZOOM_OUT_PAN),
         *field<int>(cam, CAM_NUM_ZOOM_LEVELS));
    return true;
}

void __fastcall h_loaded(void* self, void* /*edx*/) {
    o_loaded(self);
    void* cam = static_cast<uint8_t*>(self) - SETTINGS_OFS;
    g_camera = cam;
    camera_forget_zoom();
    if (!apply(cam, "gamecamera.xml loaded"))
        RLOG(CH_UI, L_INFO, "camera %p: gamecamera.xml loaded, zoom kept as in the file", cam);
}

void* __fastcall h_dtor(void* self, void* /*edx*/, unsigned flags) {
    if (self == g_camera) {
        g_camera = nullptr;
        camera_forget_zoom();
    }
    return o_dtor(self, flags);
}

}  // namespace

void camera_install(HMODULE exe) {
    if (reinterpret_cast<uintptr_t>(exe) != 0x00400000 ||
        memcmp(reinterpret_cast<const void*>(FN_LOADED), kLoadedSig, 8) != 0 ||
        memcmp(reinterpret_cast<const void*>(FN_DTOR), kDtorSig, 8) != 0) {
        RLOG(CH_UI, L_WARN, "camera hooks off: unknown legends.exe build");
        return;
    }
    void** vs = reinterpret_cast<void**>(VT_CAM_SETTINGS);
    void** vc = reinterpret_cast<void**>(VT_CAMERA);
    if (reinterpret_cast<uintptr_t>(vs[SLOT_LOADED]) != FN_LOADED ||
        reinterpret_cast<uintptr_t>(vc[SLOT_DTOR]) != FN_DTOR) {
        RLOG(CH_UI, L_WARN, "camera hooks off: vtable layout differs");
        return;
    }
    g_installed = patch_slot(&vs[SLOT_LOADED], (void*)h_loaded, (void**)&o_loaded) &&
                  patch_slot(&vc[SLOT_DTOR], (void*)h_dtor, (void**)&o_dtor);
    RLOG(CH_UI, L_INFO, "camera hooks %s; zoom from config: in %d, out %d, %d steps (0 = keep gamecamera.xml)",
         g_installed ? "installed" : "FAILED", g_cfg.zoom_in, g_cfg.zoom_out, g_cfg.zoom_steps);
    if (g_installed) camera_limits_install();
}

// The live camera's zoom. The distance is animated: an animator of the engine's camera (0x00EFxxxx) at +0x8F4 has
// the end of the current animation at +0x90C — where the last wheel step aims — and the distance of this frame at
// +0x918, evaluated from the animation's curve every frame before the view is built (GameCamera: inline in
// 0x006C1D60; the engine's camera: 0x00EFC480 from 0x00EFDA80).
// The wheel step (0x006C0230) takes (out - in) / levels from +0x918 (or +0x90C), clamps the aim to the range
// [+0xD8C, +0xD90] into +0xD88 and starts a new animation (0x00EFD250 -> 0x00EFC550): up to the extended distance
// while the tilt (+0x708) is at least regulation_extended_zoom_out_tilt_angle (+0xD60), else up to zoom_out
// (0x006C0B30 pulls an aim above zoom_out back while the tilt is below it).
namespace {
constexpr int CAM_ZOOM_AIM = 0x90C, CAM_ZOOM_DIST = 0x918, CAM_STEP_AIM = 0xD88, CAM_LO = 0xD8C, CAM_HI = 0xD90,
              CAM_TILT = 0x708, CAM_EXT_TILT = 0xD60, CAM_STATE = 0xD68, ZOOM_ANIMATOR = 0x8F4;

bool read_zoom(void* cam, float* dist, float* aim, float* in, float* out, float* ext, int* levels) {
    __try {
        *dist = *field<float>(cam, CAM_ZOOM_DIST);
        *aim = *field<float>(cam, CAM_ZOOM_AIM);
        *in = *field<float>(cam, CAM_ZOOM_IN_DIST);
        *out = *field<float>(cam, CAM_ZOOM_OUT_DIST);
        *ext = *field<float>(cam, CAM_EXT_ZOOM_OUT_DIST);
        *levels = *field<int>(cam, CAM_NUM_ZOOM_LEVELS);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The zoom's motion. Each frame GameCamera calls its zoom method (vtable +0x48, 0x006BE830 -> step 0x006C0230)
// twice, from its key handling (0x006BD9E0: one step in) and from 0x00EFD1E0 (one step out): the aim ends where it
// was, but every call restarts the animation (0x006C0B30 -> 0x00EFD250 -> 0x00EFC550, curve builder 0x00D837C0
// called at 0x00EFC688) from the current distance, with the slope of the running curve as its speed and the full
// transition time ahead. A wheel notch adds one more restart. So the motion is planned anew every frame, keeping
// its speed and the deadline: it arrives too fast, goes past the aim and springs back — by up to ~60 % of a step
// after a few quick notches, and past the closest and farthest zoom too (autotest ui_zoom_limit, zoom_spring_3,
// 2026-10-06: 6875 restarts in 3400 frames, all through 0x006BE86A).
//
// The curve is two degree-7 polynomials over the halves of the animation (coefficients at animator + 0xBC + 4 and
// + 0xE0 + 4, x = 0..1 in each half). GameCamera's per-frame update (0x006C1D60) evaluates it and stores the
// distance at 0x006C20FB, after the frame's restarts; right after (0x006C2100, esi = the animator, edi = the time),
// before 0x006C2182 builds the view:
//  * [camera] zoom_spring (Mods tab "Zoom Bounce"): 0 none, 1 weak, 2 medium, 3 as the game. When the frame's last
//    restart (the curve the camera follows) goes past its aim, or back behind its start, by more than the level
//    allows, the curve is built again by the engine's builder with the carried speed scaled down (bisection): the
//    motion keeps as much speed as fits and arrives without springing back. The frame's earlier restarts (the
//    zoom-in half of the pair) are left alone: cutting their speed starved the one that counts (0.9.2 first build:
//    the zoom crawled at 3 units a frame).
//  * [camera] stop_at_limits (default 1): a distance that crossed a bound of the range [+0xD8C, +0xD90] from inside
//    is held at the bound; one already outside (the pull back from the extended zoom) comes in on its own.
constexpr uintptr_t AT_ZOOM_STORED = 0x006C2100;
const unsigned char kZoomSig[9] = {0x89, 0x7E, 0x0C,                       // mov [esi + 0xC], edi
                                   0x8B, 0xBD, 0xE0, 0x06, 0x00, 0x00};    // mov edi, [ebp + 0x6E0]
constexpr uintptr_t AT_BUILD_CALL = 0x00EFC688;   // call 0x00D837C0 inside 0x00EFC550
constexpr uintptr_t FN_BUILD = 0x00D837C0;        // stdcall (builder, start, end, speed, 0)
typedef void(__stdcall* Build_t)(void* builder, float start, float end, float speed, float extra);
const float kSpringAllow[3] = {0.002f, 0.10f, 0.25f};   // overshoot allowed, in zoom steps: none / weak / medium
constexpr int ANIM_T0 = 0x04, ANIM_RATE = 0x10, ANIM_END = 0x18, ANIM_VALUE = 0x24, ANIM_SEG0 = 0xBC, ANIM_SEG1 = 0xE0;

void* g_zoom_tramp = nullptr;
bool g_hold_limits = true;
volatile bool g_prev_ok = false;
float g_prev_dist = 0;
long g_held = 0;
long g_spring_restarts = 0, g_spring_frames = 0, g_spring_damped = 0;   // restarts / frames with one / curves cut
int g_trace = -1;   // [debug] log_camera, read at the first Present (-1 = not read yet)
struct Restart {   // the zoom animation's last restart, kept for the evaluation of its frame
    bool valid;
    void* builder;
    float start, end, speed, extra;
} g_last = {};

float poly8(const float* c, float x) {
    float s = 0, p = 1;
    for (int i = 0; i < 8; ++i, p *= x) s += c[i] * p;
    return s;
}

const float* segment(const uint8_t* anim, int half) {
    return reinterpret_cast<const float*>(anim + (half ? ANIM_SEG1 : ANIM_SEG0) + 4);
}

// How far the zoom animator's curve goes past 'end' (or back behind 'start'), in distance units.
float overshoot(const uint8_t* anim, float start, float end) {
    const float d = end - start;
    const float sign = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : 0.0f);
    float worst = 0;
    for (int h = 0; h < 2; ++h) {
        for (int i = 0; i <= 24; ++i) {
            const float y = poly8(segment(anim, h), float(i) / 24.0f);
            const float o = sign != 0.0f ? std::max(sign * (y - end), sign * (start - y)) : std::fabs(y - end);
            worst = std::max(worst, o);
        }
    }
    return worst;
}

// The curve's value at 'now', as 0x00EFC480 computes it.
float curve_value(const uint8_t* anim, uint32_t now) {
    const uint32_t t0 = *reinterpret_cast<const uint32_t*>(anim + ANIM_T0);
    const float p = float(int32_t(now - t0)) * *reinterpret_cast<const float*>(anim + ANIM_RATE);
    if (p >= 1.0f) return *reinterpret_cast<const float*>(anim + ANIM_END);
    if (p < 0.0f) return poly8(segment(anim, 0), 0.0f);
    return p < 0.5f ? poly8(segment(anim, 0), p / 0.5f) : poly8(segment(anim, 1), (p - 0.5f) / 0.5f);
}

// zoom_spring: the curve of the frame's last restart (the restarts run with the input, a frame's time before the
// evaluation, so the curve is already a frame along). Judged once: the restart record is cleared here; a curve the
// animator got some other way since (other end or start) is left alone.
void damp_frame(uint8_t* cam, uint8_t* anim, uint32_t now, float& d) {
    const int mode = g_cfg.zoom_spring;
    const Restart r = g_last;
    g_last.valid = false;
    if (!r.valid || mode >= 3 || r.speed == 0.0f) return;
    if (std::fabs(*reinterpret_cast<const float*>(anim + ANIM_END) - r.end) > 0.5f ||
        std::fabs(poly8(segment(anim, 0), 0.0f) - r.start) > 0.5f)
        return;
    ++g_spring_frames;
    const int levels = *field<int>(cam, CAM_NUM_ZOOM_LEVELS);
    const float in = *field<float>(cam, CAM_ZOOM_IN_DIST), out = *field<float>(cam, CAM_ZOOM_OUT_DIST);
    const float step = levels > 0 ? std::fabs(out - in) / float(levels) : 1000.0f;
    const float allow = std::max(1.0f, kSpringAllow[mode] * step);
    const float before = overshoot(anim, r.start, r.end);
    if (before <= allow) return;
    const Build_t build = reinterpret_cast<Build_t>(FN_BUILD);
    float lo = 0.0f, hi = 1.0f;
    for (int i = 0; i < 12; ++i) {
        const float k = 0.5f * (lo + hi);
        build(r.builder, r.start, r.end, r.speed * k, r.extra);
        if (overshoot(anim, r.start, r.end) <= allow) lo = k;
        else hi = k;
    }
    build(r.builder, r.start, r.end, r.speed * lo, r.extra);
    d = curve_value(anim, now);
    *reinterpret_cast<float*>(anim + ANIM_VALUE) = d;
    ++g_spring_damped;
    if (g_trace > 0)
        RLOG(CH_UI, L_INFO, "camera zoom curve %.0f -> %.0f: speed %.0f would overshoot by %.0f (%.0f allowed), kept "
             "x%.3f -> %.0f", r.start, r.end, r.speed, before, allow, lo, overshoot(anim, r.start, r.end));
}

void __stdcall on_zoom_stored(uint8_t* anim, uint32_t now) {
    uint8_t* cam = static_cast<uint8_t*>(g_camera);
    if (!cam || anim != cam + ZOOM_ANIMATOR) return;
    __try {
        float& d = *field<float>(cam, CAM_ZOOM_DIST);
        damp_frame(cam, anim, now, d);
        const float lo = *field<float>(cam, CAM_LO), hi = *field<float>(cam, CAM_HI);
        if (g_hold_limits && g_prev_ok && lo < hi) {
            if (d > hi && g_prev_dist <= hi + 0.5f) {
                d = hi;
                ++g_held;
            } else if (d < lo && g_prev_dist >= lo - 0.5f) {
                d = lo;
                ++g_held;
            }
        }
        g_prev_dist = d;
        g_prev_ok = true;
        if (g_trace > 0)
            RLOG(CH_UI, L_INFO, "camera zoom evaluated: distance %.0f, aim %.0f", d, *field<float>(cam, CAM_ZOOM_AIM));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

__declspec(naked) void h_zoom_stored() {
    __asm {
        pushad
        sub esp, 128                   // the update keeps values in xmm registers across this point
        movups xmmword ptr [esp], xmm0
        movups xmmword ptr [esp + 16], xmm1
        movups xmmword ptr [esp + 32], xmm2
        movups xmmword ptr [esp + 48], xmm3
        movups xmmword ptr [esp + 64], xmm4
        movups xmmword ptr [esp + 80], xmm5
        movups xmmword ptr [esp + 96], xmm6
        movups xmmword ptr [esp + 112], xmm7
        push edi                       // the time of this frame
        push esi                       // the animator
        call on_zoom_stored
        movups xmm0, xmmword ptr [esp]
        movups xmm1, xmmword ptr [esp + 16]
        movups xmm2, xmmword ptr [esp + 32]
        movups xmm3, xmmword ptr [esp + 48]
        movups xmm4, xmmword ptr [esp + 64]
        movups xmm5, xmmword ptr [esp + 80]
        movups xmm6, xmmword ptr [esp + 96]
        movups xmm7, xmmword ptr [esp + 112]
        add esp, 128
        popad
        jmp dword ptr [g_zoom_tramp]
    }
}

// Every restart of an animation through 0x00EFC550 builds its curve here; the zoom's last one is kept.
void __stdcall on_build(const uint32_t* a, uint8_t* anim) {
    Restart r = {true, reinterpret_cast<void*>(a[0])};
    memcpy(&r.start, &a[1], 4);
    memcpy(&r.end, &a[2], 4);
    memcpy(&r.speed, &a[3], 4);
    memcpy(&r.extra, &a[4], 4);
    reinterpret_cast<Build_t>(FN_BUILD)(r.builder, r.start, r.end, r.speed, r.extra);
    uint8_t* cam = static_cast<uint8_t*>(g_camera);
    if (!cam || anim != cam + ZOOM_ANIMATOR) return;
    g_last = r;
    ++g_spring_restarts;
    if (g_trace > 0) {
        uint32_t c[5] = {};
        const int n = game_callers(a, c, 5);
        RLOG(CH_UI, L_INFO, "camera zoom restart %.0f -> %.0f, speed %.0f (callers %08x %08x %08x %08x %08x, %d)",
             r.start, r.end, r.speed, c[0], c[1], c[2], c[3], c[4], n);
    }
}

__declspec(naked) void h_build() {
    __asm {
        push esi                   // the animator (0x00EFC550 runs on esi)
        lea eax, [esp + 8]         // the five arguments of the builder
        push eax
        call on_build
        ret 0x14
    }
}

void install_zoom_motion() {
    g_hold_limits = config_int(L"camera", L"stop_at_limits", 1, true) != 0;
    unsigned char expect[5] = {0xE8, 0, 0, 0, 0};
    *reinterpret_cast<int32_t*>(expect + 1) = int32_t(FN_BUILD - (AT_BUILD_CALL + 5));
    unsigned char call[5] = {0xE8, 0, 0, 0, 0};
    *reinterpret_cast<int32_t*>(call + 1) = int32_t(reinterpret_cast<uintptr_t>(&h_build) - (AT_BUILD_CALL + 5));
    if (memcmp(reinterpret_cast<const void*>(AT_BUILD_CALL), expect, 5) != 0 ||
        memcmp(reinterpret_cast<const void*>(AT_ZOOM_STORED), kZoomSig, sizeof(kZoomSig)) != 0) {
        RLOG(CH_UI, L_WARN, "camera: zoom spring / limit hooks off (code differs)");
        return;
    }
    g_zoom_tramp = detour(AT_ZOOM_STORED, kZoomSig, sizeof(kZoomSig), (void*)h_zoom_stored);
    const bool ok = g_zoom_tramp && patch_code(AT_BUILD_CALL, expect, call, 5);
    RLOG(CH_UI, ok ? L_INFO : L_ERROR, "camera: zoom motion hooks %s (zoom_spring %d: 0 none, 1 weak, 2 medium, 3 as "
         "the game; stop at the zoom limits %s)", ok ? "installed" : "FAILED", g_cfg.zoom_spring,
         g_hold_limits ? "on" : "off");
}
}  // namespace

void camera_forget_zoom() {
    g_prev_ok = false;
    g_last.valid = false;
}

std::string camera_describe() {
    void* cam = g_camera;
    if (!cam) return "camera: none";
    float dist, aim, in, out, ext;
    int levels;
    if (!read_zoom(cam, &dist, &aim, &in, &out, &ext, &levels)) return "camera: unreadable";
    char b[280];
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "camera %p: zoom distance %.0f, aim %.0f (range %.0f - %.0f, extended %.0f, %d steps; %ld frames held "
                "at a limit; spring %d: %ld restarts, %ld curves cut in %ld frames)", cam, dist, aim, in, out, ext,
                levels, g_held, g_cfg.zoom_spring, g_spring_restarts, g_spring_damped, g_spring_frames);
    return b;
}

// Called by the options tab after the user applied new zoom values.
void camera_apply_now() {
    void* cam = g_camera;
    if (cam) apply(cam, "options applied");
    else RLOG(CH_UI, L_INFO, "camera: no game running, zoom applies when the next game starts");
}

// [debug] log_camera=1: the live camera's zoom state, a line in ui.log whenever it changes (at most one per frame);
// the restarts and the evaluation above log as well.
namespace {
struct ZoomState {
    float dist, aim, step_aim, lo, hi, tilt, ext_tilt;
    int state;
};
ZoomState g_state = {};

bool read_state(void* cam, ZoomState* s) {
    __try {
        s->dist = *field<float>(cam, CAM_ZOOM_DIST);
        s->aim = *field<float>(cam, CAM_ZOOM_AIM);
        s->step_aim = *field<float>(cam, CAM_STEP_AIM);
        s->lo = *field<float>(cam, CAM_LO);
        s->hi = *field<float>(cam, CAM_HI);
        s->tilt = *field<float>(cam, CAM_TILT);
        s->ext_tilt = *field<float>(cam, CAM_EXT_TILT);
        s->state = *field<int>(cam, CAM_STATE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

void camera_trace() {
    if (g_trace < 0) g_trace = config_int(L"debug", L"log_camera", 0, true) != 0;
    if (!g_trace) return;
    void* cam = g_camera;
    ZoomState s;
    if (!cam || !read_state(cam, &s) || memcmp(&s, &g_state, sizeof(s)) == 0) return;
    g_state = s;
    RLOG(CH_UI, L_INFO, "camera zoom: distance %.0f, aim %.0f, step aim %.0f (range %.0f - %.0f), tilt %.3f "
         "(extended zoom from %.3f), state %d, held at a limit %ld", s.dist, s.aim, s.step_aim, s.lo, s.hi, s.tilt,
         s.ext_tilt, s.state, g_held);
}

void camera_limits_install() { install_zoom_motion(); }

}  // namespace rol
