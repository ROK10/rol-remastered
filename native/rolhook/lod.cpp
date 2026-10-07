// Model level of detail. For every model the engine computes its size on screen in pixels (0x00EF61F0: screen
// width x radius / distance) and picks the LOD as the number of data\lod.xml thresholds lod_0..3_pixel_size
// (50 / 110 / 800 / 1200) that size reaches, capped at 3 = the full model (0x00E6EB40, compare at 0x00E6EC68).
// The thresholds live in the engine's LOD manager (static object 0x01464968, vtable 0x0122D838, ints at
// +0x30..+0x3C), loaded from lod.xml when the graphics system starts.
//
// [graphics] lod_thresholds scales them, in percent: 50 keeps models detailed twice as far, 25 four times,
// 0 = always the full model. Checked every frame, so a change from the "Mods" tab applies at once.
#include "rolhook.h"

#include <cmath>
#include <cstring>

namespace rol {
namespace {

constexpr uintptr_t LODMGR = 0x01464968;
constexpr uintptr_t VT_LODMGR = 0x0122D838;
constexpr int THRESHOLDS = 0x30;
// LODMgr::pick_lod — its first bytes are checked before the manager is touched
constexpr uintptr_t FN_PICK_LOD = 0x00E6EB40;
const unsigned char kPickSig[8] = {0x83, 0xEC, 0x14, 0x83, 0x3D, 0x88, 0x56, 0x25};

bool g_ok = false;
int g_orig[4] = {};
bool g_have_orig = false;
int g_written[4] = {};
bool g_have_written = false;

bool read4(int out[4]) {
    __try {
        uintptr_t vt = *reinterpret_cast<const volatile uintptr_t*>(LODMGR);
        if (vt != VT_LODMGR) return false;   // not constructed yet
        memcpy(out, reinterpret_cast<const void*>(LODMGR + THRESHOLDS), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write4(const int v[4]) {
    __try {
        memcpy(reinterpret_cast<void*>(LODMGR + THRESHOLDS), v, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void lod_install(HMODULE exe) {
    g_ok = reinterpret_cast<uintptr_t>(exe) == 0x00400000 &&
           !memcmp(reinterpret_cast<const void*>(FN_PICK_LOD), kPickSig, sizeof(kPickSig));
    if (!g_ok) RLOG(CH_CORE, L_WARN, "LOD thresholds: unknown legends.exe build, setting ignored");
    else if (g_cfg.lod_thresholds != 100)
        RLOG(CH_CORE, L_INFO, "LOD thresholds at %d%% of data\\lod.xml (applied at the first frame)", g_cfg.lod_thresholds);
}

// Called at every Present: keeps the manager's thresholds at the configured share of lod.xml's values.
void lod_tick() {
    if (!g_ok) return;
    int cur[4];
    if (!read4(cur)) return;
    if (!g_have_orig) {
        if (!cur[0] && !cur[1] && !cur[2] && !cur[3]) return;   // lod.xml not loaded yet
        memcpy(g_orig, cur, sizeof(g_orig));
        g_have_orig = true;
        RLOG(CH_CORE, L_INFO, "LOD thresholds from data\\lod.xml: %d / %d / %d / %d px", cur[0], cur[1], cur[2], cur[3]);
    } else if (g_have_written && memcmp(cur, g_written, sizeof(cur)) != 0 && memcmp(cur, g_orig, sizeof(cur)) != 0) {
        // set by someone else (the engine's console, a reload of an edited lod.xml): take them as the new base
        memcpy(g_orig, cur, sizeof(g_orig));
        RLOG(CH_CORE, L_INFO, "LOD thresholds changed by the engine: %d / %d / %d / %d px", cur[0], cur[1], cur[2], cur[3]);
    }
    // with the interface scale the engine's screen is s times smaller, and so is every model's size on it: the
    // thresholds shrink with it to pick the same detail as at the real resolution
    const int pct = g_cfg.lod_thresholds, ui = uiscale_percent();
    int want[4];
    for (int i = 0; i < 4; ++i)
        want[i] = pct == 100 && ui == 100 ? g_orig[i] : int(std::lround(g_orig[i] * (pct / 100.0) * (100.0 / ui)));
    if (!memcmp(cur, want, sizeof(cur))) return;
    if (write4(want)) {
        memcpy(g_written, want, sizeof(want));
        g_have_written = true;
        RLOG(CH_CORE, L_INFO, "LOD thresholds set to %d%%: %d / %d / %d / %d px", pct, want[0], want[1], want[2], want[3]);
    }
}

}  // namespace rol
