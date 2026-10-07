// Live reverse-engineering probes — developer tools, off unless [debug] watch / ui_probe are set ("probe" channel).
//
//  * watch= : up to four hardware breakpoints (x86 debug registers DR0-DR3) on data writes, data reads/writes or
//    code execution. Every distinct instruction that hits one is logged once — with registers, the watched
//    value and the return addresses found on the stack — and counted; the counts are listed on the probe key and
//    at exit. Static analysis tells what a function does; this tells which functions touch a value in a live game.
//    Debug registers are per thread: they are set on every thread that exists at the first frame and again on
//    each probe key press (threads started later are not covered until then).
//  * ui_probe=1 : identifies the object the UI layout takes the screen rectangle from ([[0x1463c94]+4], virtual
//    methods +0xD4 copy / +0xD8 pointer, read by the top-level window layout 0x00FB7BC0 and 0x00FB7980), logs its
//    vtable and the rectangle, next to the renderer's own screen size (device wrapper [0x1465C54]).
//    ui_probe=2 also watches the rectangle, listing every instruction that reads it.
//  * watch_shadow_matrix=1 : a write watch on the first element of the engine's ShadowMapProjection shader variable
//    once it exists — finds the code that fits the shadow map to the camera.
//
// Engine rectangles are int16 {left, right, top, bottom}. Addresses are for legends.exe 1.0 (Steam), image base
// 0x400000; the probe checks the layout function's first bytes before touching engine memory.
#include "rolhook.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace rol {
namespace {

constexpr uintptr_t G_APP = 0x01463C94;          // object whose +4 provides the screen rectangle to the UI
constexpr uintptr_t G_RENDER_WRAPPER = 0x01465C54;   // renderer device wrapper (+0x104 rect, +0x1D0 w/h)
constexpr uintptr_t FN_TOPLEVEL_LAYOUT = 0x00FB7BC0;
const unsigned char kLayoutSig[8] = {0x83, 0xEC, 0x08, 0xF6, 0x05, 0x2C, 0x8E, 0x26};

enum Kind { K_EXEC = 0, K_WRITE = 1, K_RW = 3 };
struct Watch {
    uintptr_t addr = 0;
    int len = 4;
    int kind = K_RW;
    bool on = false;
    volatile LONG hits = 0;
    char label[40] = {};
};
Watch g_w[4];
int g_max_hits = 3000;   // per watch and thread: past this the breakpoint is cleared on that thread

struct Hit {
    uint32_t eip;
    uint32_t count;
    uint32_t slot;
};
constexpr int kHitCap = 4096;
Hit g_hits[kHitCap];
volatile LONG g_hit_lock = 0;
volatile LONG g_distinct = 0;
volatile LONG g_flood_logged = 0;

PVOID g_veh = nullptr;

// [debug] watch_follow = <code address>:<register>[+offset][:<length><w|rw>]: an execute breakpoint at the code address;
// its first hit reads the register (+ offset) - the address of something the code is about to use, e.g. a bone's
// matrix in a skinning loop - and at the next frame the execute breakpoint gives way to a data watch there (the
// default 4 bytes, write).
int g_follow_slot = -1;
int g_follow_reg = -1;           // index into the CONTEXT registers below
long g_follow_off = 0;
int g_follow_len = 4, g_follow_kind = 1;
volatile LONG g_follow_hit = 0;
uintptr_t g_follow_addr = 0;
bool g_follow_armed = false;
double g_follow_at = 0;          // [debug] watch_follow_at: seconds of the test clock before the breakpoint is set
bool g_follow_started = false;
const char* const kRegs[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"};
// [debug] watch_follow_if = <register>[+offset]<op><value> [& ...]: the execute breakpoint's hit counts only when
// every condition holds for the dword at register + offset (signed; op < <= > >= == !=; the value a number or another
// register[+offset]'s dword) - e.g. only the objects with a small animation handle (buildings) whose time moved
// since the last evaluation, among all that pass the code
struct Cond {
    int reg;
    long off;
    int op;   // 0 <, 1 <=, 2 >, 3 >=, 4 ==, 5 !=
    long val;
    int reg2;   // >= 0: the value is the dword at reg2 + off2
    long off2;
};
Cond g_conds[4];
int g_nconds = 0;
volatile LONG g_follow_skipped = 0;   // hits that did not meet the conditions

DWORD reg_of(const CONTEXT* c, int r) {
    switch (r) {
    case 0: return c->Eax;
    case 1: return c->Ebx;
    case 2: return c->Ecx;
    case 3: return c->Edx;
    case 4: return c->Esi;
    case 5: return c->Edi;
    case 6: return c->Ebp;
    default: return c->Esp;
    }
}

bool conds_hold(const CONTEXT* c) {
    for (int i = 0; i < g_nconds; ++i) {
        const Cond& k = g_conds[i];
        long v = 0, w = k.val;
        __try {
            v = *reinterpret_cast<const long*>(uintptr_t(reg_of(c, k.reg)) + k.off);
            if (k.reg2 >= 0) w = *reinterpret_cast<const long*>(uintptr_t(reg_of(c, k.reg2)) + k.off2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        const bool ok = k.op == 0 ? v < w : k.op == 1 ? v <= w : k.op == 2 ? v > w : k.op == 3 ? v >= w
                        : k.op == 4 ? v == w : v != w;
        if (!ok) return false;
    }
    return true;
}

// "edi+0x10>=0 & edi+0x10<16": up to 4 conditions; false when a part is not understood
bool parse_conds(const std::string& s) {
    g_nconds = 0;
    size_t p = 0;
    while (p < s.size() && g_nconds < 4) {
        size_t e = s.find('&', p);
        std::string part = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? s.size() : e + 1;
        part.erase(0, part.find_first_not_of(' '));
        part.erase(part.find_last_not_of(' ') + 1);
        if (part.empty()) continue;
        Cond k = {-1, 0, -1, 0, -1, 0};
        for (int r = 0; r < 8; ++r)
            if (!_strnicmp(part.c_str(), kRegs[r], 3)) k.reg = r;
        if (k.reg < 0) return false;
        const char* q = part.c_str() + 3;
        char* end = nullptr;
        if (*q == '+' || *q == '-') {
            k.off = strtol(q, &end, 0);
            q = end;
        }
        static const char* const ops[] = {"<=", ">=", "==", "!=", "<", ">"};
        static const int codes[] = {1, 3, 4, 5, 0, 2};
        for (int i = 0; i < 6 && k.op < 0; ++i)
            if (!strncmp(q, ops[i], strlen(ops[i]))) {
                k.op = codes[i];
                q += strlen(ops[i]);
            }
        if (k.op < 0) return false;
        for (int r = 0; r < 8; ++r)
            if (!_strnicmp(q, kRegs[r], 3)) k.reg2 = r;
        if (k.reg2 >= 0) {
            q += 3;
            if (*q == '+' || *q == '-') k.off2 = strtol(q, &end, 0);
        } else {
            k.val = long(strtoul(q, &end, 0));   // 0xFFFFFFFF = -1
            if (end == q) return false;
        }
        g_conds[g_nconds++] = k;
    }
    return true;
}
// [debug] watch_deref = <pointer address>+<offset>[:<length><w|rw>], watch_deref_at = <seconds of the test clock>: a
// data watch on what a global pointer points at, + offset, once the pointer is set (e.g. the game world's clock,
// [0x013E50AC] + 0x38: who advances it)
uintptr_t g_deref_base = 0;
long g_deref_off = 0;
int g_deref_len = 4, g_deref_kind = 1;
double g_deref_at = 0;
bool g_deref_armed = false;
// [debug] watch_shadow_matrix=1: a write watchpoint on the engine's ShadowMapProjection shader variable once it exists
// ([[0x01455970 + 0x18] + 0x40], matrix at +0x34): the code that fits the shadow map to the camera writes it.
bool g_watch_shadow = false, g_shadow_armed = false;
uintptr_t shadow_var() {
    __try {
        uintptr_t table = *reinterpret_cast<const uintptr_t*>(0x01455970 + 0x18);
        return table ? *reinterpret_cast<const uintptr_t*>(table + 0x40) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uintptr_t g_text_lo = 0, g_text_hi = 0;   // legends.exe code range
int g_ui_probe = 0;
int g_probe_vk = VK_DIVIDE;
bool g_probe_key_down = false;
bool g_armed_once = false;
bool g_ui_found = false;
double g_next_ui_try = 0;
bool g_sig_ok = false;

// ---------------------------------------------------------------- debug registers
DWORD dr7_value() {
    DWORD v = 0;
    for (int i = 0; i < 4; ++i) {
        if (!g_w[i].on) continue;
        DWORD len = g_w[i].kind == K_EXEC ? 0 : g_w[i].len == 1 ? 0 : g_w[i].len == 2 ? 1 : 3;
        v |= 1u << (2 * i);
        v |= DWORD(g_w[i].kind) << (16 + 4 * i);
        v |= len << (18 + 4 * i);
    }
    if (v) v |= 1u << 8;   // LE: exact data breakpoints
    return v;
}

struct ArmJob {
    std::vector<DWORD> tids;
    int done = 0;
};

// Runs on its own thread: suspends each thread, sets DR0-DR3/DR7, resumes. Nothing here allocates, so a thread
// suspended while holding the heap lock cannot deadlock it.
DWORD WINAPI arm_worker(void* p) {
    auto* job = static_cast<ArmJob*>(p);
    const DWORD dr7 = dr7_value();
    for (DWORD tid : job->tids) {
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!th) continue;
        if (SuspendThread(th) != DWORD(-1)) {
            CONTEXT c = {};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                c.Dr0 = g_w[0].on ? g_w[0].addr : 0;
                c.Dr1 = g_w[1].on ? g_w[1].addr : 0;
                c.Dr2 = g_w[2].on ? g_w[2].addr : 0;
                c.Dr3 = g_w[3].on ? g_w[3].addr : 0;
                c.Dr6 = 0;
                c.Dr7 = dr7;
                if (SetThreadContext(th, &c)) ++job->done;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    return 0;
}

void arm_all_threads(const char* why) {
    ArmJob job;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof(te)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid) job.tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    HANDLE h = CreateThread(nullptr, 0, arm_worker, &job, 0, nullptr);
    if (!h) return;
    WaitForSingleObject(h, 10000);
    CloseHandle(h);
    RLOG(CH_PROBE, L_INFO, "watchpoints %s on %d/%d threads (DR7 0x%08lx, %s)", dr7_value() ? "set" : "cleared",
         job.done, int(job.tids.size()), (unsigned long)dr7_value(), why);
}

// ---------------------------------------------------------------- hits
bool in_text(uint32_t v) { return v >= g_text_lo && v < g_text_hi; }

bool read_u32(uintptr_t a, uint32_t* v) {
    __try {
        *v = *reinterpret_cast<const volatile uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void log_hit(int slot, const CONTEXT* c) {
    const Watch& w = g_w[slot];
    uint32_t callers[8];
    int n = game_callers(reinterpret_cast<const void*>(uintptr_t(c->Esp)), callers, 8);
    char cs[8 * 12 + 1] = {};
    for (int i = 0; i < n; ++i) _snprintf_s(cs + strlen(cs), sizeof(cs) - strlen(cs), _TRUNCATE, " %08x", callers[i]);
    if (w.kind == K_EXEC) {
        uint32_t a[4] = {};
        for (int i = 0; i < 4; ++i) read_u32(c->Esp + 4 + 4 * i, &a[i]);
        RLOG(CH_PROBE, L_INFO,
             "watch %d '%s' call  eip %08lx thread %lu | ecx %08lx edx %08lx eax %08lx esi %08lx edi %08lx | "
             "args %08x %08x %08x %08x | callers%s",
             slot, w.label, (unsigned long)c->Eip, GetCurrentThreadId(), (unsigned long)c->Ecx, (unsigned long)c->Edx,
             (unsigned long)c->Eax, (unsigned long)c->Esi, (unsigned long)c->Edi, a[0], a[1], a[2], a[3], cs);
    } else {
        // a plain read of the watched address would trip the breakpoint again inside this handler; the kernel copy
        // of ReadProcessMemory does not
        uint32_t v = 0;
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(w.addr), &v, 4, nullptr);
        RLOG(CH_PROBE, L_INFO,
             "watch %d '%s' %s after eip %08lx thread %lu | value %08x | eax %08lx ecx %08lx edx %08lx esi %08lx "
             "edi %08lx | callers%s",
             slot, w.label, w.kind == K_WRITE ? "write" : "access", (unsigned long)c->Eip, GetCurrentThreadId(), v,
             (unsigned long)c->Eax, (unsigned long)c->Ecx, (unsigned long)c->Edx, (unsigned long)c->Esi,
             (unsigned long)c->Edi, cs);
    }
}

void record(int slot, const CONTEXT* c) {
    const uint32_t eip = c->Eip;
    bool fresh = false;
    while (InterlockedCompareExchange(&g_hit_lock, 1, 0)) YieldProcessor();
    uint32_t h = (eip * 2654435761u + uint32_t(slot)) & (kHitCap - 1);
    for (int k = 0; k < kHitCap; ++k) {
        Hit& e = g_hits[(h + k) & (kHitCap - 1)];
        if (e.count && e.eip == eip && e.slot == uint32_t(slot)) {
            ++e.count;
            break;
        }
        if (!e.count) {
            e.eip = eip;
            e.slot = uint32_t(slot);
            e.count = 1;
            fresh = true;
            ++g_distinct;
            break;
        }
    }
    InterlockedExchange(&g_hit_lock, 0);
    if (fresh) log_hit(slot, c);
}

volatile LONG g_steps = 0, g_steps_unmatched = 0;   // single-step exceptions seen; those not ours (summary)

LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_steps);
    CONTEXT* c = ep->ContextRecord;
    DWORD fired = c->Dr6 & 0xF;
    if (!fired) {   // some kernels leave DR6 out of the context: match an execute breakpoint by address
        for (int i = 0; i < 4; ++i)
            if (g_w[i].on && g_w[i].kind == K_EXEC && c->Eip == g_w[i].addr) fired |= 1u << i;
        if (!fired) {
            InterlockedIncrement(&g_steps_unmatched);
            return EXCEPTION_CONTINUE_SEARCH;   // a genuine single step, not ours
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (!(fired & (1u << i)) || !g_w[i].on) continue;
        if (i == g_follow_slot && g_nconds && !conds_hold(c)) {   // not the object looked for: go on, not a hit
            InterlockedIncrement(&g_follow_skipped);
            c->EFlags |= 0x10000;
            continue;
        }
        if (i == g_follow_slot && !InterlockedExchange(&g_follow_hit, 1)) {
            g_follow_addr = uintptr_t(reg_of(c, g_follow_reg)) + g_follow_off;
            c->Dr7 &= ~(3u << (2 * i));   // on this thread at once; probe_on_present re-arms all threads
        }
        record(i, c);
        if (g_w[i].kind == K_EXEC) c->EFlags |= 0x10000;   // RF: run the instruction without trapping again
        if (g_max_hits > 0 && InterlockedIncrement(&g_w[i].hits) == g_max_hits) {
            c->Dr7 &= ~(3u << (2 * i));
            if (!InterlockedExchange(&g_flood_logged, 1))
                RLOG(CH_PROBE, L_WARN, "watch %d '%s' reached %d hits: cleared on thread %lu (probe key re-arms)", i,
                     g_w[i].label, g_max_hits, GetCurrentThreadId());
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void log_hit_summary(const char* why) {
    std::vector<Hit> v;
    while (InterlockedCompareExchange(&g_hit_lock, 1, 0)) YieldProcessor();
    for (const auto& e : g_hits)
        if (e.count) v.push_back(e);
    InterlockedExchange(&g_hit_lock, 0);
    std::sort(v.begin(), v.end(), [](const Hit& a, const Hit& b) {
        return a.slot != b.slot ? a.slot < b.slot : a.count > b.count;
    });
    RLOG(CH_PROBE, L_INFO, "watch summary (%s): %d distinct instructions; %ld single-step exceptions, %ld of them not "
         "matched to a watch (DR6 empty), %ld watch_follow hits skipped by watch_follow_if", why, int(v.size()), g_steps,
         g_steps_unmatched, g_follow_skipped);
    for (const auto& e : v)
        RLOG(CH_PROBE, L_INFO, "  watch %u '%s'  eip %08x  x%u", e.slot, g_w[e.slot].label, e.eip, e.count);
}

bool add_watch(uintptr_t addr, int len, int kind, const char* label) {
    for (auto& w : g_w) {
        if (w.on) continue;
        w.addr = addr;
        w.kind = kind;
        w.len = kind == K_EXEC ? 1 : len;
        w.hits = 0;
        w.on = true;
        strncpy_s(w.label, label, _TRUNCATE);
        return true;
    }
    return false;
}

// "0x1464998:4:rw, 0xfb7bc0:x, 0x12345678:2:w" — address[:length 1|2|4][:x|w|rw]; the default is 4:rw.
void parse_watches(const std::wstring& s) {
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(L',', p);
        std::wstring item = s.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
        p = e == std::wstring::npos ? s.size() : e + 1;
        while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
        while (!item.empty() && iswspace(item.back())) item.pop_back();
        if (item.empty()) continue;
        wchar_t* end = nullptr;
        uintptr_t addr = uintptr_t(wcstoul(item.c_str(), &end, 16));
        int len = 4, kind = K_RW;
        for (const wchar_t* q = end; q && *q;) {
            if (*q == L':') {
                ++q;
                continue;
            }
            if (iswdigit(*q)) {
                len = int(wcstol(q, const_cast<wchar_t**>(&q), 10));
            } else if (!_wcsnicmp(q, L"rw", 2)) {
                kind = K_RW;
                q += 2;
            } else if (*q == L'w' || *q == L'W') {
                kind = K_WRITE;
                ++q;
            } else if (*q == L'x' || *q == L'X') {
                kind = K_EXEC;
                ++q;
            } else {
                ++q;
            }
        }
        if (len != 1 && len != 2 && len != 4) len = 4;
        if (kind != K_EXEC) addr &= ~uintptr_t(len - 1);   // the CPU wants the range aligned to its length
        if (!addr || !add_watch(addr, len, kind, w2u(item.c_str()).c_str()))
            RLOG(CH_PROBE, L_WARN, "watch '%s' ignored (bad address or all four debug registers taken)",
                 w2u(item.c_str()).c_str());
    }
}

// ---------------------------------------------------------------- UI probe
typedef const int16_t*(__thiscall* RectPtr_t)(void* self);

const int16_t* call_rect_ptr(void* obj, uintptr_t fn) {
    __try {
        return reinterpret_cast<RectPtr_t>(fn)(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool read_rect(const void* p, int16_t r[4]) {
    __try {
        memcpy(r, p, 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns true once the screen object exists (the UI is up).
bool ui_probe_run(const char* why) {
    uint32_t app = 0, scr = 0, vt = 0, s53 = 0, s54 = 0, s39 = 0, s42 = 0;
    if (!read_u32(G_APP, &app) || !app || !read_u32(app + 4, &scr) || !scr || !read_u32(scr, &vt) || !vt) return false;
    read_u32(vt + 53 * 4, &s53);
    read_u32(vt + 54 * 4, &s54);
    read_u32(vt + 39 * 4, &s39);
    read_u32(vt + 42 * 4, &s42);
    RLOG(CH_PROBE, L_INFO,
         "ui probe (%s): app [0x%08x] = %08x | screen object [app+4] = %08x, vtable %08x | slot 53 (+0xD4 copy rect) "
         "%08x, slot 54 (+0xD8 rect ptr) %08x, slot 39 %08x, slot 42 %08x",
         why, unsigned(G_APP), app, scr, vt, s53, s54, s39, s42);
    if (in_text(s54)) {
        const int16_t* p = call_rect_ptr(reinterpret_cast<void*>(uintptr_t(scr)), s54);
        int16_t r[4];
        if (p && read_rect(p, r)) {
            long off = long(reinterpret_cast<uintptr_t>(p)) - long(scr);
            RLOG(CH_PROBE, L_INFO, "ui probe: screen rect at %p (screen object %+ld): left %d right %d top %d bottom %d",
                 p, off, r[0], r[1], r[2], r[3]);
            if (g_ui_probe >= 2 && !g_ui_found) {
                if (add_watch(reinterpret_cast<uintptr_t>(p) & ~uintptr_t(3), 4, K_RW, "screen rect l/r"))
                    arm_all_threads("ui probe: screen rect");
            }
        } else {
            RLOG(CH_PROBE, L_WARN, "ui probe: slot 54 call failed (%p)", p);
        }
    }
    uint32_t wr = 0;
    if (read_u32(G_RENDER_WRAPPER, &wr) && wr) {
        uint32_t wvt = 0, w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        int16_t r[4] = {};
        read_u32(wr, &wvt);
        read_rect(reinterpret_cast<void*>(uintptr_t(wr) + 0x104), r);
        read_u32(wr + 0x1D0, &w1);
        read_u32(wr + 0x1D4, &h1);
        read_u32(wr + 0xA0, &w2);
        read_u32(wr + 0xA4, &h2);
        RLOG(CH_PROBE, L_INFO,
             "ui probe: renderer wrapper [0x%08x] = %08x, vtable %08x | rect +0x104: l %d r %d t %d b %d | "
             "+0x1D0 %ux%u | +0xA0 %ux%u",
             unsigned(G_RENDER_WRAPPER), wr, wvt, r[0], r[1], r[2], r[3], w1, h1, w2, h2);
    }
    return true;
}

}  // namespace

void probe_install(HMODULE exe) {
    auto base = reinterpret_cast<BYTE*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) {
            g_text_lo = uintptr_t(base) + sec->VirtualAddress;
            g_text_hi = g_text_lo + sec->Misc.VirtualSize;
        }
    g_sig_ok = uintptr_t(base) == 0x00400000 &&
               !memcmp(reinterpret_cast<const void*>(FN_TOPLEVEL_LAYOUT), kLayoutSig, sizeof(kLayoutSig));

    g_max_hits = config_int(L"debug", L"watch_max_hits", 3000, true);
    g_ui_probe = g_sig_ok ? config_int(L"debug", L"ui_probe", 0, true) : 0;
    g_probe_vk = config_key(L"debug", L"probe_key", VK_DIVIDE);
    parse_watches(config_str(L"debug", L"watch", L""));
    const std::string follow = w2u(config_str(L"debug", L"watch_follow", L"").c_str());
    if (!follow.empty()) {
        unsigned long code = 0;
        char reg[8] = {}, rest[32] = {};
        if (sscanf_s(follow.c_str(), "%lx:%3s%31s", &code, reg, unsigned(sizeof(reg)), rest, unsigned(sizeof(rest))) >= 2) {
            for (int r = 0; r < 8; ++r)
                if (!_stricmp(reg, kRegs[r])) g_follow_reg = r;
            const char* q = rest;
            if (*q == '+' || *q == '-') {
                char* end = nullptr;
                g_follow_off = strtol(q, &end, 0);
                q = end;
            }
            if (*q == ':') {
                ++q;
                if (isdigit(static_cast<unsigned char>(*q))) g_follow_len = atoi(q);
                while (isdigit(static_cast<unsigned char>(*q))) ++q;
                g_follow_kind = !_strnicmp(q, "rw", 2) ? K_RW : K_WRITE;
            }
            if (g_follow_reg >= 0 && add_watch(code, 1, K_EXEC, "follow")) {
                for (int i = 0; i < 4; ++i)
                    if (g_w[i].on && g_w[i].kind == K_EXEC && g_w[i].addr == code) g_follow_slot = i;
            }
        }
        if (g_follow_slot < 0) RLOG(CH_PROBE, L_WARN, "watch_follow=%s not understood", follow.c_str());
        g_follow_at = _wtof(config_str(L"debug", L"watch_follow_at", L"0").c_str());
        const std::string cond = w2u(config_str(L"debug", L"watch_follow_if", L"").c_str());
        if (!cond.empty() && !parse_conds(cond)) {
            RLOG(CH_PROBE, L_WARN, "watch_follow_if=%s not understood - watch_follow off", cond.c_str());
            if (g_follow_slot >= 0) g_w[g_follow_slot].on = false;
            g_follow_slot = -1;
        } else if (g_nconds) {
            RLOG(CH_PROBE, L_INFO, "watch_follow: a hit counts only if %s (%d conditions)", cond.c_str(), g_nconds);
        }
        if (g_follow_slot >= 0 && g_follow_at > 0) g_w[g_follow_slot].on = false;   // set later (probe_on_present)
        else g_follow_started = true;
    }
    g_watch_shadow = g_sig_ok && config_int(L"debug", L"watch_shadow_matrix", 0, true) != 0;
    const std::string deref = w2u(config_str(L"debug", L"watch_deref", L"").c_str());
    if (!deref.empty()) {
        unsigned long base = 0;
        long off = 0;
        char rest[16] = {};
        if (sscanf_s(deref.c_str(), "%lx%li%15s", &base, &off, rest, unsigned(sizeof(rest))) >= 2 && base) {
            g_deref_base = base;
            g_deref_off = off;
            const char* q = rest;
            if (*q == ':') ++q;
            if (isdigit(static_cast<unsigned char>(*q))) g_deref_len = atoi(q);
            while (isdigit(static_cast<unsigned char>(*q))) ++q;
            g_deref_kind = !_strnicmp(q, "rw", 2) ? K_RW : K_WRITE;
            if (g_deref_len != 1 && g_deref_len != 2 && g_deref_len != 4) g_deref_len = 4;
            g_deref_at = _wtof(config_str(L"debug", L"watch_deref_at", L"0").c_str());
        } else {
            RLOG(CH_PROBE, L_WARN, "watch_deref=%s not understood (pointer address+offset[:length w|rw])", deref.c_str());
        }
    }
    bool any = false;
    for (const auto& w : g_w) any |= w.on;
    if (!any && !g_ui_probe && !g_watch_shadow && g_follow_slot < 0 && !g_deref_base) return;
    if (!g_sig_ok)
        RLOG(CH_PROBE, L_WARN, "probe: unknown legends.exe build - ui_probe disabled (watchpoints still work)");
    g_veh = AddVectoredExceptionHandler(1, veh);
    for (int i = 0; i < 4; ++i)
        if (g_w[i].on)
            RLOG(CH_PROBE, L_INFO, "watch %d: %08lx len %d %s ('%s')", i, (unsigned long)g_w[i].addr, g_w[i].len,
                 g_w[i].kind == K_EXEC ? "execute" : g_w[i].kind == K_WRITE ? "write" : "read/write", g_w[i].label);
    RLOG(CH_PROBE, L_INFO, "probe ready: ui_probe %d, probe key 0x%02x, max hits per watch %d (armed at the first frame)",
         g_ui_probe, g_probe_vk, g_max_hits);
}

// Called at every Present.
void probe_on_present() {
    if (!g_veh) return;
    if (!g_armed_once) {
        g_armed_once = true;
        bool any = false;
        for (const auto& w : g_w) any |= w.on;
        if (any) arm_all_threads("first frame");
    }
    if (g_follow_slot >= 0 && !g_follow_started && test_clock() >= g_follow_at) {
        g_follow_started = true;
        g_w[g_follow_slot].on = true;
        arm_all_threads("watch_follow: the execute breakpoint");
    }
    if (g_follow_slot >= 0 && g_follow_hit && !g_follow_armed) {
        g_follow_armed = true;
        g_w[g_follow_slot].on = false;
        char label[40];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%s%+ld at %08lX", kRegs[g_follow_reg], g_follow_off,
                    (unsigned long)g_w[g_follow_slot].addr);
        if (g_follow_len != 1 && g_follow_len != 2 && g_follow_len != 4) g_follow_len = 4;
        add_watch(g_follow_addr & ~uintptr_t(g_follow_len - 1), g_follow_len, g_follow_kind, label);
        RLOG(CH_PROBE, L_INFO, "watch_follow: %s = %08lx - the execute breakpoint gives way to a %s watch there", label,
             (unsigned long)g_follow_addr, g_follow_kind == K_WRITE ? "write" : "read/write");
        arm_all_threads("watch_follow");
    }
    if (g_deref_base && !g_deref_armed && test_clock() >= g_deref_at) {
        uintptr_t p = 0;
        __try {
            p = *reinterpret_cast<const uintptr_t*>(g_deref_base);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            p = 0;
        }
        if (p) {
            g_deref_armed = true;
            char label[48];
            _snprintf_s(label, sizeof(label), _TRUNCATE, "[%08lX]%+ld", (unsigned long)g_deref_base, g_deref_off);
            const uintptr_t a = (p + g_deref_off) & ~uintptr_t(g_deref_len - 1);
            if (add_watch(a, g_deref_len, g_deref_kind, label)) {
                RLOG(CH_PROBE, L_INFO, "watch_deref: %s = %08lx - %s watch there", label, (unsigned long)a,
                     g_deref_kind == K_WRITE ? "write" : "read/write");
                arm_all_threads("watch_deref");
            }
        }
    }
    if (g_watch_shadow && !g_shadow_armed) {
        uintptr_t var = shadow_var();
        if (var) {
            g_shadow_armed = true;
            if (add_watch(var + 0x34, 4, K_WRITE, "ShadowMapProjection m00")) {
                RLOG(CH_PROBE, L_INFO, "shadow matrix variable at %08lx: write watch on its first element",
                     (unsigned long)var);
                arm_all_threads("shadow matrix");
            }
        }
    }
    if (g_ui_probe && !g_ui_found && now_ms() >= g_next_ui_try) {
        g_next_ui_try = now_ms() + 1000.0;
        g_ui_found = ui_probe_run("first time the UI is up");
    }
    if (g_probe_vk) {
        bool down = (GetAsyncKeyState(g_probe_vk) & 0x8000) != 0;
        if (down && !g_probe_key_down) {
            if (g_ui_probe) ui_probe_run("probe key");
            log_hit_summary("probe key");
            for (auto& w : g_w) w.hits = 0;
            g_flood_logged = 0;
            arm_all_threads("probe key");
        }
        g_probe_key_down = down;
    }
}

void probe_summary() {
    if (!g_veh) return;
    log_hit_summary("exit");
}

// A watchpoint set from code while the game runs (motion.cpp: the memory the game copies a unit's vertices from):
// the hits go to probe.log like those of [debug] watch.
bool probe_watch_now(uintptr_t addr, int len, bool write_only, const char* label) {
    if (!g_text_lo) return false;   // probe_install has not run
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, veh);
    if (!g_veh) return false;
    if (len != 1 && len != 2 && len != 4) len = 4;
    addr &= ~uintptr_t(len - 1);
    if (!add_watch(addr, len, write_only ? K_WRITE : K_RW, label)) return false;
    RLOG(CH_PROBE, L_INFO, "watch set from code: %08lx len %d %s ('%s')", (unsigned long)addr, len,
         write_only ? "write" : "read/write", label);
    arm_all_threads(label);
    return true;
}

}  // namespace rol
