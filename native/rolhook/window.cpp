// Borderless fullscreen ([graphics] borderless=1): the engine's fullscreen mode runs as a borderless window over the
// whole monitor. In fullscreen the engine already gives its window a popup style (no frame) and the mode's size
// (display setup 0x00E5E190); rolhook asks D3D for a windowed device instead of exclusive fullscreen and keeps the
// window on the monitor. Alt-Tab then needs no mode switch, overlays and other monitors keep working. When the game
// asks to minimize itself on losing focus, that is ignored.
//
// The cursor belongs to the game only while its window is in front:
//  * rolhook clips it to the window then (the camera scrolls at the screen edges, a click on another monitor would
//    drop the game behind it) and lets it go as soon as the window is deactivated — from the window procedure and,
//    in case the game stops presenting frames in the background, from rolhook's background thread;
//  * the engine itself keeps the cursor in its window when there is more than one monitor (personal.ini
//    RestrictMouse=1, set up at 0x0046ECE1): every frame, right after BeginScene (0x00E50F86), it calls 0x00477F30,
//    which moves the cursor back inside unless the window is minimized or Scroll Lock is on (the engine's "free
//    mouse" switch, which also stops the edge scrolling). In exclusive fullscreen the game was minimized on losing
//    focus, which ended that; here the window stays up, so the callback is skipped while another window is in front.
#include "rolhook.h"

#include <d3d9.h>

namespace rol {
namespace {

bool g_on = false;
HWND g_hwnd = nullptr;
volatile LONG g_clipped = 0;
typedef BOOL(WINAPI* ShowWindow_t)(HWND, int);
ShowWindow_t o_ShowWindow = nullptr;
volatile LONG g_min_blocked = 0;
WNDPROC g_prev_proc = nullptr;
bool g_unicode = true;

// the engine's cursor restriction callback (legends.exe 1.0): cmp dword ptr [0x01471C40], 0
constexpr uintptr_t FN_RESTRICT = 0x00477F30;
const unsigned char kRestrictSig[7] = {0x83, 0x3D, 0x40, 0x1C, 0x47, 0x01, 0x00};
typedef int(__cdecl* Restrict_t)(void*, void*);
Restrict_t o_restrict = nullptr;
volatile LONG g_restrict_skipped = 0;

bool in_front() { return g_hwnd && GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd); }

void release(const char* why) {
    if (!InterlockedExchange(&g_clipped, 0)) return;
    ClipCursor(nullptr);
    RLOG(CH_D3D, L_DEBUG, "borderless: cursor released (%s)", why);
}

int __cdecl h_restrict(void* a, void* b) {
    if (g_hwnd && GetForegroundWindow() != g_hwnd) {
        InterlockedIncrement(&g_restrict_skipped);
        return 0;
    }
    return o_restrict(a, b);
}

LRESULT CALLBACK bl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS)
        release(m == WM_ACTIVATEAPP ? "WM_ACTIVATEAPP" : m == WM_ACTIVATE ? "WM_ACTIVATE" : "WM_KILLFOCUS");
    return g_unicode ? CallWindowProcW(g_prev_proc, h, m, w, l) : CallWindowProcA(g_prev_proc, h, m, w, l);
}

BOOL WINAPI h_ShowWindow(HWND h, int cmd) {
    if (g_on && h && h == g_hwnd &&
        (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE)) {
        if (InterlockedIncrement(&g_min_blocked) == 1)
            RLOG(CH_D3D, L_INFO, "borderless: the game's minimize on focus loss ignored (window stays behind others)");
        return TRUE;
    }
    return o_ShowWindow(h, cmd);
}

RECT monitor_of(HWND w) {
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY);
    if (m && GetMonitorInfoW(m, &mi)) return mi.rcMonitor;
    RECT r = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return r;
}

}  // namespace

void borderless_install(HMODULE exe) {
    g_on = g_cfg.borderless;
    if (!g_on) return;
    bool sw = patch_iat(exe, "USER32.dll", "ShowWindow", (void*)h_ShowWindow, (void**)&o_ShowWindow);
    if (reinterpret_cast<uintptr_t>(exe) == 0x00400000)
        o_restrict = reinterpret_cast<Restrict_t>(detour(FN_RESTRICT, kRestrictSig, 7, (void*)h_restrict));
    RLOG(CH_CORE, L_INFO, "borderless fullscreen ON (windowed D3D device over the monitor)%s; engine cursor restriction %s",
         sw ? "" : "; ShowWindow not hooked", o_restrict ? "only while the game is in front" : "NOT hooked");
}

// Before CreateDevice / Reset: fullscreen parameters become windowed ones (a copy; the engine keeps its own).
bool borderless_params(const D3DPRESENT_PARAMETERS* in, D3DPRESENT_PARAMETERS& out) {
    if (!g_on || !in || in->Windowed) return false;
    out = *in;
    out.Windowed = TRUE;
    out.FullScreen_RefreshRateInHz = 0;   // must be 0 for a windowed device
    return true;
}

// After CreateDevice / Reset: the frameless window covers the monitor (D3D stretches the backbuffer if the chosen
// resolution is smaller than the monitor); its window procedure is wrapped once to let the cursor go on deactivation.
void borderless_window(HWND w, UINT bb_w, UINT bb_h) {
    if (!g_on || !w) return;
    g_hwnd = w;
    RECT m = monitor_of(w);
    LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE);
    LONG_PTR want = (st & ~LONG_PTR(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE;
    if (st != want) SetWindowLongPtrW(w, GWL_STYLE, want);
    SetWindowPos(w, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    if (!g_prev_proc) {
        g_unicode = IsWindowUnicode(w) != FALSE;
        g_prev_proc = reinterpret_cast<WNDPROC>(g_unicode ? SetWindowLongPtrW(w, GWLP_WNDPROC, LONG_PTR(bl_wndproc))
                                                          : SetWindowLongPtrA(w, GWLP_WNDPROC, LONG_PTR(bl_wndproc)));
    }
    RLOG(CH_D3D, L_INFO, "borderless: window %p over the monitor %ld,%ld %ldx%ld, backbuffer %ux%u", (void*)w, m.left,
         m.top, m.right - m.left, m.bottom - m.top, bb_w, bb_h);
}

// At every Present: the cursor stays in the window while the game is in front.
void borderless_present() {
    if (!g_on || !g_hwnd) return;
    if (in_front()) {
        RECT r, cur;
        // Windows drops the clip on some focus changes: compare with the live one, not with what was set
        if (GetWindowRect(g_hwnd, &r) && GetClipCursor(&cur) && !EqualRect(&r, &cur)) {
            ClipCursor(&r);
            InterlockedExchange(&g_clipped, 1);
        }
    } else {
        release("not in front at Present");
    }
}

// From rolhook's background thread (every flush_ms): the game may stop presenting while it is behind other windows.
void borderless_tick() {
    if (g_on && g_clipped && !in_front()) release("not in front (background check)");
}

void borderless_summary() {
    if (!g_on) return;
    release("exit");
    RLOG(CH_D3D, L_INFO, "borderless: engine cursor restriction skipped %ld time(s) while another window was in front",
         (long)g_restrict_skipped);
}

}  // namespace rol
