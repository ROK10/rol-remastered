// Smooth animation ([graphics] anim_pose_ms, anim_clock): skeleton poses fresh on every frame, a game clock that runs
// evenly across the simulation's ticks, and the parts that move only on the ticks (buildings' turning heads) placed
// between them.
//
// Poses. Units' animation time runs in milliseconds, every frame, and the engine samples it between key frames (slerp,
// 0x00EA15D0 / 0x00F27770). But a unit's render update (0x00A69C40, and 0x00A69180 for the other path) first asks a
// pose cache shared by all units (0x00A6A0B0 / 0x00A696D0, keyed by animation and time): a cached pose of the same
// animation at most N ms older than the unit's time is used as it is, and only an older one makes the engine evaluate
// the skeleton again. N is 66 ms when [0x013BB11A] > 65 (in battles it is) and [0x0126E850] / [0x0126E854] (20 ms)
// otherwise - so at 175 frames a second the limbs moved in 66 ms steps, ~16 a second, all units with the same
// animation on the same frame, while the bodies glide every frame (motion probe, autotest motion). A smaller N
// samples the skeleton again on every frame its time moved by N ms or more; 1 keeps units with the same animation at
// the same time sharing one pose. 0 leaves the game's values. Only the picture changes: the poses are not the
// simulation's.
//
// Clock. The animation states are made anew every frame (a pool, 0x00E7E110) and take their time from the game's tick
// count and the part of the next tick that has passed: units 67 ms x their ticks + [world + 0x90] (0x0070EC69), the
// game world's clock ([0x013E50AC] + 0x38: 66.67 ms x the ticks done + [world + 0x90], 0x006D9B00, never backwards)
// for others; the fraction beside it ([world + 0x94]) places what moves between two ticks. 0x006D9BA0 works both out
// once a frame, after the clocks have taken theirs: 66.67 x elapsed / period from the time the tick timer has left
// (0x00C738D0). When a tick runs, the engine sets the part back to 0 and has 0x006D9BA0 skip the next frame
// ([world + 0x70] bit 2): the clocks take the tick's start on the tick's frame and again on the next, then leap two
// frames' worth (autotest clock_smooth W lines: 7190, 7200, 7200, 7214 ms at 7.3 ms frames, ticks 107 -> 108, the part
// 65, 65, 14) - every animation stood still for a frame and leapt 15 times a second, an Alin mine's sails most
// visibly (autotest bld_time: the sails' vertices moved 1/7 of a step, then 2 steps, every ~10 frames). anim_clock
// makes the skip clear the flag and work the part out as on any frame (0x006D9D2C: pop ebx; add esp, 0x10; ret ->
// jmp 0x006D9BB1), so the frame after a tick gets the time that has passed since it; a tick that comes late (the
// timer's time left stops at 0; caught signed at 0x00C73908 for the call from 0x006D9C8E) lets the part run on past
// the tick's end by as much (up to 50 ms) instead of standing at it (0x006D9D1C); and the world clock (0x006D9B26)
// takes the time the last part was worked out at - its ticks x 66.67 + the part as the timer measured it, short of
// the tick's moment when the tick came early - instead of the tick's start on the tick's frame and the part held at
// 1 after an early tick. The part itself is worked out from the timer's time to a fraction of a ms (its whole-ms "now"
// set against the real clock) and rounded only then, so the clocks step evenly with the frames' real time: the whole-ms
// timer made steps of 5 and 7 ms where 6 were due. Only the picture's timing changes: the simulation counts its ticks
// as before (autotest clock_smooth: the units' animation times +6..+8 ms at ~7 ms frames across the ticks, the mill's
// sails even). On with smooth animation (anim_pose_ms > 0) unless [graphics] anim_clock=0.
//
// The Mods tab switches all of it at once (anim_apply): the code and both variables are written in place.
#include "rolhook.h"

#include <cmath>
#include <cstring>
#include <iterator>
#include <unordered_map>

namespace rol {
namespace {

struct Site {
    uintptr_t at;          // cmp dword ptr [0x013BB11A], 0x41; mov eax, 0x42
    uintptr_t variable;    // the tolerance used when the cmp says no
    const char* what;
};
const Site kSites[] = {
    {0x00A6A151, 0x0126E850, "unit pose cache (0x00A6A0B0)"},
    {0x00A6975A, 0x0126E854, "pose cache of the other render path (0x00A696D0)"},
};
const unsigned char kSig[12] = {0x83, 0x3D, 0x1A, 0xB1, 0x3B, 0x01, 0x41,   // cmp dword ptr [0x13bb11a], 0x41
                                0xB8, 0x42, 0x00, 0x00, 0x00};              // mov eax, 0x42
bool g_ok = false;                // both sites found as above at the start
int g_var_orig[2] = {20, 20};     // the variables as the game set them
int g_now = 0;                    // what is in place (0 = the game's)

bool write_int(uintptr_t a, int v) {
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(a), 4, PAGE_READWRITE, &old)) return false;
    *reinterpret_cast<volatile int*>(a) = v;
    VirtualProtect(reinterpret_cast<void*>(a), 4, old, &old);
    return true;
}

// ---------------------------------------------------------------- the tick's elapsed part
// 0x00C73908 in the timer's time-left function (ecx = the next tick's moment, eax = now): sub ecx, eax; mov eax, 0
const unsigned char kLeftSig[7] = {0x2B, 0xC8, 0xB8, 0x00, 0x00, 0x00, 0x00};
// 0x006D9D1C, the end of 0x006D9BA0 (ebx = the world, eax = the part): mov [ebx + 0x90], eax
const unsigned char kPartSig[6] = {0x89, 0x83, 0x90, 0x00, 0x00, 0x00};
constexpr uint32_t kPartCallRet = 0x006D9C93;   // after 0x006D9BA0's call of the time-left function
constexpr int kLateMax = 50;                    // ms a late tick's part may run on
// 0x006D9D2C, the end of 0x006D9BA0's skip (after it cleared the flag): pop ebx; add esp, 0x10; ret
const unsigned char kSkipSig[5] = {0x5B, 0x83, 0xC4, 0x10, 0xC3};
const unsigned char kSkipNew[5] = {0xE9, 0x80, 0xFE, 0xFF, 0xFF};   // jmp 0x006D9BB1: work the part out
bool g_skip_ok = false;
long g_ticks_seen = 0;   // ticks after which the part was worked out (anim_clock on)
int g_last_ticks = -1;
// The world clock (0x006D9B00, before 0x006D9BA0 in the frame): the time of the last part worked out - its ticks
// x 66.67 + the part as the timer measured it, signed (before the tick's moment when the tick came early)
// 0x006D9B26 (edi = the world, esi = 66.67 x the ticks done): add esi, [edi + 0x90]
const unsigned char kClockSig[6] = {0x03, 0xB7, 0x90, 0x00, 0x00, 0x00};
void* g_clock_tramp = nullptr;
bool g_comp = false;        // a part was worked out since the clocks went on
int g_comp_ticks = 0;
double g_comp_part = 0;     // ms, 66.67 x (period - time left) / period
double g_comp_real = 0;     // the real time (now_ms) it was worked out at
double g_comp_rate = 1.0;   // world ms per real ms: 66.67 / the game speed's tick length
long g_clock_fixed = 0;     // clock updates that differed from the engine's sum
void* g_left_tramp = nullptr;
void* g_part_tramp = nullptr;
volatile bool g_clock_on = false;
int g_left = 0;              // the time left the call from 0x006D9BA0 computed, signed (ms)
int g_left_now = 0;          // the timer's "now" it was taken from (whole ms)
double g_left_real = 0;      // the real time (now_ms) then
bool g_left_fresh = false;
// The timer counts whole ms (timeGetTime): its "now" lags the true time by the part of a ms gone. Its offset from the
// real clock - the most "now" has been ahead of now_ms lately (a "now" read just as the ms turned) - gives the true
// time to a fraction of a ms; the largest seen fades by 0.1 ms a second to follow drift.
double g_tm_off = 0;
bool g_tm_have = false;
int g_last_engine = 0, g_last_given = 0, g_last_left = 0;
long g_parts = 0, g_late = 0, g_late_capped = 0;
int g_late_max = 0;

void __stdcall on_left(const uint32_t* r) {   // the pushad block: edi, esi, ebp, esp, ebx, edx, ecx, eax
    // the function's stack at the site: [0] its saved ecx, [1] the return address (the pushfd above the block: + 4)
    const uint32_t ret = *reinterpret_cast<const uint32_t*>(uintptr_t(r[3]) + 8);
    if (ret != kPartCallRet) return;
    g_left = int(r[6] - r[7]);
    g_left_now = int(r[7]);
    g_left_real = now_ms();
    g_left_fresh = true;
}

__declspec(naked) void h_left() {
    __asm {
        pushfd
        pushad
        push esp
        call on_left
        popad
        popfd
        jmp dword ptr [g_left_tramp]
    }
}

int __stdcall tick_part(int engine) {
    const bool fresh = g_left_fresh;
    g_left_fresh = false;
    const int ticks = *reinterpret_cast<const int*>(0x012CF5A8);   // the ticks done
    if (ticks != g_last_ticks) {
        if (g_clock_on && g_last_ticks >= 0) g_ticks_seen += ticks - g_last_ticks;
        g_last_ticks = ticks;
    }
    int out = engine;
    // the period 0x006D9BA0 scaled by (the game speed's tick length, ms)
    const int period = *reinterpret_cast<const int*>(0x010E7920 + 4 * *reinterpret_cast<const uint8_t*>(0x013BB03F));
    if (fresh && period > 0) {
        // the time left to a fraction of a ms: the timer's "now" lags the true time by the part of a ms gone
        const double off = g_left_now - g_left_real;
        if (g_tm_have) g_tm_off -= (g_left_real - g_comp_real) * 1e-4;
        if (!g_tm_have || off > g_tm_off || off < g_tm_off - 2.0) g_tm_off = off;   // 2 ms off: the timer jumped
        g_tm_have = true;
        double gone = (g_left_real + g_tm_off) - g_left_now;   // of the current ms
        if (gone < 0) gone = 0;
        if (gone > 1) gone = 1;
        double left = g_left - gone;
        if (left < -kLateMax) {
            left = -kLateMax;
            ++g_late_capped;
        }
        if (g_left < 0) {   // a late tick: the part runs on past the tick's end
            ++g_late;
            if (-g_left > g_late_max) g_late_max = -g_left;
        }
        g_comp_part = (period - left) * 200.0 / (3.0 * period);
        g_comp_ticks = ticks;
        g_comp_real = g_left_real;
        g_comp_rate = 200.0 / (3.0 * period);
        g_comp = true;
        // the clocks' part: the engine rounds a whole-ms time and stands at the tick's end when the tick is late;
        // given: that time to a fraction of a ms, rounded - even steps of the frames' real time - and on past the end
        if (g_clock_on) {
            const int v = int(g_comp_part + 0.5);
            out = v > 1 ? v : 1;   // the engine's least, 1, after a tick that came early
        }
    }
    ++g_parts;
    g_last_left = fresh ? g_left : 0x7FFFFFFF;
    g_last_engine = engine;
    g_last_given = out;
    return out;
}

// The world clock's value for the frame: the engine has esi + [edi + 0x90]; esi leaves as the value minus
// [edi + 0x90], which the moved instruction adds back.
int __stdcall world_clock(int ticks_ms, int part) {
    const int engine = ticks_ms + part;
    if (!g_clock_on || !g_comp) return engine;
    const int v = int(g_comp_ticks * (200.0 / 3.0) + g_comp_part + (g_comp_ticks >= 0 ? 0.5 : -0.5));
    // a value far from the engine's (a new game, a load) is not ours to change
    if (v > engine + 100 || v < engine - 100) return engine;
    if (v > engine + 1 || v < engine - 1) ++g_clock_fixed;
    return v;
}

__declspec(naked) void h_clock() {
    __asm {
        pushad
        push dword ptr [edi + 0x90]
        push esi
        call world_clock
        sub eax, [edi + 0x90]
        mov [esp + 4], eax   // the pushad block's esi
        popad
        jmp dword ptr [g_clock_tramp]
    }
}

__declspec(naked) void h_part() {
    __asm {
        pushad
        push eax
        call tick_part
        mov [esp + 28], eax   // the pushad block's eax
        popad
        jmp dword ptr [g_part_tramp]
    }
}

// ---------------------------------------------------------------- parts that move on the ticks
// 0x00A6AD5E in 0x00A6AD30 (a model's render update): call 0x00E71AF0 with ecx = the game object's transform (+8 of
// it: a rotation quaternion, the translation, then 5 more floats) and eax = the render object it is copied into (and
// on to the objects chained to it). The turning heads of buildings - an Alin mine's (alim_mill_rotate_pfo.bha, 18
// degrees a key every 0.33 s) - get a new transform only when a tick runs: 3.6 degrees and 13.7 world units at once,
// 15 times a second, standing still between (autotest xform: changed on 44 of 522 frames, 44 ticks); units, placed
// between the ticks by the engine, move every frame. anim_clock hands such a render object the transform between its
// last two - the one before the last tick and the last - by the part of a tick the world clock has gone past the last
// one's tick: one tick behind, as the engine places moving units. Left alone: objects that change on frames in a row
// (moved by something else), a step of more than 60 degrees or 300 world units (a jump), a rotation that is not a
// unit quaternion.
constexpr uintptr_t kCopyFn = 0x00E71AF0;
constexpr uintptr_t kCopySite = 0x00A6AD5E;
uintptr_t g_copy_next = 0;   // where the call went before (0x00E71AF0, or another hook that goes on to it)
struct Moved {
    float q0[4], t0[3];   // the transform before the last change
    float q1[4], t1[3];   // the last one
    int tick1;            // the ticks done when it came
    long frame1;          // and the frame
    long seen;            // the last frame the object was copied on
    int mover;            // > 0: changed twice within a tick lately - moved by something else, left alone
    bool have0;
    float out[12];        // this frame's transform handed on
    long out_frame;
};
std::unordered_map<uintptr_t, Moved> g_moved;   // per render object
long g_moved_frames = 0, g_moved_eased = 0;     // copies seen (objects x frames), copies handed between two
long g_prune_frame = 0;

// The world's time this frame to a fraction of a millisecond, for placing between the ticks: the last part worked out
// (the timer counts whole ms) carried on by the real time since, run evenly - the frames' real intervals - and pulled
// a fifth of the way to that each frame; taken as it is when 10 ms or more apart (a jump, a pause). Once a frame.
double g_st = 0, g_st_real = 0;
long g_st_frame = -1;
bool g_st_have = false;

double smooth_world_time(long frame) {
    if (frame == g_st_frame) return g_st;
    const double real = now_ms();
    const double target = g_comp_ticks * (200.0 / 3.0) + g_comp_part + (real - g_comp_real) * g_comp_rate;
    if (!g_st_have) {
        g_st = target;
        g_st_have = true;
    } else {
        const double pred = g_st + (real - g_st_real) * g_comp_rate;
        g_st = pred + (target - pred) * 0.2;
        if (g_st > target + 10.0 || g_st < target - 10.0) g_st = target;
    }
    g_st_real = real;
    g_st_frame = frame;
    return g_st;
}

bool unit_quat(const float* q) {
    const float l = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return l > 0.98f && l < 1.02f;
}

const float* __stdcall copy_source(const float* src, uintptr_t robj) {
    if (!g_clock_on || !g_comp) return src;
    const long frame = g_frame;
    if (frame - g_prune_frame > 600) {   // render objects gone for a while
        g_prune_frame = frame;
        for (auto it = g_moved.begin(); it != g_moved.end();)
            it = frame - it->second.seen > 600 ? g_moved.erase(it) : std::next(it);
    }
    auto ins = g_moved.try_emplace(robj);
    Moved& m = ins.first->second;
    const int ticks = *reinterpret_cast<const int*>(0x012CF5A8);
    if (ins.second || frame - m.seen > 30) {   // new, or back after a while: take it as it is
        memcpy(m.q1, src, 16);
        memcpy(m.t1, src + 4, 12);
        m.tick1 = ticks;
        m.frame1 = frame;
        m.mover = 0;
        m.have0 = false;
        m.seen = frame;
        m.out_frame = -1;
        return src;
    }
    m.seen = frame;
    if (m.out_frame == frame) {   // copied again this frame (another pass): the same transform
        memcpy(m.out + 7, src + 7, 20);
        return m.out;
    }
    ++g_moved_frames;
    if (memcmp(m.q1, src, 16) != 0 || memcmp(m.t1, src + 4, 12) != 0) {
        m.mover = ticks == m.tick1 ? 8 : (m.mover > 0 ? m.mover - 1 : 0);   // two changes in one tick
        memcpy(m.q0, m.q1, 16);
        memcpy(m.t0, m.t1, 12);
        memcpy(m.q1, src, 16);
        memcpy(m.t1, src + 4, 12);
        m.tick1 = ticks;
        m.frame1 = frame;
        m.have0 = true;
    }
    if (!m.have0 || m.mover > 0 || !unit_quat(m.q0) || !unit_quat(m.q1)) return src;
    float dot = m.q0[0] * m.q1[0] + m.q0[1] * m.q1[1] + m.q0[2] * m.q1[2] + m.q0[3] * m.q1[3];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    dot *= sign;
    const float dx = m.t1[0] - m.t0[0], dy = m.t1[1] - m.t0[1], dz = m.t1[2] - m.t0[2];
    if (dot < 0.866f || dx * dx + dy * dy + dz * dz > 300.0f * 300.0f) return src;   // 60 degrees (cos 30), a jump
    // the world's time now, to a fraction of a ms, past the last change's tick, in ticks
    double f = (smooth_world_time(frame) - m.tick1 * (200.0 / 3.0)) / (200.0 / 3.0);
    if (f >= 1.0) return src;
    if (f < 0.0) f = 0.0;
    const float a = float(1.0 - f), b = float(f) * sign;
    float q[4], len = 0;
    for (int i = 0; i < 4; ++i) {
        q[i] = a * m.q0[i] + b * m.q1[i];
        len += q[i] * q[i];
    }
    len = len > 0 ? 1.0f / std::sqrt(len) : 1.0f;
    for (int i = 0; i < 4; ++i) m.out[i] = q[i] * len;
    for (int i = 0; i < 3; ++i) m.out[4 + i] = float(m.t0[i] + (m.t1[i] - m.t0[i]) * f);
    memcpy(m.out + 7, src + 7, 20);
    m.out_frame = frame;
    ++g_moved_eased;
    return m.out;
}

// At 0x00A6AD5E (ecx = the transform, eax = the render object): ecx leaves as the transform to copy.
__declspec(naked) void h_copy() {
    __asm {
        pushad
        push eax
        push ecx
        call copy_source
        mov [esp + 24], eax   // the pushad block's ecx
        popad
        jmp dword ptr [g_copy_next]
    }
}

void clock_switch(bool on) {
    if (!g_part_tramp || on == g_clock_on) return;
    if (g_skip_ok) patch_code(0x006D9D2C, nullptr, on ? kSkipNew : kSkipSig, 5);
    g_comp = false;   // the next part worked out is the first the world clock takes
    g_moved.clear();
    g_st_have = false;
    g_clock_on = on;
    RLOG(CH_CORE, L_INFO, "smooth animation: game clocks %s", on ? "go on through the ticks (the frame after a tick "
         "worked out, a late tick run on, turning parts of buildings between the ticks)" : "as the engine runs them");
}

}  // namespace

void anim_apply(int ms) {
    ms = ms < 0 ? 0 : (ms > 255 ? 255 : ms);
    clock_switch(ms > 0 && g_cfg.anim_clock);
    if (!g_ok || ms == g_now) return;
    for (int i = 0; i < 2; ++i) {
        unsigned char b[12];
        memcpy(b, kSig, sizeof(b));
        b[8] = static_cast<unsigned char>(ms ? ms : 0x42);   // mov eax, <ms> (the game: 0x42 = 66)
        patch_code(kSites[i].at, nullptr, b, sizeof(b));     // the first 8 bytes were checked at the start
        write_int(kSites[i].variable, ms ? ms : g_var_orig[i]);
    }
    RLOG(CH_CORE, L_INFO, "smooth animation: %s", ms ? "on" : "off (the game's 66 ms)");
    if (ms) RLOG(CH_CORE, L_INFO, "smooth animation: cached skeleton poses at most %d ms old (the game: 66)", ms);
    g_now = ms;
}

void anim_install(HMODULE exe) {
    if (reinterpret_cast<uintptr_t>(exe) != 0x00400000) return;
    int found = 0;
    for (int i = 0; i < 2; ++i) {
        if (memcmp(reinterpret_cast<const void*>(kSites[i].at), kSig, sizeof(kSig)) != 0) {
            RLOG(CH_CORE, L_WARN, "smooth animation: %s - code differs, off", kSites[i].what);
            continue;
        }
        g_var_orig[i] = *reinterpret_cast<const int*>(kSites[i].variable);
        ++found;
    }
    g_ok = found == 2;
    g_skip_ok = memcmp(reinterpret_cast<const void*>(0x006D9D2C), kSkipSig, sizeof(kSkipSig)) == 0 &&
                *reinterpret_cast<const uint16_t*>(0x006D9D27) == 0xFD24;   // and al, 0xfd before it
    if (g_skip_ok) g_left_tramp = detour(0x00C73908, kLeftSig, sizeof(kLeftSig), (void*)h_left);
    if (g_left_tramp) g_clock_tramp = detour(0x006D9B26, kClockSig, sizeof(kClockSig), (void*)h_clock);
    if (g_clock_tramp) g_part_tramp = detour(0x006D9D1C, kPartSig, sizeof(kPartSig), (void*)h_part);
    if (g_part_tramp) {   // the call that copies a model's transform: to h_copy, which goes on where it went
        const uint8_t* p = reinterpret_cast<const uint8_t*>(kCopySite);
        const int32_t rel = *reinterpret_cast<const int32_t*>(p + 1);
        const uintptr_t target = kCopySite + 5 + uintptr_t(rel);
        const uint8_t* fn = reinterpret_cast<const uint8_t*>(kCopyFn);
        if (p[0] == 0xE8 && fn[0] == 0x56 && fn[1] == 0x57 && fn[2] == 0x8B && fn[3] == 0xF1) {   // push esi; push edi; mov esi, ecx
            g_copy_next = target;
            const int32_t now = int32_t(reinterpret_cast<uintptr_t>(&h_copy) - (kCopySite + 5));
            if (!patch_code(kCopySite + 1, &rel, &now, 4)) g_copy_next = 0;
        }
        if (!g_copy_next)
            RLOG(CH_CORE, L_WARN, "smooth animation: the transform copy at 0x00A6AD5E differs - turning parts as the engine's");
    }
    if (!g_part_tramp)
        RLOG(CH_CORE, L_WARN, "smooth animation: the tick timer's code (0x006D9D2C / 0x00C73908 / 0x006D9B26 / "
             "0x006D9D1C) differs - game clocks as the engine's");
    if (g_cfg.anim_pose_ms > 0) anim_apply(g_cfg.anim_pose_ms);
}

bool anim_clock_values(int* engine, int* given, int* left) {
    if (!g_part_tramp || !g_parts) return false;
    *engine = g_last_engine;
    *given = g_last_given;
    *left = g_last_left;
    return true;
}

void anim_summary() {
    if (!g_parts) return;
    RLOG(CH_CORE, L_INFO, "smooth animation: the clocks' part worked out on %ld frames, %ld ticks went by with it; a tick "
         "was late on %ld frames (%.1f%%), the clocks ran on (late by at most %d ms, %ld times past the %d ms limit); "
         "the world clock took the timer's own time on %ld updates; models' transforms copied %ld times, %ld of them "
         "handed between two ticks' (turning parts)", g_parts, g_ticks_seen, g_late, 100.0 * g_late / g_parts,
         g_late_max, g_late_capped, kLateMax, g_clock_fixed, g_moved_frames, g_moved_eased);
}

}  // namespace rol
