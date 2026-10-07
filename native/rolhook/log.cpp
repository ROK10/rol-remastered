// Logger: per-channel files + all.log, levels from rolhook.ini, buffered with periodic flush.
#include "rolhook.h"
#include "uiauto.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <share.h>
#include <vector>

namespace rol {

Config g_cfg;
volatile int g_level[CH_COUNT] = {L_INFO, L_INFO, L_INFO, L_INFO, L_INFO, L_INFO, L_INFO, L_INFO, L_INFO};
volatile LONG g_frame = 0;

static const char* const kChan[CH_COUNT] = {"core", "files", "bigs", "textures", "d3d", "frame", "memory", "ui",
                                            "probe"};
static const char* const kLvl[] = {"OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

namespace {

struct Sink {
    FILE* f = nullptr;
    std::string buf;
};

Sink g_sink[CH_COUNT];
Sink g_all;
CRITICAL_SECTION g_cs;
bool g_ready = false;
LARGE_INTEGER g_freq, g_t0;

void flush_sink(Sink& s) {
    if (s.f && !s.buf.empty()) {
        fwrite(s.buf.data(), 1, s.buf.size(), s.f);
        fflush(s.f);
    }
    s.buf.clear();
}

void flush_locked() {
    for (auto& s : g_sink) flush_sink(s);
    flush_sink(g_all);
}

std::wstring trim(std::wstring s) {
    size_t c = s.find(L';');
    if (c != std::wstring::npos) s.resize(c);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && iswspace(s[b])) ++b;
    return s.substr(b);
}

// Layered config: rolhook.ini first, then every rolhook.d\*.ini in name order; a later file overrides a key.
// Each mod of the modpack drops its own fragment into rolhook.d, so disabling a mod = deleting its fragment.
// Last comes the file named by the ROLHOOK_EXTRA_INI environment variable (absolute or relative to the game
// folder): the automated test run (modkit\tools\autotest.py) sets it for the game it starts, so its settings never
// touch the files of an ordinary start.
std::vector<std::wstring> g_ini_files;

std::wstring extra_ini(const std::wstring& dir) {
    wchar_t v[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"ROLHOOK_EXTRA_INI", v, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return {};
    std::wstring p(v);
    if (p.size() < 2 || (p[1] != L':' && p[0] != L'\\')) p = dir + p;
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : p;
}

void collect_ini_files(const std::wstring& dir) {
    g_ini_files.assign(1, dir + L"rolhook.ini");
    std::vector<std::wstring> frags;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"rolhook.d\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) frags.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(frags.begin(), frags.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const auto& n : frags) g_ini_files.push_back(dir + L"rolhook.d\\" + n);
    g_cfg.extra_ini = extra_ini(dir);
    if (!g_cfg.extra_ini.empty()) g_ini_files.push_back(g_cfg.extra_ini);
}

// "10, 20.5, 30" -> {10, 20.5, 30}
std::vector<double> parse_list(const std::wstring& s) {
    std::vector<double> v;
    const wchar_t* p = s.c_str();
    while (*p) {
        wchar_t* end = nullptr;
        double x = wcstod(p, &end);
        if (end == p) { ++p; continue; }
        v.push_back(x);
        p = end;
    }
    return v;
}

// "data.big, multiplayer_data.big" -> {"data.big", "multiplayer_data.big"}
std::vector<std::wstring> parse_names(const std::wstring& s) {
    std::vector<std::wstring> v;
    size_t b = 0;
    while (b <= s.size()) {
        size_t e = s.find(L',', b);
        if (e == std::wstring::npos) e = s.size();
        std::wstring t = trim(s.substr(b, e - b));
        if (!t.empty()) v.push_back(t);
        b = e + 1;
    }
    return v;
}

// The options tab's layer, read last; written by user_ini_set().
std::wstring user_ini_path() { return g_cfg.game_dir + L"rolhook.d\\zz_options.ini"; }

std::wstring ini_str_ex(const wchar_t* sec, const wchar_t* key, const wchar_t* def, bool include_user) {
    std::wstring val = def;
    const std::wstring user = user_ini_path();
    for (const auto& f : g_ini_files) {
        if (!include_user && !_wcsicmp(f.c_str(), user.c_str())) continue;
        wchar_t buf[512];
        GetPrivateProfileStringW(sec, key, L"\x01", buf, 512, f.c_str());
        if (wcscmp(buf, L"\x01") != 0) val = trim(buf);
    }
    return val;
}

std::wstring ini_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    return ini_str_ex(sec, key, def, true);
}

int ini_int(const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring v = ini_str(sec, key, L"");
    return v.empty() ? def : _wtoi(v.c_str());
}

int parse_level(const std::wstring& v) {
    static const wchar_t* names[] = {L"off", L"error", L"warn", L"info", L"debug", L"trace"};
    for (int i = 0; i <= L_TRACE; ++i)
        if (!_wcsicmp(v.c_str(), names[i])) return i;
    int n = _wtoi(v.c_str());
    return (n >= L_OFF && n <= L_TRACE && !v.empty()) ? n : L_INFO;
}

bool make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/')
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int clamp_or_zero(int v, int lo, int hi) { return v <= 0 ? 0 : std::min(hi, std::max(lo, v)); }

}  // namespace

int config_int(const wchar_t* sec, const wchar_t* key, int def, bool include_user) {
    std::wstring v = ini_str_ex(sec, key, L"", include_user);
    return v.empty() ? def : _wtoi(v.c_str());
}

std::wstring config_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def) { return ini_str(sec, key, def); }

// A key setting: F1..F24, ScrollLock, Num0..Num9, Num*, Num/, Num., a virtual-key code (decimal or 0x..), or off /
// 0 / empty; def when no file sets it. The game binds all F keys, Pause, Insert and Num+/Num- (data\keymap.xml).
int config_key(const wchar_t* sec, const wchar_t* key, int def) {
    std::wstring v = ini_str(sec, key, L"\x01");
    if (v == L"\x01") return def;
    if (v.empty() || !_wcsicmp(v.c_str(), L"off") || v == L"0") return 0;
    if ((v[0] == L'F' || v[0] == L'f') && _wtoi(v.c_str() + 1) >= 1 && _wtoi(v.c_str() + 1) <= 24)
        return VK_F1 + _wtoi(v.c_str() + 1) - 1;
    if (!_wcsicmp(v.c_str(), L"ScrollLock")) return VK_SCROLL;
    if (!_wcsicmp(v.c_str(), L"Num*")) return VK_MULTIPLY;
    if (!_wcsicmp(v.c_str(), L"Num/")) return VK_DIVIDE;
    if (!_wcsicmp(v.c_str(), L"Num.")) return VK_DECIMAL;
    if (v.size() == 4 && !_wcsnicmp(v.c_str(), L"Num", 3) && iswdigit(v[3])) return VK_NUMPAD0 + (v[3] - L'0');
    return int(wcstol(v.c_str(), nullptr, 0));
}

void user_ini_set(const wchar_t* sec, const wchar_t* key, int value) {
    const std::wstring path = user_ini_path();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW((g_cfg.game_dir + L"rolhook.d").c_str(), nullptr);
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            static const char head[] =
                "; Written by rolhook from the \"Mods\" tab of the game's options window.\r\n"
                "; Holds only the values that differ from what the enabled mods set; delete it to go back to them.\r\n";
            DWORD n = 0;
            WriteFile(f, head, DWORD(sizeof(head) - 1), &n, nullptr);
            CloseHandle(f);
        }
    }
    wchar_t buf[32];
    swprintf_s(buf, L"%d", value);
    WritePrivateProfileStringW(sec, key, buf, path.c_str());
    bool listed = false;
    for (const auto& f : g_ini_files) listed |= !_wcsicmp(f.c_str(), path.c_str());
    if (!listed) g_ini_files.push_back(path);
}

void user_ini_remove(const wchar_t* sec, const wchar_t* key) {
    const std::wstring path = user_ini_path();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        WritePrivateProfileStringW(sec, key, nullptr, path.c_str());
}

std::string config_sources() {
    std::string s;
    for (const auto& f : g_ini_files) {
        if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (!s.empty()) s += " + ";
        s += rel_path(f.c_str());
    }
    return s.empty() ? "(no config files, defaults)" : s;
}

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

void log_init() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    InitializeCriticalSectionAndSpinCount(&g_cs, 4000);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    g_cfg.game_dir = dir;

    collect_ini_files(dir);
    g_cfg.enabled = ini_int(L"general", L"enabled", 1) != 0;
    std::wstring root = ini_str(L"general", L"log_dir", L"modkit\\logs\\sessions");
    if (root.size() < 2 || root[1] != L':') root = dir + root;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    g_cfg.log_root = root;
    g_cfg.flush_ms = std::max(50, ini_int(L"general", L"flush_ms", 250));
    g_cfg.assert_ignore = _wcsicmp(ini_str(L"general", L"assert_mode", L"ignore").c_str(), L"ask") != 0;
    g_cfg.crash_dump = ini_int(L"general", L"crash_dump", 1) != 0;
    g_cfg.fix_lockrect = ini_int(L"general", L"fix_lockrect", 0) != 0;
    g_cfg.frame_interval_s = std::max(1, ini_int(L"general", L"frame_interval", 1));
    g_cfg.memory_interval_s = std::max(1, ini_int(L"general", L"memory_interval", 5));
    for (int i = 0; i < CH_COUNT; ++i) {
        std::wstring key(kChan[i], kChan[i] + strlen(kChan[i]));
        g_level[i] = parse_level(ini_str(L"levels", key.c_str(), L"info"));
    }
    g_cfg.anisotropy = std::min(16, std::max(0, ini_int(L"graphics", L"anisotropy", 0)));
    g_cfg.zoom_in = clamp_or_zero(ini_int(L"camera", L"zoom_in", 0), 300, 20000);
    g_cfg.zoom_out = clamp_or_zero(ini_int(L"camera", L"zoom_out", 0), 1000, 40000);
    g_cfg.zoom_steps = clamp_or_zero(ini_int(L"camera", L"zoom_steps", 0), 2, 40);
    g_cfg.zoom_spring = std::min(3, std::max(0, ini_int(L"camera", L"zoom_spring", 3)));
    g_cfg.atoc = std::min(2, std::max(0, ini_int(L"graphics", L"atoc", 0)));
    g_cfg.lod_thresholds = std::min(200, std::max(0, ini_int(L"graphics", L"lod_thresholds", 100)));
    g_cfg.borderless = ini_int(L"graphics", L"borderless", 0) != 0;
    g_cfg.shadow_linear = !_wcsicmp(ini_str(L"graphics", L"shadow_filter", L"point").c_str(), L"linear");
    {
        int s = ini_int(L"graphics", L"shadow_size", 0);   // a power of two 512..16384, or 0
        g_cfg.shadow_size = (s >= 512 && s <= 16384 && !(s & (s - 1))) ? s : 0;
    }
    g_cfg.shadow_fit = std::min(2, std::max(0, ini_int(L"graphics", L"shadow_fit", 0)));
    g_cfg.shadow_fit_margin = std::min(200, std::max(5, ini_int(L"graphics", L"shadow_fit_margin", 25)));
    g_cfg.anim_pose_ms = std::min(255, std::max(0, ini_int(L"graphics", L"anim_pose_ms", 0)));
    g_cfg.anim_clock = ini_int(L"graphics", L"anim_clock", 1) != 0 ? 1 : 0;
    g_cfg.soft_shadows = std::min(3, std::max(0, ini_int(L"graphics", L"soft_shadows", 0)));
    g_cfg.ssao = std::min(3, std::max(0, ini_int(L"graphics", L"ssao", 0)));
    g_cfg.gamma_light = ini_int(L"graphics", L"gamma_light", 0) != 0 ? 1 : 0;
    g_cfg.sharpen = std::min(3, std::max(0, ini_int(L"graphics", L"sharpen", 0)));
    g_cfg.vibrance = std::min(3, std::max(0, ini_int(L"graphics", L"vibrance", 0)));
    {   // [ui] layout_scale / layout_overlay (ui_scale mod): game files read from <overlay>\<scale>\ when they exist;
        // layout_scale 0 = auto, the scale for the height the game will run at (uiauto.h)
        const int choice = ini_int(L"ui", L"layout_scale", 100);
        g_cfg.ui_layout_choice = choice <= 0 ? 0 : std::min(300, std::max(100, choice));
        g_cfg.ui_layout_auto.clear();
        g_cfg.ui_layout_scale = g_cfg.ui_layout_choice ? g_cfg.ui_layout_choice
                                                       : ui_auto_scale(ui_screen_height(g_cfg.ui_layout_auto));
        std::wstring o = ini_str(L"ui", L"layout_overlay", L"");
        while (!o.empty() && (o.back() == L'\\' || o.back() == L'/')) o.pop_back();
        g_cfg.ui_layout_overlay.clear();
        if (!o.empty() && g_cfg.ui_layout_scale > 100) {
            const bool absolute = o.size() > 1 && (o[1] == L':' || (o[0] == L'\\' && o[1] == L'\\'));
            g_cfg.ui_layout_overlay = (absolute ? o : g_cfg.game_dir + o) + L"\\" +
                                      std::to_wstring(g_cfg.ui_layout_scale) + L"\\";
        }
    }
    g_cfg.capture_vk = config_key(L"debug", L"capture_key", VK_MULTIPLY);
    g_cfg.capture_at = std::max(0, ini_int(L"debug", L"capture_at", 0));
    g_cfg.shot_vk = config_key(L"debug", L"shot_key", 0);
    g_cfg.test_start_first_frame = !_wcsicmp(ini_str(L"test", L"start", L"match").c_str(), L"first_frame");
    g_cfg.test_shots = parse_list(ini_str(L"test", L"shots", L""));
    std::sort(g_cfg.test_shots.begin(), g_cfg.test_shots.end());
    g_cfg.test_capture = ini_int(L"test", L"capture", 0) != 0;
    {   // "8:6, 10:-2" -> wheel notches at seconds of the test clock
        std::wstring w = ini_str(L"test", L"wheel", L"");
        const wchar_t* p = w.c_str();
        while (*p) {
            wchar_t* end = nullptr;
            double sec = wcstod(p, &end);
            if (end == p) { ++p; continue; }
            p = end;
            if (*p != L':') continue;
            long n = wcstol(p + 1, &end, 10);
            p = end;
            if (n) g_cfg.test_wheel.push_back({sec, int(n)});
        }
    }
    {   // "3:Left:250, 9:Up:100" -> keys held from a second of the test clock for so many ms (Left, Right, Up, Down
        // or a virtual-key code)
        std::wstring w = ini_str(L"test", L"keys", L"");
        size_t b = 0;
        while (b < w.size()) {
            size_t e = w.find(L',', b);
            if (e == std::wstring::npos) e = w.size();
            const std::wstring item = trim(w.substr(b, e - b));
            b = e + 1;
            const size_t c1 = item.find(L':'), c2 = c1 == std::wstring::npos ? c1 : item.find(L':', c1 + 1);
            if (c2 == std::wstring::npos) continue;
            const std::wstring name = trim(item.substr(c1 + 1, c2 - c1 - 1));
            static const struct { const wchar_t* name; int vk; } kNames[] = {
                {L"Left", VK_LEFT}, {L"Right", VK_RIGHT}, {L"Up", VK_UP}, {L"Down", VK_DOWN}};
            int vk = int(wcstol(name.c_str(), nullptr, 0));
            for (const auto& k : kNames)
                if (!_wcsicmp(name.c_str(), k.name)) vk = k.vk;
            const double at = _wtof(item.substr(0, c1).c_str()), ms = _wtof(item.substr(c2 + 1).c_str());
            if (vk > 0 && vk < 256 && ms > 0) g_cfg.test_keys.push_back({at, vk, ms});
        }
        std::sort(g_cfg.test_keys.begin(), g_cfg.test_keys.end(),
                  [](const Config::TestKey& x, const Config::TestKey& y) { return x.at < y.at; });
    }
    g_cfg.test_options = std::max(0.0, _wtof(ini_str(L"test", L"options", L"0").c_str()));
    g_cfg.test_options_tab = ini_str(L"test", L"options_tab", L"");
    g_cfg.test_quit = std::max(0.0, _wtof(ini_str(L"test", L"quit", L"0").c_str()));
    g_cfg.test_timeout = std::max(0.0, _wtof(ini_str(L"test", L"timeout", L"0").c_str()));
    g_cfg.pristine = parse_names(ini_str(L"test", L"pristine_bigs", L""));
    g_cfg.tex_override = ini_int(L"textures", L"override", 0) != 0;
    std::wstring od = ini_str(L"textures", L"override_dir", L"");
    if (!od.empty()) {
        if (od.size() < 2 || od[1] != L':') od = dir + od;
        if (od.back() != L'\\' && od.back() != L'/') od += L'\\';
        g_cfg.tex_override_dir = od;
    }
    if (!g_cfg.enabled) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"%04u-%02u-%02u_%02u-%02u-%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_cfg.session_dir = root + L"\\" + name + L"\\";
    if (!make_dirs(g_cfg.session_dir)) {
        g_cfg.log_root = dir + L"rolhook_logs";
        g_cfg.session_dir = g_cfg.log_root + L"\\" + name + L"\\";
        make_dirs(g_cfg.session_dir);
    }
    // _SH_DENYWR, not _wfopen_s (exclusive): the logs can be read while the game runs
    for (int i = 0; i < CH_COUNT; ++i) {
        if (g_level[i] == L_OFF) continue;
        std::wstring key(kChan[i], kChan[i] + strlen(kChan[i]));
        g_sink[i].f = _wfsopen((g_cfg.session_dir + key + L".log").c_str(), L"wb", _SH_DENYWR);
    }
    g_all.f = _wfsopen((g_cfg.session_dir + L"all.log").c_str(), L"wb", _SH_DENYWR);
    FILE* latest = nullptr;
    if (_wfopen_s(&latest, (g_cfg.log_root + L"\\latest.txt").c_str(), L"wb") == 0 && latest) {
        fputs(w2u(name).c_str(), latest);
        fclose(latest);
    }
    g_ready = true;
}

void logf(int ch, int lvl, const char* fmt, ...) {
    if (!g_ready || ch < 0 || ch >= CH_COUNT || lvl < L_ERROR || lvl > L_TRACE) return;
    char msg[3072];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(msg);
    char head[112];
    int h = _snprintf_s(head, sizeof(head), _TRUNCATE, "[%11.3f] [%5lu] [f%7ld] %-5s %-8s| ", now_ms(),
                        GetCurrentThreadId(), (long)g_frame, kLvl[lvl], kChan[ch]);
    if (h < 0) h = (int)strlen(head);

    EnterCriticalSection(&g_cs);
    for (Sink* s : {&g_sink[ch], &g_all}) {
        s->buf.append(head, h);
        s->buf.append(msg, n);
        s->buf.push_back('\n');
    }
    if (lvl <= L_WARN || g_sink[ch].buf.size() > (1u << 20) || g_all.buf.size() > (4u << 20)) flush_locked();
    LeaveCriticalSection(&g_cs);
}

void log_flush() {
    if (!g_ready) return;
    EnterCriticalSection(&g_cs);
    flush_locked();
    LeaveCriticalSection(&g_cs);
}

void log_shutdown() {
    if (!g_ready) return;
    EnterCriticalSection(&g_cs);
    flush_locked();
    for (auto& s : g_sink)
        if (s.f) { fclose(s.f); s.f = nullptr; }
    if (g_all.f) { fclose(g_all.f); g_all.f = nullptr; }
    g_ready = false;
    LeaveCriticalSection(&g_cs);
}

}  // namespace rol
