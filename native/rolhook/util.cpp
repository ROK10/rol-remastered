// Helpers: string conversion, address description, stack capture, IAT / vtable patching.
#include "rolhook.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace rol {

std::string w2u(const wchar_t* s) {
    if (!s || !*s) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring a2w(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &out[0], n);
    return out;
}

std::string a2u(const char* s) { return w2u(a2w(s).c_str()); }

std::string rel_path(const wchar_t* path) {
    if (!path) return "(null)";
    const std::wstring& g = g_cfg.game_dir;
    if (!g.empty() && _wcsnicmp(path, g.c_str(), g.size()) == 0) return w2u(path + g.size());
    return w2u(path);
}

std::string module_name(HMODULE m) {
    if (!m) return "?";
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* b = wcsrchr(path, L'\\');
    return w2u(b ? b + 1 : path);
}

std::string addr_desc(const void* p) {
    char buf[320];
    HMODULE m = nullptr;
    if (p && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)p, &m) && m) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s+0x%x", module_name(m).c_str(),
                    (unsigned)((const char*)p - (const char*)m));
    } else {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%p", p);
    }
    return buf;
}

namespace {

bool is_executable(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// True when the bytes right before 'ret' encode a CALL (rel32, register, or memory forms).
bool follows_call(uintptr_t ret) {
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(ret);
        if (p[-5] == 0xE8) return true;                              // call rel32
        if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;    // call reg / [reg]
        if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;    // call [reg+disp8]
        if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10) return true;    // call [sib+disp8]
        if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;    // call [abs32] / [reg+disp32]
        if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10) return true;    // call [sib+disp32]
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

}  // namespace

// Frame-pointer walk first; the game is built with frame-pointer omission, so fall back to scanning the raw
// stack for values that are return addresses (point into code right after a CALL). Heuristic frames get '~'.
std::string stack_desc(int skip, int frames) {
    void* st[64];
    USHORT n = CaptureStackBackTrace(DWORD(skip + 1), DWORD(std::min(frames, 62)), st, nullptr);
    std::string s;
    for (USHORT i = 0; i < n; ++i) {
        if (i) s += " <- ";
        s += addr_desc(st[i]);
    }
    if (n >= 6) return s;

    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(&st[0]);
    auto tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
    int found = 0;
    uintptr_t prev = 0;
    for (int i = 0; i < 2048 && reinterpret_cast<uintptr_t>(sp + i) + sizeof(uintptr_t) <= top && found < frames; ++i) {
        uintptr_t v = sp[i];
        if (v == prev || v < 0x10000 || !is_executable(v) || !follows_call(v)) continue;
        prev = v;
        s += found || n ? " <~ " : "~ ";
        s += addr_desc(reinterpret_cast<void*>(v));
        ++found;
    }
    return s;
}

namespace {

uintptr_t g_exe_text_lo = 0, g_exe_text_hi = 0;

void find_exe_text() {
    auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 5)) {
            g_exe_text_lo = uintptr_t(base) + sec->VirtualAddress;
            g_exe_text_hi = g_exe_text_lo + sec->Misc.VirtualSize;
        }
}

}  // namespace

// Return addresses into the game's own code found on the stack from 'sp' up: the game omits frame pointers, so
// this is the practical "which engine function called" (values that point right after a CALL in legends.exe).
int game_callers(const void* sp, uint32_t* out, int cap) {
    if (!g_exe_text_lo) find_exe_text();
    int n = 0;
    __try {
        const uint32_t* p = static_cast<const uint32_t*>(sp);
        for (int i = 0; i < 1024 && n < cap; ++i) {
            uint32_t v = p[i];
            if (v >= g_exe_text_lo && v < g_exe_text_hi && follows_call(v) && (n == 0 || out[n - 1] != v)) out[n++] = v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

bool patch_code(uintptr_t at, const void* expect, const void* bytes, int len) {
    if (expect && memcmp(reinterpret_cast<const void*>(at), expect, size_t(len)) != 0) return false;
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(at), size_t(len), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(at), bytes, size_t(len));
    VirtualProtect(reinterpret_cast<void*>(at), size_t(len), old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size_t(len));
    return true;
}

// The first 'len' bytes at 'target' must be whole instructions without relative jumps or calls (checked against
// 'expect'): they move to a trampoline that jumps back after them, and 'target' gets a jmp to 'hook'.
void* detour(uintptr_t target, const unsigned char* expect, int len, void* hook) {
    if (len < 5 || memcmp(reinterpret_cast<const void*>(target), expect, size_t(len)) != 0) return nullptr;
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return nullptr;
    memcpy(t, expect, size_t(len));
    t[len] = 0xE9;
    *reinterpret_cast<int32_t*>(t + len + 1) = int32_t(target + len - (reinterpret_cast<uintptr_t>(t) + len + 5));
    uint8_t jmp[16];
    jmp[0] = 0xE9;
    *reinterpret_cast<int32_t*>(jmp + 1) = int32_t(reinterpret_cast<uintptr_t>(hook) - (target + 5));
    for (int i = 5; i < len; ++i) jmp[i] = 0x90;
    if (!patch_code(target, expect, jmp, len)) {
        VirtualFree(t, 0, MEM_RELEASE);
        return nullptr;
    }
    return t;
}

bool patch_slot(void** slot, void* hook, void** orig) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    if (orig) *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

bool patch_iat(HMODULE mod, const char* dll, const char* func, void* hook, void** orig) {
    auto base = reinterpret_cast<BYTE*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<char*>(base + imp->Name), dll) != 0) continue;
        if (!imp->OriginalFirstThunk) continue;  // no name table: cannot match by name
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(ibn->Name), func) != 0) continue;
            return patch_slot(reinterpret_cast<void**>(&addrs->u1.Function), hook, orig);
        }
    }
    return false;
}

}  // namespace rol
