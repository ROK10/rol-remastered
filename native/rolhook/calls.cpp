// Call counter ([debug] count_calls = address/length, ...; [debug] count_window = start, seconds of the test clock):
// how many times engine functions run in each frame - which work every frame, which on the simulation's ticks.
// Each function's first `length` bytes (whole instructions without relative jumps or calls: modkit\tools\prologue.py
// prints the list) move to a trampoline; its entry counts the call and goes on. calls.log in the session folder: the
// addresses, then per frame of the window "C frame t_ms dt_ms n0 n1 ...". A developer tool, installed at the start.
#include "rolhook.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace rol {
namespace {

constexpr int kMax = 32;
uintptr_t g_addr[kMax];
volatile LONG g_count[kMax];
void* g_tramp[kMax];
int g_n = 0;
uint8_t* g_entries = nullptr;
double g_start = -1, g_len = 0;
bool g_open = false, g_done = false;
FILE* g_out = nullptr;
double g_last = -1;

}  // namespace

void calls_install() {
    const std::wstring list = config_str(L"debug", L"count_calls", L"");
    if (list.empty()) return;
    const std::wstring win = config_str(L"debug", L"count_window", L"");
    if (swscanf_s(win.c_str(), L"%lf , %lf", &g_start, &g_len) < 1 || g_start < 0) g_start = 0;
    if (g_len <= 0) g_len = 3;
    g_entries = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMax * 16, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_EXECUTE_READWRITE));
    if (!g_entries) return;
    const std::string s = w2u(list.c_str());
    size_t p = 0;
    while (p < s.size() && g_n < kMax) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        unsigned long a = 0, len = 0;
        if (sscanf_s(s.substr(p, q - p).c_str(), " %lx / %lu", &a, &len) == 2 && len >= 5 && len <= 16) {
            const int i = g_n;
            uint8_t* e = g_entries + i * 16;
            e[0] = 0xF0;   // lock inc dword ptr [g_count + 4i]
            e[1] = 0xFF;
            e[2] = 0x05;
            *reinterpret_cast<uint32_t*>(e + 3) = uint32_t(reinterpret_cast<uintptr_t>(&g_count[i]));
            e[7] = 0xFF;   // jmp dword ptr [g_tramp + 4i]
            e[8] = 0x25;
            *reinterpret_cast<uint32_t*>(e + 9) = uint32_t(reinterpret_cast<uintptr_t>(&g_tramp[i]));
            FlushInstructionCache(GetCurrentProcess(), e, 16);
            unsigned char expect[16];
            memcpy(expect, reinterpret_cast<const void*>(a), len);   // the length comes from prologue.py
            g_tramp[i] = detour(a, expect, int(len), e);
            if (g_tramp[i]) {
                g_addr[i] = a;
                ++g_n;
            } else {
                RLOG(CH_CORE, L_WARN, "call counter: 0x%08lX/%lu not hooked", a, len);
            }
        }
        p = q + 1;
    }
    RLOG(CH_CORE, L_INFO, "call counter: %d functions, %g s from %g s of the test clock", g_n, g_len, g_start);
}

void calls_on_present() {
    if (!g_n || g_done) return;
    const double t = test_clock();
    if (!g_open) {
        if (t < g_start) {
            for (int i = 0; i < g_n; ++i) InterlockedExchange(&g_count[i], 0);
            return;
        }
        if (_wfopen_s(&g_out, (g_cfg.session_dir + L"calls.log").c_str(), L"w") != 0 || !g_out) {
            g_done = true;
            return;
        }
        fputs("call counter: per frame C frame t_ms dt_ms and the calls of", g_out);
        for (int i = 0; i < g_n; ++i) fprintf(g_out, " %08X", unsigned(g_addr[i]));
        fputc('\n', g_out);
        for (int i = 0; i < g_n; ++i) InterlockedExchange(&g_count[i], 0);
        g_open = true;
        g_last = now_ms();
        return;
    }
    const double now = now_ms();
    fprintf(g_out, "C %ld %.3f %.3f", (long)g_frame, now, now - g_last);
    g_last = now;
    for (int i = 0; i < g_n; ++i) fprintf(g_out, " %ld", InterlockedExchange(&g_count[i], 0));
    fputc('\n', g_out);
    if (t >= g_start + g_len) {
        fclose(g_out);
        g_out = nullptr;
        g_done = true;
        RLOG(CH_CORE, L_INFO, "call counter: window closed (calls.log)");
    }
}

}  // namespace rol
