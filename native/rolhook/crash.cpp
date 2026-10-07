// Engine assert boxes (MessageBoxW with Abort/Retry/Ignore) and unhandled exceptions: log, minidump, chain.
#include "rolhook.h"

#include <dbghelp.h>
#include <intrin.h>

namespace rol {
namespace {

typedef int(WINAPI* MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SetUEF_t)(LPTOP_LEVEL_EXCEPTION_FILTER);
MessageBoxW_t o_MessageBoxW;
SetUEF_t o_SetUEF;
LPTOP_LEVEL_EXCEPTION_FILTER g_game_filter = nullptr;
volatile LONG g_crashed = 0;
volatile LONG g_asserts = 0;

std::string one_line(const wchar_t* s) {
    std::string t = w2u(s);
    for (auto& c : t)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return t;
}

int WINAPI h_MessageBoxW(HWND w, LPCWSTR text, LPCWSTR cap, UINT type) {
    bool is_assert = (type & MB_TYPEMASK) == MB_ABORTRETRYIGNORE;
    LONG n = is_assert ? InterlockedIncrement(&g_asserts) : 0;
    int lvl = is_assert ? L_ERROR : L_WARN;
    RLOG(CH_CORE, lvl, "%s #%ld [%s] type 0x%x: %s", is_assert ? "ASSERT" : "MessageBox", n, one_line(cap).c_str(),
         type, one_line(text).c_str());
    RLOG(CH_CORE, lvl, "  caller %s | stack %s", addr_desc(_ReturnAddress()).c_str(), stack_desc(1, 20).c_str());
    if (is_assert && g_cfg.assert_ignore) {
        RLOG(CH_CORE, L_ERROR, "  -> answered IGNORE automatically (assert_mode=ignore)");
        return IDIGNORE;
    }
    int r = o_MessageBoxW(w, text, cap, type);
    RLOG(CH_CORE, lvl, "  -> user answered %d", r);
    return r;
}

std::string walk_stack(const CONTEXT* ctx) {
    HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    CONTEXT c = *ctx;
    STACKFRAME64 sf = {};
    sf.AddrPC.Offset = c.Eip;
    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = c.Ebp;
    sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c.Esp;
    sf.AddrStack.Mode = AddrModeFlat;
    std::string s;
    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
            break;
        if (i) s += " <- ";
        s += addr_desc(reinterpret_cast<void*>(static_cast<uintptr_t>(sf.AddrPC.Offset)));
    }
    return s;
}

void write_dump(EXCEPTION_POINTERS* ep) {
#ifdef ROLHOOK_PLAYER   // the players' build writes no memory dump: the crash log above has the stack and registers
    (void)ep;
    RLOG(CH_CORE, L_ERROR, "  no minidump in the players' build");
#else
    std::wstring path = g_cfg.session_dir + L"crash.dmp";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        RLOG(CH_CORE, L_ERROR, "  cannot create minidump file (err %lu)", GetLastError());
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei = {GetCurrentThreadId(), ep, FALSE};
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                              MiniDumpWithUnloadedModules),
                                &mei, nullptr, nullptr);
    CloseHandle(f);
    RLOG(CH_CORE, L_ERROR, "  minidump %s: %s", ok ? "written" : "FAILED", w2u(path.c_str()).c_str());
#endif
}

LONG WINAPI rol_filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_crashed, 1) == 0) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        RLOG(CH_CORE, L_ERROR, "CRASH: exception 0x%08lx at %s", er->ExceptionCode, addr_desc(er->ExceptionAddress).c_str());
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            RLOG(CH_CORE, L_ERROR, "  access violation: %s of address 0x%08lx",
                 er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                 (unsigned long)er->ExceptionInformation[1]);
        const CONTEXT* c = ep->ContextRecord;
        RLOG(CH_CORE, L_ERROR, "  eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx ebp %08lx esp %08lx",
             c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        RLOG(CH_CORE, L_ERROR, "  stack: %s", walk_stack(c).c_str());
        memory_tick();
        if (g_cfg.crash_dump) write_dump(ep);
        log_flush();
    }
    return g_game_filter ? g_game_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI h_SetUEF(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    RLOG(CH_CORE, L_INFO, "game installs unhandled-exception filter %s (chained after rolhook)",
         addr_desc(reinterpret_cast<void*>(f)).c_str());
    LPTOP_LEVEL_EXCEPTION_FILTER prev = g_game_filter;
    g_game_filter = f;
    return prev;
}

}  // namespace

void crash_install(HMODULE exe) {
    SetUnhandledExceptionFilter(rol_filter);
    o_SetUEF = SetUnhandledExceptionFilter;
    bool a = patch_iat(exe, "KERNEL32.dll", "SetUnhandledExceptionFilter", (void*)h_SetUEF, (void**)&o_SetUEF);
    bool b = patch_iat(exe, "USER32.dll", "MessageBoxW", (void*)h_MessageBoxW, (void**)&o_MessageBoxW);
    if (!o_MessageBoxW) o_MessageBoxW = MessageBoxW;
    RLOG(CH_CORE, L_INFO, "crash: exception filter installed, SetUnhandledExceptionFilter %s, MessageBoxW %s (assert_mode=%s)",
         a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked", g_cfg.assert_ignore ? "ignore" : "ask");
}

}  // namespace rol
