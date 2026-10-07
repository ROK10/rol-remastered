// Screenshots and the automated test run.
//
// [debug] shot_key: the frame on screen when the key goes down is saved as <session>\shot_<frame>.png.
// [test] is set by the ROLHOOK_EXTRA_INI layer of modkit\tools\autotest.py, never by the everyday profile:
//   start=match | first_frame   the test clock starts when the match is on screen (the first frame that draws a
//                               terrain tile texture, art\terrain\...) or at the first frame (menus, smoke test)
//   shots=6, 12                 screenshots at these seconds of the test clock -> shot_<n>_<seconds>s.png
//   capture=1                   with each shot, a frame capture (capture.cpp) of the same frame
//   wheel=8:6                   mouse wheel notches posted to the game window at a second of the clock (+ = away
//                               from the user, the camera zooms in), for close-up shots
//   keys=3:Left:250             a key held from a second of the clock for so many ms (Left / Right / Up / Down pan
//                               the camera; or a virtual-key code), sent as real keyboard input by scan code
//                               (SendInput) and only while the game's window is in front, so it never reaches
//                               another program; let go on time, when the window loses the front, at the end
//   options=6                   F11 posted at this second: the game opens its options window ...
//   options_tab=Mods            ... and rolhook shows this tab of it (ui.cpp presses its button), for shots of it
//   quit=14                     the game is ended at this second of the test clock, once the shots are written
//   timeout=240                 ... or this many seconds after the process started when the clock never started
//                               (stuck in a menu or a message box): a shot of the screen first, then the end
// The game thread only copies the backbuffer; the PNG files are encoded through WIC on a worker thread.
#include "rolhook.h"

#include <d3d9.h>
#include <wincodec.h>

#include <algorithm>
#include <deque>
#include <unordered_set>

namespace rol {

bool g_shots_watch = false;

namespace {

struct Job {
    std::wstring path;
    UINT w = 0, h = 0;
    std::vector<uint8_t> bgr;   // 24-bit rows, top-down
};

CRITICAL_SECTION g_qcs;
std::deque<Job> g_queue;
HANDLE g_qevent = nullptr;
HANDLE g_worker = nullptr;
volatile LONG g_pending = 0;   // queued or being written
volatile LONG g_written = 0, g_failed = 0;

SRWLOCK g_tlock = SRWLOCK_INIT;
std::unordered_set<void*> g_terrain;   // terrain tile textures created before the match start was seen

bool g_test = false;          // [test] shots / quit / timeout configured
bool g_started = false;       // the test clock runs
double g_t0 = 0;              // its start (now_ms)
long g_start_frame = 0;
size_t g_next = 0;            // next shot of [test] shots
size_t g_next_wheel = 0;      // next [test] wheel event
size_t g_next_key = 0;        // next [test] keys event
struct Held {
    int vk;
    double until;   // second of the test clock
};
std::vector<Held> g_held;     // [test] keys down now
bool g_options_sent = false;  // [test] options: F11 posted
bool g_wait_capture = false;  // that shot waits for the frame capture armed at an earlier Present
long g_capture_asked = 0;     // ... at this frame
bool g_key_down = false;
volatile LONG g_quitting = 0;

bool write_png(const Job& j, HRESULT& out) {
    IWICImagingFactory* fac = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    if (SUCCEEDED(hr)) hr = fac->CreateStream(&st);
    if (SUCCEEDED(hr)) hr = st->InitializeFromFilename(j.path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(j.w, j.h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&pf);
    if (SUCCEEDED(hr) && pf != GUID_WICPixelFormat24bppBGR) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    if (SUCCEEDED(hr))
        hr = fr->WritePixels(j.h, j.w * 3, UINT(j.bgr.size()), const_cast<BYTE*>(j.bgr.data()));
    if (SUCCEEDED(hr)) hr = fr->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (fac) fac->Release();
    out = hr;
    return SUCCEEDED(hr);
}

DWORD WINAPI worker(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // rolhook's own thread: no apartment of the game's to clash with
    for (;;) {
        WaitForSingleObject(g_qevent, INFINITE);
        for (;;) {
            Job j;
            EnterCriticalSection(&g_qcs);
            bool any = !g_queue.empty();
            if (any) {
                j = std::move(g_queue.front());
                g_queue.pop_front();
            }
            LeaveCriticalSection(&g_qcs);
            if (!any) break;
            double t = now_ms();
            HRESULT hr = S_OK;
            if (write_png(j, hr)) {
                InterlockedIncrement(&g_written);
                RLOG(CH_CORE, L_INFO, "shot written: %s (%ux%u, %.0f ms)", rel_path(j.path.c_str()).c_str(), j.w, j.h,
                     now_ms() - t);
            } else {
                InterlockedIncrement(&g_failed);
                RLOG(CH_CORE, L_ERROR, "shot FAILED hr=0x%08lx: %s", (unsigned long)hr, rel_path(j.path.c_str()).c_str());
            }
            InterlockedDecrement(&g_pending);
        }
    }
}

void enqueue(Job&& j) {
    if (!g_worker) {
        InitializeCriticalSection(&g_qcs);
        g_qevent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_worker = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    }
    InterlockedIncrement(&g_pending);
    EnterCriticalSection(&g_qcs);
    g_queue.push_back(std::move(j));
    LeaveCriticalSection(&g_qcs);
    SetEvent(g_qevent);
}

// The finished frame in the backbuffer -> 24-bit rows. A multisampled backbuffer is resolved first.
bool grab(IDirect3DDevice9* d, Job& j) {
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
    HRESULT hr = d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    D3DSURFACE_DESC sd = {};
    if (SUCCEEDED(hr)) bb->GetDesc(&sd);
    if (SUCCEEDED(hr) && sd.Format != D3DFMT_X8R8G8B8 && sd.Format != D3DFMT_A8R8G8B8) hr = E_NOTIMPL;
    IDirect3DSurface9* src = bb;
    if (SUCCEEDED(hr) && sd.MultiSampleType != D3DMULTISAMPLE_NONE) {
        hr = d->CreateRenderTarget(sd.Width, sd.Height, sd.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
        if (SUCCEEDED(hr)) hr = d->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE);
        src = rt;
    }
    if (SUCCEEDED(hr)) hr = d->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = d->GetRenderTargetData(src, sys);
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        j.w = sd.Width;
        j.h = sd.Height;
        j.bgr.resize(size_t(j.w) * j.h * 3);
        for (UINT y = 0; y < j.h; ++y) {
            const uint8_t* s = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
            uint8_t* o = &j.bgr[size_t(y) * j.w * 3];
            for (UINT x = 0; x < j.w; ++x, s += 4, o += 3) {
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
            }
        }
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    if (rt) rt->Release();
    if (bb) bb->Release();
    if (FAILED(hr))
        RLOG(CH_CORE, L_ERROR, "shot: backbuffer copy FAILED hr=0x%08lx (%ux%u fmt %d msaa %d)", (unsigned long)hr,
             sd.Width, sd.Height, int(sd.Format), int(sd.MultiSampleType));
    return SUCCEEDED(hr);
}

void shoot(IDirect3DDevice9* d, const wchar_t* name) {
    Job j;
    if (!grab(d, j)) return;
    j.path = g_cfg.session_dir + name;
    enqueue(std::move(j));
}

// As a scan code: the game takes the camera keys from the keyboard's scan codes (0.9.22 sent virtual-key codes and
// the camera never moved; DirectInput-style readers see only scan-code input). The arrows are E0-prefixed codes.
void send_key(int vk, bool up) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = WORD(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC));
    const bool extended = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR ||
                          vk == VK_NEXT || vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0u) | (extended ? KEYEVENTF_EXTENDEDKEY : 0u);
    SendInput(1, &in, sizeof(in));
}

// Every [test] key still down goes up (on time, the window left the front, the end of the run).
void release_keys(double t, const char* why) {
    for (const Held& h : g_held) {
        send_key(h.vk, true);
        RLOG(CH_CORE, L_INFO, "test: key 0x%02X up at %.2f s of the test clock (%s)", unsigned(h.vk), t, why);
    }
    g_held.clear();
}

// Ends the game for the test run: the queued PNGs are written, the summaries go to the logs, the process is
// terminated (no engine shutdown: it cannot hang, crash at exit or write the profile).
void quit(const char* why) {
    if (InterlockedExchange(&g_quitting, 1)) return;
    release_keys(g_started ? (now_ms() - g_t0) / 1000.0 : -1.0, "the end of the run");
    RLOG(CH_CORE, L_INFO, "test: ending the game (%s)", why);
    for (int i = 0; i < 300 && g_pending > 0; ++i) Sleep(100);
    exit_summaries(why);
    TerminateProcess(GetCurrentProcess(), 0);
}

}  // namespace

void shots_install(HMODULE) {
    g_test = !g_cfg.test_shots.empty() || g_cfg.test_quit > 0 || g_cfg.test_timeout > 0;
    g_shots_watch = g_test && !g_cfg.test_start_first_frame;
    if (!g_cfg.extra_ini.empty())
        RLOG(CH_CORE, L_INFO, "extra config layer (ROLHOOK_EXTRA_INI): %s", rel_path(g_cfg.extra_ini.c_str()).c_str());
    if (g_test) {
        std::string list;
        for (double s : g_cfg.test_shots) {
            char b[24];
            _snprintf_s(b, sizeof(b), _TRUNCATE, "%s%g", list.empty() ? "" : ", ", s);
            list += b;
        }
        RLOG(CH_CORE, L_INFO, "TEST RUN: clock from %s | shots at %s s%s | quit at %g s | timeout %g s",
             g_cfg.test_start_first_frame ? "the first frame" : "the match start (first terrain draw)",
             list.empty() ? "-" : list.c_str(), g_cfg.test_capture ? " with frame captures" : "", g_cfg.test_quit,
             g_cfg.test_timeout);
    }
    if (g_cfg.shot_vk) RLOG(CH_CORE, L_INFO, "screenshot key 0x%02x -> <session>\\shot_<frame>.png", g_cfg.shot_vk);
}

void shots_terrain_texture(void* tex) {
    if (!g_shots_watch || !tex) return;
    AcquireSRWLockExclusive(&g_tlock);
    g_terrain.insert(tex);
    ReleaseSRWLockExclusive(&g_tlock);
}

void shots_before_draw(IDirect3DDevice9* d) {
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(d->GetTexture(0, &t)) || !t) return;
    AcquireSRWLockShared(&g_tlock);
    bool terrain = g_terrain.count(t) != 0;
    ReleaseSRWLockShared(&g_tlock);
    t->Release();
    if (!terrain) return;
    g_shots_watch = false;
    g_started = true;
    g_t0 = now_ms();
    g_start_frame = g_frame;
    RLOG(CH_CORE, L_INFO, "test: match on screen (first terrain draw) at %.1f s, frame %ld - test clock starts",
         g_t0 / 1000.0, (long)g_frame);
}

void shots_on_present(IDirect3DDevice9* d) {
    if (g_cfg.shot_vk) {
        bool down = (GetAsyncKeyState(g_cfg.shot_vk) & 0x8000) != 0;
        if (down && !g_key_down) {
            wchar_t name[48];
            swprintf_s(name, L"shot_%ld.png", (long)g_frame);
            shoot(d, name);
        }
        g_key_down = down;
    }
    if (!g_test || g_quitting) return;
    if (!g_started && g_cfg.test_start_first_frame) {
        g_started = true;
        g_t0 = now_ms();
        g_start_frame = g_frame;
        RLOG(CH_CORE, L_INFO, "test: clock starts at the first frame (%.1f s)", g_t0 / 1000.0);
    }
    if (!g_started) {
        if (g_cfg.test_timeout > 0 && now_ms() >= g_cfg.test_timeout * 1000.0) {
            RLOG(CH_CORE, L_WARN, "test: no match on screen after %g s - shot of the screen, then the end",
                 g_cfg.test_timeout);
            shoot(d, L"shot_timeout.png");
            quit("timeout: the match never started");
        }
        return;
    }
    const double t = (now_ms() - g_t0) / 1000.0;
    while (g_next_wheel < g_cfg.test_wheel.size() && t >= g_cfg.test_wheel[g_next_wheel].first) {
        // queued, not sent: the window procedure runs on this thread and must not be entered from inside Present
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        d->GetCreationParameters(&cp);
        HWND w = cp.hFocusWindow;
        RECT r = {};
        if (w && GetWindowRect(w, &r)) {
            const int n = g_cfg.test_wheel[g_next_wheel].second;
            const LPARAM at = MAKELPARAM((r.left + r.right) / 2, (r.top + r.bottom) / 2);   // screen coordinates
            for (int i = 0; i < (n < 0 ? -n : n); ++i)
                PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, n < 0 ? -WHEEL_DELTA : WHEEL_DELTA), at);
            RLOG(CH_CORE, L_INFO, "test: %d mouse wheel notch(es) at %.2f s of the test clock", n, t);
        }
        ++g_next_wheel;
    }
    if (!g_cfg.test_keys.empty()) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        d->GetCreationParameters(&cp);
        const bool front = cp.hFocusWindow && GetForegroundWindow() == cp.hFocusWindow;
        for (size_t i = 0; i < g_held.size();) {
            if (t >= g_held[i].until || !front) {
                send_key(g_held[i].vk, true);
                RLOG(CH_CORE, front ? L_INFO : L_WARN, "test: key 0x%02X up at %.2f s of the test clock%s",
                     unsigned(g_held[i].vk), t, front ? "" : " (the game's window left the front)");
                g_held.erase(g_held.begin() + ptrdiff_t(i));
            } else {
                ++i;
            }
        }
        while (g_next_key < g_cfg.test_keys.size() && t >= g_cfg.test_keys[g_next_key].at) {
            const Config::TestKey& k = g_cfg.test_keys[g_next_key++];
            if (!front) {
                RLOG(CH_CORE, L_WARN, "test: key 0x%02X at %.2f s skipped - the game's window is not in front",
                     unsigned(k.vk), t);
                continue;
            }
            send_key(k.vk, false);
            g_held.push_back({k.vk, t + k.ms / 1000.0});
            RLOG(CH_CORE, L_INFO, "test: key 0x%02X down at %.2f s of the test clock, for %.0f ms", unsigned(k.vk), t,
                 k.ms);
        }
    }
    if (g_cfg.test_options > 0 && !g_options_sent && t >= g_cfg.test_options) {
        g_options_sent = true;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        d->GetCreationParameters(&cp);
        if (HWND w = cp.hFocusWindow) {   // queued, as the wheel: the window procedure must not run inside Present
            const LPARAM down = LPARAM(1u | (MapVirtualKeyW(VK_F11, MAPVK_VK_TO_VSC) << 16));
            PostMessageW(w, WM_KEYDOWN, VK_F11, down);
            PostMessageW(w, WM_KEYUP, VK_F11, LPARAM(DWORD(down) | 0xC0000000u));
            if (!g_cfg.test_options_tab.empty()) ui_test_open_tab(g_cfg.test_options_tab.c_str());
            RLOG(CH_CORE, L_INFO, "test: F11 posted at %.2f s of the test clock (options window%s%s)", t,
                 g_cfg.test_options_tab.empty() ? "" : ", tab ", w2u(g_cfg.test_options_tab.c_str()).c_str());
        }
    }
    ui_test_tick();
    if (g_next < g_cfg.test_shots.size()) {
        if (g_wait_capture) {
            // this Present ends the captured frame (capture_on_present closes the file right after us); if the
            // capture file could not be opened, the shot goes ahead without it
            if (!capturing() && g_frame < g_capture_asked + 3) return;
            g_wait_capture = false;
        } else if (t >= g_cfg.test_shots[g_next]) {
            if (g_cfg.test_capture && capture_request()) {
                g_wait_capture = true;   // the next frame is captured; its Present takes the shot
                g_capture_asked = g_frame;
                return;
            }
        } else {
            return;
        }
        wchar_t name[64];
        swprintf_s(name, L"shot_%u_%gs.png", unsigned(g_next + 1), g_cfg.test_shots[g_next]);
        RLOG(CH_CORE, L_INFO, "test: shot %u at %.2f s of the test clock (frame %ld)", unsigned(g_next + 1), t,
             (long)g_frame);
        shoot(d, name);
        ++g_next;
        return;
    }
    if (g_cfg.test_quit > 0 && t >= g_cfg.test_quit) quit("test finished");
}

// From the background thread: the hard stop when the game no longer presents frames (a hang, a modal box) —
// 30 s after the later of the timeout and the planned end of the test.
void shots_tick() {
    if (!g_test || g_quitting || g_cfg.test_timeout <= 0) return;
    double deadline = g_cfg.test_timeout * 1000.0;
    if (g_started && g_cfg.test_quit > 0) deadline = std::max(deadline, g_t0 + g_cfg.test_quit * 1000.0);
    if (now_ms() >= deadline + 30000.0) quit("hard timeout: no frames");
}

double test_clock() { return g_started ? (now_ms() - g_t0) / 1000.0 : -1.0; }

void shots_summary() {
    release_keys(test_clock(), "the process ends");   // a key left down would stay down for the whole system
    if (!g_test && !g_cfg.shot_vk) return;
    RLOG(CH_CORE, L_INFO, "shots: %ld written, %ld failed%s", g_written, g_failed,
         g_test ? (g_started ? "" : " | the test clock never started") : "");
}

}  // namespace rol
