// File I/O tracing (opens, directory scans, existence checks, mappings) and .big archive tracing:
// every read from a .big handle is resolved to the archive entry it belongs to.
#include "rolhook.h"

#include <objbase.h>
#include <oleauto.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <unordered_map>
#include <vector>

namespace rol {
namespace {

struct BigEntry {
    uint32_t off = 0, end = 0, usize = 0;
    std::string name;
};

struct BigInfo {
    std::string path;    // as shown in logs
    std::string base;    // file name
    std::vector<BigEntry> ents;
    uint64_t size = 0;
    uint32_t opens = 0;
    uint64_t reads = 0, bytes = 0;
    bool parsed = false;
};

struct FindState {
    std::string pattern;
    int count = 0;
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<HANDLE, BigInfo*> g_handles;      // open handles of .big files
std::unordered_map<std::wstring, BigInfo*> g_bigs;   // by lower-case full path
std::unordered_map<HANDLE, FindState> g_finds;       // active FindFirstFile scans
std::unordered_map<HANDLE, std::string> g_fx_handles; // open shader files (.fx/.fxh/.fxo/.fxv): effects are named by them
// Their content: the engine reads every .fxv of the shader cache first and makes the effects afterwards, so the file
// read last names none of them (every effect was "vert_lighting_2.fxv"). What a shader file gave is kept per open
// handle, then by size and FNV-1a hash once closed: D3DXCreateEffect's buffer is looked up by its content.
std::unordered_map<HANDLE, std::string> g_fx_data;
std::unordered_map<uint64_t, std::string> g_fx_by_content;   // (hash ^ size) -> file name
thread_local EntryRef t_last = {};
thread_local char t_last_fx[200] = {};
thread_local double t_last_fx_t = 0;
thread_local const BigEntry* t_last_logged = nullptr;
volatile LONG g_open_ok = 0, g_open_fail = 0, g_exists_calls = 0, g_scans = 0;

decltype(&CreateFileW) o_CreateFileW;
decltype(&CreateFileA) o_CreateFileA;
decltype(&ReadFile) o_ReadFile;
decltype(&CloseHandle) o_CloseHandle;
decltype(&FindFirstFileW) o_FindFirstFileW;
decltype(&FindFirstFileA) o_FindFirstFileA;
decltype(&FindNextFileW) o_FindNextFileW;
decltype(&FindClose) o_FindClose;
decltype(&GetFileAttributesW) o_GetFileAttributesW;
decltype(&CreateFileMappingW) o_CreateFileMappingW;
decltype(&MapViewOfFile) o_MapViewOfFile;

uint64_t fnv1a(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h ^ (uint64_t(n) << 40);
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], DWORD(s.size()));
    return s;
}

bool is_big(const wchar_t* p) {
    size_t n = wcslen(p);
    return n > 4 && _wcsicmp(p + n - 4, L".big") == 0;
}

const char* strip_dot(const std::string& s) {
    return (s.size() > 2 && s[0] == '.' && (s[1] == '\\' || s[1] == '/')) ? s.c_str() + 2 : s.c_str();
}

// Archive label for logs: path below the BIGS folder ("vinci_units.big", "patches\patch7\art.big").
std::string big_label(const std::wstring& full) {
    std::wstring low = lower(full);
    size_t at = low.rfind(L"\\bigs\\");
    if (at != std::wstring::npos) return w2u(full.c_str() + at + 6);
    const wchar_t* slash = wcsrchr(full.c_str(), L'\\');
    return w2u(slash ? slash + 1 : full.c_str());
}

// Reads the archive header with rolhook's own (unhooked) kernel32 imports.
BigInfo* parse_big(const std::wstring& full) {
    auto* bi = new BigInfo();
    bi->path = rel_path(full.c_str());
    bi->base = big_label(full);

    HANDLE f = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return bi;
    LARGE_INTEGER sz = {};
    GetFileSizeEx(f, &sz);
    bi->size = uint64_t(sz.QuadPart);
    DWORD want = DWORD(std::min<uint64_t>(bi->size, 16u << 20));
    std::vector<uint8_t> buf(want ? want : 1);
    DWORD got = 0;
    ReadFile(f, buf.data(), want, &got, nullptr);
    CloseHandle(f);

    size_t p = 0;
    auto need = [&](size_t n) { return p + n <= got; };
    auto u8 = [&]() { return uint32_t(buf[p++]); };
    auto u16 = [&]() { uint32_t v = buf[p] | (uint32_t(buf[p + 1]) << 8); p += 2; return v; };
    auto u32 = [&]() { uint32_t v; memcpy(&v, &buf[p], 4); p += 4; return v; };
    auto wstr = [&](std::string* out) {
        if (!need(4)) return false;
        uint32_t n = u32();
        if (n > 4096 || !need(size_t(n) * 2)) return false;
        if (out) {
            std::wstring w(n, L'\0');
            memcpy(&w[0], &buf[p], size_t(n) * 2);
            *out = w2u(w.c_str());
        }
        p += size_t(n) * 2;
        return true;
    };

    // header: u32 version (11), u8 flag, wstr builder machine name (not checked by the engine)
    std::string builder;
    if (!need(5)) return bi;
    uint32_t version = u32();
    u8();
    if (version != 11 || !wstr(&builder) || builder.empty()) {
        RLOG(CH_BIGS, L_WARN, "%s: unknown archive header (version %u)", bi->path.c_str(), version);
        return bi;
    }
    if (builder != "WAR-BUILDER") RLOG(CH_BIGS, L_DEBUG, "%s: built on %s", bi->path.c_str(), builder.c_str());
    if (!need(4)) return bi;
    uint32_t ndeps = u32();
    if (ndeps) {
        if (!need(7)) return bi;
        u32(); u16(); u8();
        for (uint32_t i = 0; i < ndeps; ++i) {
            if (!wstr(nullptr) || !need(4)) return bi;
            u32();
        }
    }
    if (!need(4)) return bi;
    uint32_t n = u32();
    if (n) {
        if (!need(7)) return bi;
        u32(); u16(); u8();
    }
    bi->ents.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        BigEntry e;
        if (!wstr(&e.name) || !need(24)) return bi;
        u32(); u32();
        e.off = u32();
        e.usize = u32();
        u32(); u32();
        if (!wstr(nullptr) || !need(2)) return bi;
        u8(); u8();
        bi->ents.push_back(std::move(e));
    }
    std::sort(bi->ents.begin(), bi->ents.end(), [](const BigEntry& a, const BigEntry& b) { return a.off < b.off; });
    for (size_t i = 0; i < bi->ents.size(); ++i)
        bi->ents[i].end = (i + 1 < bi->ents.size()) ? bi->ents[i + 1].off : uint32_t(bi->size);
    bi->parsed = true;
    return bi;
}

const BigEntry* find_entry(const BigInfo* bi, uint64_t pos) {
    const auto& v = bi->ents;
    auto it = std::upper_bound(v.begin(), v.end(), pos, [](uint64_t q, const BigEntry& e) { return q < e.off; });
    if (it == v.begin()) return nullptr;
    --it;
    return pos < it->end ? &*it : nullptr;
}

void on_open(const wchar_t* path, DWORD acc, HANDLE h, DWORD err, const char* api) {
    if (!path) return;
    const char* mode = (acc & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) ? ((acc & GENERIC_READ) ? "RW" : "W") : "R";
    if (h == INVALID_HANDLE_VALUE) {
        InterlockedIncrement(&g_open_fail);
        RLOG(CH_FILES, L_DEBUG, "%s FAIL %-2s err %lu  %s", api, mode, err, rel_path(path).c_str());
        return;
    }
    InterlockedIncrement(&g_open_ok);
    if (!is_big(path)) {
        RLOG(CH_FILES, L_INFO, "%s ok   %-2s %s", api, mode, rel_path(path).c_str());
        const wchar_t* dot = wcsrchr(path, L'.');
        if (dot && (!_wcsicmp(dot, L".fxv") || !_wcsicmp(dot, L".fxo") || !_wcsicmp(dot, L".fx") || !_wcsicmp(dot, L".fxh"))) {
            const wchar_t* base = wcsrchr(path, L'\\');
            AcquireSRWLockExclusive(&g_lock);
            g_fx_handles[h] = w2u(base ? base + 1 : path);
            ReleaseSRWLockExclusive(&g_lock);
        }
        return;
    }
    wchar_t full[MAX_PATH * 2] = {};
    GetFullPathNameW(path, MAX_PATH * 2, full, nullptr);
    std::wstring key = lower(full);

    AcquireSRWLockShared(&g_lock);
    auto it = g_bigs.find(key);
    BigInfo* bi = it == g_bigs.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);

    bool first = false;
    if (!bi) {
        BigInfo* fresh = parse_big(full);
        AcquireSRWLockExclusive(&g_lock);
        auto ins = g_bigs.emplace(key, fresh);
        if (!ins.second) delete fresh;
        bi = ins.first->second;
        first = ins.second;
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockExclusive(&g_lock);
    bi->opens++;
    g_handles[h] = bi;
    ReleaseSRWLockExclusive(&g_lock);

    if (first)
        RLOG(CH_BIGS, L_INFO, "archive %-26s %5u entries %8.1f MB  %s", bi->base.c_str(), unsigned(bi->ents.size()),
             bi->size / 1048576.0, bi->path.c_str());
    else
        RLOG(CH_BIGS, L_DEBUG, "archive %s reopened (open #%u)", bi->base.c_str(), bi->opens);
    RLOG(CH_FILES, L_DEBUG, "%s ok   %-2s %s", api, mode, rel_path(path).c_str());
}

// [test] pristine_bigs: these archives of BIGS\ are read from modkit\backup\BIGS\ — the originals modmgr keeps before
// a mod patches an archive — so a test run can leave out the archive patches of the enabled mods.
std::wstring pristine_path(const wchar_t* p) {
    if (g_cfg.pristine.empty() || !p || !is_big(p)) return {};
    wchar_t full[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(p, MAX_PATH * 2, full, nullptr)) return {};
    const wchar_t* base = wcsrchr(full, L'\\');
    if (!base) return {};
    std::wstring dir(full, size_t(base + 1 - full));
    if (_wcsicmp(dir.c_str(), (g_cfg.game_dir + L"BIGS\\").c_str()) != 0) return {};   // not BIGS\patches\...
    ++base;
    for (const auto& n : g_cfg.pristine) {
        if (_wcsicmp(n.c_str(), base) != 0) continue;
        std::wstring alt = g_cfg.game_dir + L"modkit\\backup\\BIGS\\" + base;
        if (GetFileAttributesW(alt.c_str()) == INVALID_FILE_ATTRIBUTES) {
            RLOG(CH_BIGS, L_WARN, "test: no original of %s in modkit\\backup\\BIGS, the installed one is read",
                 w2u(base).c_str());
            return {};
        }
        RLOG(CH_BIGS, L_INFO, "test: BIGS\\%s read from modkit\\backup\\BIGS (the original, without mod patches)",
             w2u(base).c_str());
        return alt;
    }
    return {};
}

// [ui] layout_scale (ui_scale mod): a game file opened for reading is read from <layout_overlay>\<scale>\<same path>
// when the mod built one there — BIGS\ui.big, BIGS\multiplayer_data.big (layouts scaled). The first file served
// switches on the engine side (uilayout.cpp: fonts, sprites).
std::wstring overlay_path(const wchar_t* p, DWORD acc) {
    if (g_cfg.ui_layout_overlay.empty() || !p) return {};
    if (acc & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) return {};
    wchar_t full[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(p, MAX_PATH * 2, full, nullptr)) return {};
    const std::wstring& gd = g_cfg.game_dir;
    if (_wcsnicmp(full, gd.c_str(), gd.size()) != 0) return {};
    const wchar_t* rel = full + gd.size();
    if (!_wcsnicmp(rel, L"modkit\\", 7)) return {};
    std::wstring alt = g_cfg.ui_layout_overlay + rel;
    if (GetFileAttributesW(alt.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    static std::vector<std::wstring> logged;   // once per file
    std::wstring key(rel);
    for (auto& c : key) c = towlower(c);
    AcquireSRWLockExclusive(&g_lock);
    const bool fresh = std::find(logged.begin(), logged.end(), key) == logged.end();
    if (fresh) logged.push_back(key);
    ReleaseSRWLockExclusive(&g_lock);
    if (fresh)
        RLOG(CH_CORE, L_INFO, "interface scale %d%%: %s read from %s", g_cfg.ui_layout_scale, w2u(rel).c_str(),
             rel_path(alt.c_str()).c_str());
    uilayout_overlay_used();
    return alt;
}

HANDLE WINAPI h_CreateFileW(LPCWSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tmpl) {
    std::wstring alt = pristine_path(p);
    if (alt.empty()) alt = overlay_path(p, acc);
    if (!alt.empty()) p = alt.c_str();
    HANDLE h = o_CreateFileW(p, acc, share, sa, disp, fl, tmpl);
    DWORD err = GetLastError();
    on_open(p, acc, h, err, "open ");
    SetLastError(err);
    return h;
}

HANDLE WINAPI h_CreateFileA(LPCSTR p, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD fl, HANDLE tmpl) {
    std::wstring w = a2w(p);
    std::wstring alt = pristine_path(w.c_str());
    if (alt.empty()) alt = overlay_path(w.c_str(), acc);
    HANDLE h = alt.empty() ? o_CreateFileA(p, acc, share, sa, disp, fl, tmpl)
                           : o_CreateFileW(alt.c_str(), acc, share, sa, disp, fl, tmpl);
    DWORD err = GetLastError();
    on_open(alt.empty() ? w.c_str() : alt.c_str(), acc, h, err, "openA");
    SetLastError(err);
    return h;
}

// MSXML. Some XML the game loads by path through MSXML 4.0 (c:\rts2\main\basic\helpxml.cpp: CoCreateInstance of
// DOMDocument40 at 0x00D76F83 and four more places, then IXMLDOMDocument::load with a VT_BSTR path, vtable +0xE8),
// the campaign files (campaigns\<nation>\<nation>.campaign) among them. MSXML opens the file itself, past the hooks
// above, so load() is given the same overlay: when the game creates an MSXML document, the load slot of the
// document's vtable is replaced (once per vtable).
decltype(&CoCreateInstance) o_CoCreateInstance;
typedef HRESULT(__stdcall* DomLoad_t)(void* self, VARIANT src, VARIANT_BOOL* ok);
constexpr int kDomLoadSlot = 0xE8 / 4;   // IUnknown 3 + IDispatch 4 + IXMLDOMNode 36 + IXMLDOMDocument's 16th
struct DomVtable {
    void** vt;
    DomLoad_t load;
};
DomVtable g_dom[4];
int g_dom_count = 0;
volatile LONG g_dom_redirects = 0;
// MSXML document classes: DOMDocument26/30/40/60 and their free-threaded twins
const GUID kDomClasses[] = {
    {0xF5078F1B, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},
    {0xF5078F1C, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},
    {0xF5078F32, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},
    {0xF5078F33, 0xC551, 0x11D3, {0x89, 0xB9, 0x00, 0x00, 0xF8, 0x1F, 0xE2, 0x21}},
    {0x88D969C0, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},
    {0x88D969C1, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},
    {0x88D96A05, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},
    {0x88D96A06, 0xF192, 0x11D4, {0xA6, 0x5F, 0x00, 0x40, 0x96, 0x32, 0x51, 0xE5}},
};

DomLoad_t dom_original(void* self) {
    void** vt = *static_cast<void***>(self);
    for (int i = 0; i < g_dom_count; ++i)
        if (g_dom[i].vt == vt) return g_dom[i].load;
    return nullptr;
}

HRESULT __stdcall h_dom_load(void* self, VARIANT src, VARIANT_BOOL* ok) {
    AcquireSRWLockShared(&g_lock);
    DomLoad_t orig = dom_original(self);
    ReleaseSRWLockShared(&g_lock);
    if (!orig) return E_FAIL;   // cannot happen: only patched vtables lead here
    if (src.vt == VT_BSTR && src.bstrVal) {
        std::wstring alt = overlay_path(src.bstrVal, GENERIC_READ);
        if (!alt.empty()) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_BSTR;
            v.bstrVal = SysAllocString(alt.c_str());
            if (v.bstrVal) {
                InterlockedIncrement(&g_dom_redirects);
                RLOG(CH_FILES, L_INFO, "msxml load %s from %s", w2u(src.bstrVal).c_str(), rel_path(alt.c_str()).c_str());
                HRESULT hr = orig(self, v, ok);
                SysFreeString(v.bstrVal);
                return hr;
            }
        }
    }
    return orig(self, src, ok);
}

void hook_dom_vtable(void* obj) {
    void** vt = *static_cast<void***>(obj);
    AcquireSRWLockExclusive(&g_lock);
    bool known = false;
    for (int i = 0; i < g_dom_count; ++i) known |= g_dom[i].vt == vt;
    DWORD old;
    if (!known && g_dom_count < int(sizeof(g_dom) / sizeof(g_dom[0])) && vt[kDomLoadSlot] != (void*)h_dom_load &&
        VirtualProtect(&vt[kDomLoadSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        g_dom[g_dom_count++] = {vt, reinterpret_cast<DomLoad_t>(vt[kDomLoadSlot])};
        vt[kDomLoadSlot] = (void*)h_dom_load;
        VirtualProtect(&vt[kDomLoadSlot], sizeof(void*), old, &old);
        known = true;
        RLOG(CH_CORE, L_INFO, "msxml: document vtable %p hooked (load at slot %d), overlay applies to MSXML loads", vt,
             kDomLoadSlot);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

HRESULT WINAPI h_CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out) {
    HRESULT hr = o_CoCreateInstance(clsid, outer, ctx, iid, out);
    if (SUCCEEDED(hr) && out && *out) {
        for (const GUID& g : kDomClasses)
            if (IsEqualGUID(clsid, g)) {
                hook_dom_vtable(*out);
                break;
            }
    }
    return hr;
}

BOOL WINAPI h_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD rd, LPOVERLAPPED ov) {
    BigInfo* bi = nullptr;
    AcquireSRWLockShared(&g_lock);
    if (!g_handles.empty()) {
        auto it = g_handles.find(h);
        if (it != g_handles.end()) bi = it->second;
    }
    bool fx_file = false;
    if (!bi && !g_fx_handles.empty()) {
        auto fx = g_fx_handles.find(h);
        if (fx != g_fx_handles.end()) {
            strncpy_s(t_last_fx, fx->second.c_str(), _TRUNCATE);
            t_last_fx_t = now_ms();
            fx_file = true;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (fx_file) {   // keep what it gives: the effect made from it is named by its content
        BOOL ok = o_ReadFile(h, buf, n, rd, ov);
        DWORD err = GetLastError();
        const DWORD got = (ok && rd) ? *rd : 0;
        if (got && got <= (64u << 20)) {
            AcquireSRWLockExclusive(&g_lock);
            g_fx_data[h].append(static_cast<const char*>(buf), got);
            ReleaseSRWLockExclusive(&g_lock);
        }
        SetLastError(err);
        return ok;
    }
    if (!bi) return o_ReadFile(h, buf, n, rd, ov);

    uint64_t pos;
    if (ov) {
        pos = ov->Offset | (uint64_t(ov->OffsetHigh) << 32);
    } else {
        LONG hi = 0;
        DWORD lo = SetFilePointer(h, 0, &hi, FILE_CURRENT);
        pos = lo | (uint64_t(DWORD(hi)) << 32);
    }
    BOOL ok = o_ReadFile(h, buf, n, rd, ov);
    DWORD err = GetLastError();
    bi->reads++;
    bi->bytes += n;
    const BigEntry* e = bi->parsed ? find_entry(bi, pos) : nullptr;
    if (e) {
        strncpy_s(t_last.archive, bi->base.c_str(), _TRUNCATE);
        strncpy_s(t_last.name, strip_dot(e->name), _TRUNCATE);
        t_last.usize = e->usize;
        t_last.t = now_ms();
        if (t_last_logged != e) {
            t_last_logged = e;
            RLOG(CH_BIGS, L_DEBUG, "read %s :: %s  (%u bytes unpacked)", bi->base.c_str(), strip_dot(e->name), e->usize);
        }
    } else {
        RLOG(CH_BIGS, L_TRACE, "read %s header @%llu len %lu", bi->base.c_str(), pos, n);
    }
    if (!ok) RLOG(CH_BIGS, L_WARN, "read FAILED %s @%llu len %lu err %lu", bi->base.c_str(), pos, n, err);
    SetLastError(err);
    return ok;
}

BOOL WINAPI h_CloseHandle(HANDLE h) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_handles.empty()) g_handles.erase(h);
    if (!g_fx_handles.empty()) {
        auto fx = g_fx_handles.find(h);
        if (fx != g_fx_handles.end()) {
            auto data = g_fx_data.find(h);
            if (data != g_fx_data.end()) {
                if (!data->second.empty()) g_fx_by_content[fnv1a(data->second.data(), data->second.size())] = fx->second;
                g_fx_data.erase(data);
            }
            g_fx_handles.erase(fx);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return o_CloseHandle(h);
}

void scan_begin(HANDLE h, const std::string& pattern, const wchar_t* first, DWORD err) {
    InterlockedIncrement(&g_scans);
    if (h == INVALID_HANDLE_VALUE) {
        RLOG(CH_FILES, L_INFO, "scan %s -> no matches (err %lu)", pattern.c_str(), err);
        return;
    }
    RLOG(CH_FILES, L_DEBUG, "scan %s -> %s", pattern.c_str(), w2u(first).c_str());
    AcquireSRWLockExclusive(&g_lock);
    g_finds[h] = FindState{pattern, 1};
    ReleaseSRWLockExclusive(&g_lock);
}

HANDLE WINAPI h_FindFirstFileW(LPCWSTR pat, LPWIN32_FIND_DATAW fd) {
    HANDLE h = o_FindFirstFileW(pat, fd);
    DWORD err = GetLastError();
    scan_begin(h, rel_path(pat), h != INVALID_HANDLE_VALUE ? fd->cFileName : L"", err);
    SetLastError(err);
    return h;
}

HANDLE WINAPI h_FindFirstFileA(LPCSTR pat, LPWIN32_FIND_DATAA fd) {
    HANDLE h = o_FindFirstFileA(pat, fd);
    DWORD err = GetLastError();
    std::wstring wp = a2w(pat);
    std::wstring wf = h != INVALID_HANDLE_VALUE ? a2w(fd->cFileName) : std::wstring();
    scan_begin(h, rel_path(wp.c_str()), wf.c_str(), err);
    SetLastError(err);
    return h;
}

BOOL WINAPI h_FindNextFileW(HANDLE h, LPWIN32_FIND_DATAW fd) {
    BOOL ok = o_FindNextFileW(h, fd);
    DWORD err = GetLastError();
    if (ok) {
        std::string pattern;
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_finds.find(h);
        if (it != g_finds.end()) { it->second.count++; pattern = it->second.pattern; }
        ReleaseSRWLockExclusive(&g_lock);
        if (!pattern.empty()) RLOG(CH_FILES, L_DEBUG, "scan %s -> %s", pattern.c_str(), w2u(fd->cFileName).c_str());
    }
    SetLastError(err);
    return ok;
}

BOOL WINAPI h_FindClose(HANDLE h) {
    FindState st;
    bool tracked = false;
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_finds.find(h);
    if (it != g_finds.end()) { st = it->second; tracked = true; g_finds.erase(it); }
    ReleaseSRWLockExclusive(&g_lock);
    if (tracked) RLOG(CH_FILES, L_INFO, "scan %s: %d match(es)", st.pattern.c_str(), st.count);
    return o_FindClose(h);
}

DWORD WINAPI h_GetFileAttributesW(LPCWSTR p) {
    DWORD a = o_GetFileAttributesW(p);
    DWORD err = GetLastError();
    InterlockedIncrement(&g_exists_calls);
    RLOG(CH_FILES, L_DEBUG, "exists? %s -> %s", rel_path(p).c_str(),
         a == INVALID_FILE_ATTRIBUTES ? "no" : (a & FILE_ATTRIBUTE_DIRECTORY) ? "dir" : "file");
    SetLastError(err);
    return a;
}

HANDLE WINAPI h_CreateFileMappingW(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name) {
    HANDLE m = o_CreateFileMappingW(f, sa, prot, hi, lo, name);
    DWORD err = GetLastError();
    if (m && on(CH_FILES, L_DEBUG)) {
        wchar_t path[MAX_PATH * 2] = L"(pagefile)";
        if (f && f != INVALID_HANDLE_VALUE) GetFinalPathNameByHandleW(f, path, MAX_PATH * 2, 0);
        const wchar_t* shown = wcsncmp(path, L"\\\\?\\", 4) == 0 ? path + 4 : path;
        RLOG(CH_FILES, L_DEBUG, "mapping %s size %lu:%lu name %s", rel_path(shown).c_str(), hi, lo,
             name ? w2u(name).c_str() : "-");
    }
    SetLastError(err);
    return m;
}

LPVOID WINAPI h_MapViewOfFile(HANDLE m, DWORD acc, DWORD hi, DWORD lo, SIZE_T n) {
    LPVOID v = o_MapViewOfFile(m, acc, hi, lo, n);
    DWORD err = GetLastError();
    RLOG(CH_FILES, L_TRACE, "map view %p off %lu:%lu len %Iu -> %p", m, hi, lo, n, v);
    SetLastError(err);
    return v;
}

}  // namespace

const EntryRef* last_entry() { return t_last.usize ? &t_last : nullptr; }

const char* last_fx_file() { return (t_last_fx[0] && now_ms() - t_last_fx_t < 10000.0) ? t_last_fx : nullptr; }

// The effects come from fxobjects.big (compiled .fxo, zlib-packed; the .fxv in the shader cache are 40-104-byte
// stubs): their unpacked size names most of them - 44 of 54 sizes in a battle are one entry's, the rest the same
// effect's two quality variants (_0, _2) or two effects. Every fxobjects.big opened (the base one and patches\patch7's)
// is asked; names that differ are listed with "/" (the frame captures separate their fields with " | ").
bool fx_entry_of_size(size_t size, char* out, size_t cap) {
    out[0] = 0;
    std::vector<std::string> names;
    AcquireSRWLockShared(&g_lock);
    for (const auto& kv : g_bigs) {
        const BigInfo* bi = kv.second;
        if (!bi || !bi->parsed || _stricmp(bi->base.c_str(), "fxobjects.big") != 0) continue;
        for (const BigEntry& e : bi->ents) {
            if (e.usize != size) continue;
            const char* n = strip_dot(e.name);
            const char* slash = strrchr(n, '\\');
            std::string base = slash ? slash + 1 : n;
            if (std::find(names.begin(), names.end(), base) == names.end()) names.push_back(base);
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (names.empty()) return false;
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) joined += (i ? "/" : "") + names[i];   // no " | ": the captures use it
    strncpy_s(out, cap, joined.c_str(), _TRUNCATE);
    return true;
}

bool fx_file_of(const void* data, size_t size, char* out, size_t cap) {
    if (!data || !size) return false;
    const uint64_t key = fnv1a(data, size);
    bool found = false;
    AcquireSRWLockShared(&g_lock);
    auto it = g_fx_by_content.find(key);
    if (it != g_fx_by_content.end()) {
        strncpy_s(out, cap, it->second.c_str(), _TRUNCATE);
        found = true;
    } else {   // a file still open
        for (const auto& kv : g_fx_data) {
            if (kv.second.size() != size || memcmp(kv.second.data(), data, size) != 0) continue;
            auto name = g_fx_handles.find(kv.first);
            if (name != g_fx_handles.end()) {
                strncpy_s(out, cap, name->second.c_str(), _TRUNCATE);
                found = true;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

void files_install(HMODULE exe) {
    struct Hook { const char* fn; void* hook; void** orig; } hooks[] = {
        {"CreateFileW", (void*)h_CreateFileW, (void**)&o_CreateFileW},
        {"CreateFileA", (void*)h_CreateFileA, (void**)&o_CreateFileA},
        {"ReadFile", (void*)h_ReadFile, (void**)&o_ReadFile},
        {"CloseHandle", (void*)h_CloseHandle, (void**)&o_CloseHandle},
        {"FindFirstFileW", (void*)h_FindFirstFileW, (void**)&o_FindFirstFileW},
        {"FindFirstFileA", (void*)h_FindFirstFileA, (void**)&o_FindFirstFileA},
        {"FindNextFileW", (void*)h_FindNextFileW, (void**)&o_FindNextFileW},
        {"FindClose", (void*)h_FindClose, (void**)&o_FindClose},
        {"GetFileAttributesW", (void*)h_GetFileAttributesW, (void**)&o_GetFileAttributesW},
        {"CreateFileMappingW", (void*)h_CreateFileMappingW, (void**)&o_CreateFileMappingW},
        {"MapViewOfFile", (void*)h_MapViewOfFile, (void**)&o_MapViewOfFile},
    };
    int ok = 0;
    for (auto& h : hooks) {
        if (patch_iat(exe, "KERNEL32.dll", h.fn, h.hook, h.orig)) ++ok;
        else RLOG(CH_CORE, L_WARN, "files: %s is not imported by the host, not traced", h.fn);
    }
    RLOG(CH_CORE, L_INFO, "files: %d/%d file APIs hooked", ok, int(sizeof(hooks) / sizeof(hooks[0])));
    if (!g_cfg.ui_layout_overlay.empty()) {
        const bool com = patch_iat(exe, "ole32.dll", "CoCreateInstance", (void*)h_CoCreateInstance,
                                   (void**)&o_CoCreateInstance);
        RLOG(CH_CORE, com ? L_INFO : L_WARN, "files: MSXML loads %s", com ? "follow the overlay (CoCreateInstance hooked)"
                                                                          : "NOT redirected (CoCreateInstance not imported)");
    }
}

void files_summary() {
    RLOG(CH_FILES, L_INFO, "summary: %ld opens ok, %ld failed, %ld existence checks, %ld directory scans, %ld MSXML "
         "loads from the overlay", g_open_ok, g_open_fail, g_exists_calls, g_scans, g_dom_redirects);
    std::vector<BigInfo*> v;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : g_bigs) v.push_back(kv.second);
    ReleaseSRWLockShared(&g_lock);
    std::sort(v.begin(), v.end(), [](BigInfo* a, BigInfo* b) { return a->bytes > b->bytes; });
    RLOG(CH_BIGS, L_INFO, "summary: %u archives opened", unsigned(v.size()));
    for (BigInfo* b : v)
        RLOG(CH_BIGS, L_INFO, "  %-26s opens %3u reads %7llu  read %8.1f MB of %8.1f MB", b->base.c_str(), b->opens,
             b->reads, b->bytes / 1048576.0, b->size / 1048576.0);
}

}  // namespace rol
