// DllMain, environment report, background flush/memory thread, ddraw.dll export forwarding.
#include "rolhook.h"

#include <psapi.h>

#include <algorithm>

#define ROLHOOK_VERSION "0.9.23"

namespace rol {
namespace {

HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;
uint64_t g_peak_va = 0;

DWORD WINAPI bg_thread(void*) {
    double next_mem = now_ms() + g_cfg.memory_interval_s * 1000.0;
    while (!g_stop) {
        Sleep(DWORD(g_cfg.flush_ms));
        if (now_ms() >= next_mem) {
            memory_tick();
            next_mem = now_ms() + g_cfg.memory_interval_s * 1000.0;
        }
        log_flush();
        shots_tick();
        borderless_tick();
    }
    return 0;
}

void log_environment(HMODULE exe) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(exe, path, MAX_PATH);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    bool laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
    RLOG(CH_CORE, L_INFO, "rolhook %s | host %s | LAA %s | pid %lu", ROLHOOK_VERSION, rel_path(path).c_str(),
         laa ? "ON" : "off", GetCurrentProcessId());

    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    auto rgv = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW ov = {sizeof(ov)};
    if (rgv) rgv(&ov);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    RLOG(CH_CORE, L_INFO, "Windows %lu.%lu build %lu | %lu logical CPUs | RAM %llu MB | process address space %llu MB",
         ov.dwMajorVersion, ov.dwMinorVersion, ov.dwBuildNumber, si.dwNumberOfProcessors, ms.ullTotalPhys >> 20,
         ms.ullTotalVirtual >> 20);
    RLOG(CH_CORE, L_INFO, "command line: %s", w2u(GetCommandLineW()).c_str());
    wchar_t cwd[MAX_PATH] = {};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    RLOG(CH_CORE, L_INFO, "game folder %s | cwd %s", w2u(g_cfg.game_dir.c_str()).c_str(), w2u(cwd).c_str());
    RLOG(CH_CORE, L_INFO, "config: %s", config_sources().c_str());
    RLOG(CH_CORE, L_INFO,
         "levels: core %d files %d bigs %d textures %d d3d %d frame %d memory %d ui %d probe %d (0 off .. 5 trace) | "
         "assert_mode %s | fix_lockrect %d | crash_dump %d",
         g_level[CH_CORE], g_level[CH_FILES], g_level[CH_BIGS], g_level[CH_TEXTURES], g_level[CH_D3D],
         g_level[CH_FRAME], g_level[CH_MEMORY], g_level[CH_UI], g_level[CH_PROBE], g_cfg.assert_ignore ? "ignore" : "ask",
         g_cfg.fix_lockrect, g_cfg.crash_dump);
    if (g_cfg.anisotropy > 1) RLOG(CH_CORE, L_INFO, "anisotropic filtering forced: %dx", g_cfg.anisotropy);
    if (g_cfg.tex_override) {
        DWORD a = GetFileAttributesW(g_cfg.tex_override_dir.c_str());
        RLOG(CH_CORE, L_INFO, "HD texture override ON: %s%s", w2u(g_cfg.tex_override_dir.c_str()).c_str(),
             (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? "" : "  (FOLDER NOT FOUND)");
    }
    for (const wchar_t* f : {L"d3d9.dll", L"dxvk.conf", L"dinput8.dll", L"version.dll", L"winmm.dll", L"dsound.dll"})
        if (GetFileAttributesW((g_cfg.game_dir + f).c_str()) != INVALID_FILE_ATTRIBUTES)
            RLOG(CH_CORE, L_INFO, "found in game folder: %s", w2u(f).c_str());
}

}  // namespace

// The summaries written when the process ends (DLL_PROCESS_DETACH, or a test run ending the game itself).
void exit_summaries(const char* why) {
    InterlockedExchange(&g_stop, 1);
    RLOG(CH_CORE, L_INFO, "process exit (%s) - summaries follow", why);
    files_summary();
    gfx_summary();
    ui_summary();
    probe_summary();
    uiscale_summary();
    uilayout_summary();
    scriptui_summary();
    motion_summary();
    anim_summary();
    shaderfx_summary();
    ssao_summary();
    borderless_summary();
    shots_summary();
    memory_tick();
    log_shutdown();
}

void memory_tick() {
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    uint64_t total = ms.ullTotalVirtual, used = ms.ullTotalVirtual - ms.ullAvailVirtual;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    SIZE_T largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t p = 0x10000;
    while (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    g_peak_va = std::max(g_peak_va, used);
    int lvl = used > total / 100 * 85 ? L_WARN : L_INFO;
    RLOG(CH_MEMORY, lvl,
         "address space %llu / %llu MB used (peak %llu, largest free block %Iu MB) | private %Iu MB | working set %Iu MB (peak %Iu)",
         used >> 20, total >> 20, g_peak_va >> 20, largest >> 20, pmc.PrivateUsage >> 20, pmc.WorkingSetSize >> 20,
         pmc.PeakWorkingSetSize >> 20);
}

}  // namespace rol

// ------------------------------------------------------------------ ddraw.dll forwarding
// The game imports DirectDrawCreateEx statically, so this DLL is loaded before any game code runs.
// Every export jumps to the real system ddraw.dll, loaded on first use.
static FARPROC g_real[22];
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static const char* const kExports[22] = {
    "AcquireDDThreadLock", "CompleteCreateSysmemSurface", "D3DParseUnknownCommand", "DDGetAttachedSurfaceLcl",
    "DDInternalLock", "DDInternalUnlock", "DSoundHelp", "DirectDrawCreate", "DirectDrawCreateClipper",
    "DirectDrawCreateEx", "DirectDrawEnumerateA", "DirectDrawEnumerateExA", "DirectDrawEnumerateExW",
    "DirectDrawEnumerateW", "DllCanUnloadNow", "DllGetClassObject", "GetDDSurfaceLocal", "GetOLEThunkData",
    "GetSurfaceFromDC", "RegisterSpecialCase", "ReleaseDDThreadLock", "SetAppCompatData"};

static BOOL CALLBACK resolve_cb(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t p[MAX_PATH] = {};
    GetSystemDirectoryW(p, MAX_PATH);
    wcscat_s(p, L"\\ddraw.dll");
    HMODULE m = LoadLibraryW(p);
    int n = 0;
    for (int i = 0; i < 22; ++i) {
        g_real[i] = m ? GetProcAddress(m, kExports[i]) : nullptr;
        n += g_real[i] != nullptr;
    }
    RLOG(rol::CH_CORE, m ? rol::L_INFO : rol::L_ERROR, "system ddraw.dll %s for forwarding (%d/22 exports)",
         m ? "loaded" : "NOT FOUND", n);
    return TRUE;
}

extern "C" void __cdecl rol_resolve_ddraw() { InitOnceExecuteOnce(&g_once, resolve_cb, nullptr, nullptr); }

#define DDSTUB(i, name)                                                   \
    extern "C" __declspec(naked) void p_##name() {                        \
        __asm pushad                                                      \
        __asm call rol_resolve_ddraw                                      \
        __asm popad                                                       \
        __asm jmp dword ptr [g_real + (i) * 4]                            \
    }

DDSTUB(0, AcquireDDThreadLock)
DDSTUB(1, CompleteCreateSysmemSurface)
DDSTUB(2, D3DParseUnknownCommand)
DDSTUB(3, DDGetAttachedSurfaceLcl)
DDSTUB(4, DDInternalLock)
DDSTUB(5, DDInternalUnlock)
DDSTUB(6, DSoundHelp)
DDSTUB(7, DirectDrawCreate)
DDSTUB(8, DirectDrawCreateClipper)
DDSTUB(9, DirectDrawCreateEx)
DDSTUB(10, DirectDrawEnumerateA)
DDSTUB(11, DirectDrawEnumerateExA)
DDSTUB(12, DirectDrawEnumerateExW)
DDSTUB(13, DirectDrawEnumerateW)
DDSTUB(14, DllCanUnloadNow)
DDSTUB(15, DllGetClassObject)
DDSTUB(16, GetDDSurfaceLocal)
DDSTUB(17, GetOLEThunkData)
DDSTUB(18, GetSurfaceFromDC)
DDSTUB(19, RegisterSpecialCase)
DDSTUB(20, ReleaseDDThreadLock)
DDSTUB(21, SetAppCompatData)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    using namespace rol;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        log_init();
        if (!g_cfg.enabled) return TRUE;
        HMODULE exe = GetModuleHandleW(nullptr);
        log_environment(exe);
        crash_install(exe);
        files_install(exe);
        gfx_install(exe);
        ui_install(exe);
        camera_install(exe);
        probe_install(exe);
        motion_install();
        calls_install();
        uiscale_install(exe);
        uilayout_install(exe);
        scriptui_install(exe);
        spritelog_install(exe);
        anim_install(exe);
        shaderfx_apply(g_cfg.soft_shadows, g_cfg.gamma_light);
        ssao_apply(g_cfg.ssao);
        post_apply(g_cfg.sharpen, g_cfg.vibrance);
        lod_install(exe);
        borderless_install(exe);
        shadow_install(exe);
        shots_install(exe);
        memory_tick();
        g_thread = CreateThread(nullptr, 0, bg_thread, nullptr, 0, nullptr);
        RLOG(CH_CORE, L_INFO, "rolhook ready, session folder %s", w2u(g_cfg.session_dir.c_str()).c_str());
        log_flush();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_cfg.enabled) exit_summaries(reserved ? "process terminating" : "FreeLibrary");
    }
    return TRUE;
}
