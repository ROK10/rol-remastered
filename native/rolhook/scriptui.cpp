// Script window functions, traced ([debug] log_script_ui=1). The campaign map's windows (campaigns\<nation>\
// <nation>.campaign, <INTERFACE>) and some scenario windows are placed and sized by the game's scripts
// (scripts.big: campaigns\<nation>\<nation>_ui.bhs, ui_common.bhs, map scripts) through the script engine's window
// functions: get_screen_width, resize_window, label_get_text_height, wrap_height, move_window_on_screen... Each is a
// native registered by name (mov eax, native / mov ecx, L"name" / call 0x00C7E010), stdcall, its arguments pointers
// to the script's values: an int, or a string handle whose first field is the text. Some act on the window whose
// callback runs (no name argument). Whether the scripts' own arithmetic (pixel constants in the scripts) fits the
// scaled layouts shows in the trace: name(arguments after the call) -> result, a line in ui.log whenever a function's
// line differs from its previous one.
//
// One entry thunk per function pushes its index and jumps to a common thunk, which copies the arguments, calls the
// original through its trampoline, logs and returns to the caller past the arguments (their count from the table).
// Addresses are for legends.exe 1.0 (Steam); every function's first bytes are checked before it is hooked.
#include "rolhook.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace rol {
namespace {

struct Native {
    const char* name;
    uintptr_t va;
    int nargs;   // stdcall: 'ret 4 * nargs'
    int len;     // whole instructions >= 5 bytes moved into the trampoline
    unsigned char sig[12];
};

// rows from modkit/tools/script_natives.py --cpp <names>
const Native kNatives[] = {
    {"get_screen_width", 0x00B54CC0, 0, 5, {0xA1, 0x94, 0x3C, 0x46, 0x01}},
    {"get_screen_height", 0x00B54CE0, 0, 5, {0xA1, 0x94, 0x3C, 0x46, 0x01}},
    {"get_window_width", 0x00B54B50, 1, 9, {0x8B, 0x44, 0x24, 0x04, 0x68, 0x84, 0x48, 0x2A, 0x01}},
    {"get_window_height", 0x00B54B80, 1, 9, {0x8B, 0x44, 0x24, 0x04, 0x68, 0x84, 0x48, 0x2A, 0x01}},
    {"get_window_pos", 0x00B54BB0, 3, 7, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x0C}},
    {"set_window_width", 0x00B54AD0, 2, 5, {0x8B, 0x44, 0x24, 0x04, 0x56}},
    {"set_window_height", 0x00B54B10, 2, 5, {0x8B, 0x44, 0x24, 0x04, 0x56}},
    {"resize_window", 0x00B54A90, 3, 9, {0x8B, 0x44, 0x24, 0x04, 0x68, 0x84, 0x48, 0x2A, 0x01}},
    {"move_window", 0x00B547A0, 3, 9, {0x8B, 0x44, 0x24, 0x04, 0x68, 0x84, 0x48, 0x2A, 0x01}},
    {"move_window_on_screen", 0x00B547E0, 4, 6, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8}},
    {"window_get_width", 0x00B52550, 0, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"window_get_height", 0x00B52580, 0, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"window_get_pos", 0x00B52670, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"window_set_width", 0x00B523E0, 1, 6, {0x51, 0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"window_set_height", 0x00B52500, 1, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"window_set_width_centered", 0x00B52430, 1, 5, {0x83, 0xEC, 0x14, 0x53, 0x56}},
    {"window_move", 0x00B52700, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"resize_cur_window", 0x00B523A0, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"window_rect_width", 0x00B549A0, 2, 9, {0x8B, 0x44, 0x24, 0x04, 0x68, 0x84, 0x48, 0x2A, 0x01}},
    {"window_rect_height", 0x00B54960, 2, 9, {0x8B, 0x44, 0x24, 0x04, 0x68, 0x84, 0x48, 0x2A, 0x01}},
    {"window_wrap_height", 0x00B549E0, 3, 5, {0x8B, 0x44, 0x24, 0x04, 0x56}},
    {"label_get_text_height", 0x00B53080, 1, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"label_get_pos", 0x00B531D0, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"label_set_rect", 0x00B53010, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"wrap_height", 0x00B52110, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"wrap_height_using_rect", 0x00B52150, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"text_width", 0x00B54730, 1, 5, {0xA1, 0x90, 0xD2, 0x29, 0x01}},
    {"expander_set_height", 0x00B54490, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"expander_set_width", 0x00B54430, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"expander_set_centered_height", 0x00B543B0, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"expander_set_centered_width", 0x00B54330, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"button_move", 0x00B53B30, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"button_get_center", 0x00B53D00, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"sprite_move_id", 0x00B52B10, 4, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"sprite_move_to_sprite_id", 0x00B52BC0, 4, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"move_child_window_point", 0x00B54080, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"rect_width", 0x00B541A0, 1, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"rect_height", 0x00B54150, 1, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"rect_pos", 0x00B54100, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"point_pos", 0x00B541F0, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"tex_width", 0x00B542E0, 1, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"tex_pos", 0x00B54240, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"get_node_screen_pos", 0x00B56730, 3, 5, {0x8B, 0x44, 0x24, 0x04, 0x57}},
    {"lookup_node_screen_pos", 0x00B56790, 3, 5, {0x8B, 0x44, 0x24, 0x04, 0x53}},
    {"draw_sprite", 0x00B521F0, 1, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"wrap", 0x00B520C0, 4, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"wrap_using_rect", 0x00B52060, 2, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
    {"write_cent", 0x00B51940, 3, 5, {0xA1, 0x94, 0xD2, 0x29, 0x01}},
};
constexpr int kCount = int(sizeof(kNatives) / sizeof(kNatives[0]));
constexpr int kTest0 = kCount, kTest3 = kCount + 1;   // the self-test's two slots after the game's functions
constexpr int kSlots = kCount + 2;
constexpr int kMaxLines = 20000;

void* g_tramp[kSlots];
int g_nargs[kSlots];
long long g_calls[kSlots];
std::string g_last[kSlots];
int g_lines = 0, g_hooked = 0;
bool g_quiet = false;   // the self-test's calls are not logged
uint8_t* g_entries = nullptr;   // 10 bytes per slot: push index / jmp common

bool read_u32(uintptr_t a, uint32_t* v) {
    __try {
        *v = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// up to 48 printable characters of a UTF-16 string at a, at least 2; false if it is not one
bool read_text(uintptr_t a, char* out, int cap) {
    if (a < 0x10000) return false;
    int n = 0;
    __try {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(a);
        for (; n < cap - 1 && n < 48 && w[n]; ++n) {
            if (w[n] < 0x20 || w[n] > 0x7E) return false;
            out[n] = char(w[n]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    out[n] = 0;
    return n >= 2;
}

// a script value: the argument points at it; an int, or a string handle {wchar_t* text, ...} (in place, or a pointer
// to one)
std::string value_of(uint32_t arg) {
    char buf[64];
    uint32_t v, h;
    if (!read_u32(arg, &v)) {
        snprintf(buf, sizeof(buf), "?%08X", arg);
        return buf;
    }
    if (read_text(v, buf + 1, sizeof(buf) - 2) || (read_u32(v, &h) && read_text(h, buf + 1, sizeof(buf) - 2))) {
        buf[0] = '"';
        strcat_s(buf, "\"");
        return buf;
    }
    snprintf(buf, sizeof(buf), "%d", int(v));
    return buf;
}

void __stdcall on_native(int i, const uint32_t* args, uint32_t ret) {
    ++g_calls[i];
    if (g_quiet || g_lines >= kMaxLines) return;
    std::string line = kNatives[i].name;
    line += '(';
    for (int k = 0; k < g_nargs[i]; ++k) {
        if (k) line += ", ";
        line += value_of(args[k]);
    }
    char r[24];
    snprintf(r, sizeof(r), ") -> %d", int(ret));
    line += r;
    if (line == g_last[i]) return;
    g_last[i] = line;
    if (++g_lines == kMaxLines)
        RLOG(CH_UI, L_WARN, "script ui: %d lines, the trace stops here (calls are still counted)", kMaxLines);
    RLOG(CH_UI, L_INFO, "script ui: %s", line.c_str());
}

// [esp] index, [esp + 4] the caller's return address, [esp + 8] the arguments
__declspec(naked) void common_thunk() {
    __asm {
        pushad
        mov ebx, dword ptr [esp + 32]
        mov ecx, dword ptr [g_nargs + ebx * 4]
        lea esi, [esp + 40]
        mov edi, ecx
    copy_args:
        test edi, edi
        jz call_original
        dec edi
        push dword ptr [esi + edi * 4]
        jmp copy_args
    call_original:
        call dword ptr [g_tramp + ebx * 4]   // stdcall: pops the copies
        mov dword ptr [esp + 28], eax        // pushad's eax
        push eax
        push esi
        push ebx
        call on_native
        popad
        mov ecx, dword ptr [esp]
        mov ecx, dword ptr [g_nargs + ecx * 4]
        mov edx, dword ptr [esp + 4]
        lea esp, [esp + 8 + ecx * 4]
        jmp edx
    }
}

// Self-test of the thunks before the game's functions are hooked: two stand-ins with known first bytes, hooked the
// same way, called with known values; the result and the stack pointer after the call must be as without the hook.
const unsigned char kTest0Sig[5] = {0xB8, 0x07, 0x00, 0x00, 0x00};   // mov eax, 7
const unsigned char kTest3Sig[6] = {0x8B, 0x44, 0x24, 0x04,          // mov eax, [esp + 4]
                                    0x8B, 0x00};                     // mov eax, [eax]

__declspec(naked) int __stdcall test0() {
    __asm {
        mov eax, 7
        ret
    }
}

__declspec(naked) int __stdcall test3(const int*, const int*, const int*) {   // *a + 10 * *b + 100 * *c
    __asm {
        mov eax, dword ptr [esp + 4]
        mov eax, dword ptr [eax]
        mov ecx, dword ptr [esp + 8]
        mov ecx, dword ptr [ecx]
        imul ecx, ecx, 10
        add eax, ecx
        mov ecx, dword ptr [esp + 12]
        mov ecx, dword ptr [ecx]
        imul ecx, ecx, 100
        add eax, ecx
        ret 12
    }
}

// calls fn(a, b, c) (or fn() when n == 0); *moved = esp after the call minus esp before it (0 when balanced)
__declspec(noinline) int call_measured(void* fn, int n, const int* a, const int* b, const int* c, int* moved) {
    int r = 0, before = 0, after = 0;
    __asm {
        mov before, esp
        cmp n, 0
        je no_args
        push c
        push b
        push a
    no_args:
        call fn
        mov r, eax
        mov after, esp
        mov esp, before
    }
    *moved = after - before;
    return r;
}

bool self_test(std::string& why) {
    g_nargs[kTest0] = 0;
    g_nargs[kTest3] = 3;
    g_tramp[kTest0] = detour(reinterpret_cast<uintptr_t>(&test0), kTest0Sig, 5, g_entries + kTest0 * 10);
    g_tramp[kTest3] = detour(reinterpret_cast<uintptr_t>(&test3), kTest3Sig, 6, g_entries + kTest3 * 10);
    if (!g_tramp[kTest0] || !g_tramp[kTest3]) {
        why = "the stand-ins could not be hooked";
        return false;
    }
    const int a = 1, b = 2, c = 3;
    int moved0 = -1, moved3 = -1;
    g_quiet = true;
    const int r0 = call_measured(reinterpret_cast<void*>(&test0), 0, nullptr, nullptr, nullptr, &moved0);
    const int r3 = call_measured(reinterpret_cast<void*>(&test3), 3, &a, &b, &c, &moved3);
    g_quiet = false;
    char buf[160];
    snprintf(buf, sizeof(buf), "test0() -> %d (want 7), stack %+d; test3(1, 2, 3) -> %d (want 321), stack %+d; "
             "calls seen %lld + %lld", r0, moved0, r3, moved3, g_calls[kTest0], g_calls[kTest3]);
    why = buf;
    return r0 == 7 && moved0 == 0 && r3 == 321 && moved3 == 0 && g_calls[kTest0] == 1 && g_calls[kTest3] == 1;
}

}  // namespace

void scriptui_install(HMODULE exe) {
    if (!config_int(L"debug", L"log_script_ui", 0, true)) return;
    if (reinterpret_cast<uintptr_t>(exe) != 0x00400000) {
        RLOG(CH_UI, L_WARN, "script ui trace: off (unknown legends.exe build)");
        return;
    }
    g_entries = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSlots * 10, MEM_COMMIT | MEM_RESERVE,
                                                   PAGE_EXECUTE_READWRITE));
    if (!g_entries) return;
    for (int i = 0; i < kSlots; ++i) {
        uint8_t* e = g_entries + i * 10;
        e[0] = 0x68;   // push i
        *reinterpret_cast<int32_t*>(e + 1) = i;
        e[5] = 0xE9;   // jmp common_thunk
        *reinterpret_cast<int32_t*>(e + 6) =
            int32_t(reinterpret_cast<uintptr_t>(&common_thunk) - (reinterpret_cast<uintptr_t>(e) + 10));
    }
    FlushInstructionCache(GetCurrentProcess(), g_entries, kSlots * 10);
    std::string why;
    if (!self_test(why)) {
        RLOG(CH_UI, L_ERROR, "script ui trace: off, the thunk self-test failed: %s", why.c_str());
        return;
    }
    RLOG(CH_UI, L_INFO, "script ui trace: thunk self-test ok (%s)", why.c_str());
    std::string failed;
    for (int i = 0; i < kCount; ++i) {
        g_nargs[i] = kNatives[i].nargs;
        g_tramp[i] = detour(kNatives[i].va, kNatives[i].sig, kNatives[i].len, g_entries + i * 10);
        if (g_tramp[i]) {
            ++g_hooked;
        } else {
            failed += failed.empty() ? "" : ", ";
            failed += kNatives[i].name;
        }
    }
    RLOG(CH_UI, failed.empty() ? L_INFO : L_WARN, "script ui trace: %d of %d window functions hooked%s%s", g_hooked,
         kCount, failed.empty() ? "" : "; code differs: ", failed.c_str());
}

void scriptui_summary() {
    if (!g_hooked) return;
    std::string calls;
    for (int i = 0; i < kCount; ++i) {
        if (!g_calls[i]) continue;
        char b[80];
        snprintf(b, sizeof(b), "%s%s %lld", calls.empty() ? "" : ", ", kNatives[i].name, g_calls[i]);
        calls += b;
    }
    RLOG(CH_UI, L_INFO, "script ui trace: %d lines; calls: %s", g_lines, calls.empty() ? "none" : calls.c_str());
}

}  // namespace rol
