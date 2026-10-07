// The "Mods" tab of the options window ("Options").
//
// The engine's OptionsWin reports every button click, checkbox toggle, slider move and combobox selection to
// four virtual methods of its control-listener interface (vtable 0x011AD020, which sits at OptionsWin + 0x6C).
// The tab's controls (id_string Rol*) are unknown to the engine: it neither fills them when the window is built
// nor saves them. rolhook does both:
//   * whenever a new set of Rol* controls appears (the window is built, or rebuilt after "Apply") and when the
//     tab is opened, the controls are set to the current values (layered config, see rolhook.d);
//   * on "Apply"/"OK" the controls are read (before the engine's handler, while the window is alive), the
//     values are applied at once (anisotropy, camera zoom and its spring, ATOC, LOD thresholds, smooth animation, soft
//     shadows, ambient occlusion, gamma-correct light, sharpening, vibrance; HD textures for textures loaded from
//     then on) and the ones that differ from the mods' defaults are written to rolhook.d\zz_options.ini;
//   * "Defaults" on this tab resets only the tab (the engine's reset is not called for a tab it does not know).
// The controls keep their state in fields of the control objects: checkbox +0x2E0, slider +0x2E4 (min +0x2E8,
// max +0x2EC), combobox +0x928 = selected index into the item array (+0x2F4, count +0x2E4; item +0xC = ITEM id).
//
// Addresses are for legends.exe 1.0 (Steam): image base 0x400000, no relocations. Each hooked function and
// engine helper is checked against its first bytes; on any mismatch nothing is hooked.
#include "rolhook.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace rol {
namespace {

constexpr uintptr_t kImageBase = 0x00400000;
constexpr uintptr_t VT_LISTENER = 0x011AD020;   // OptionsWin control-listener vtable (this = OptionsWin + 0x6C)
constexpr int SLOT_BUTTON = 82, SLOT_CHECKBOX = 115, SLOT_SLIDER = 137, SLOT_COMBO = 197;
constexpr uintptr_t FN_BUTTON = 0x00A9CF10;     // (ctl)            Apply / OK / Cancel / Defaults / tabs ...
constexpr uintptr_t FN_CHECKBOX = 0x00A9CA80;   // (ctl)
constexpr uintptr_t FN_SLIDER = 0x00A9CB10;     // (ctl, value)
constexpr uintptr_t FN_COMBO = 0x00A9C9D0;      // (ctl, item id)
constexpr uintptr_t FN_HANDLE_TEXT = 0x00464BE0;   // esi = string handle -> eax = wchar_t* (not 0-terminated)
constexpr int OPTIONSWIN_LISTENER_OFS = 0x6C;
constexpr int WIN_CHILD_COUNT = 0x228, WIN_CHILDREN = 0x238;   // UI window: array of child controls
constexpr int VT_AS_WINDOW = 10;                                // child->as_window(): nested window or null
constexpr int CTL_HANDLE = 0x04;     // id_string handle: +0 string*, +6 u16 offset, +8 u16 length, +0xA flags
constexpr int CTL_XML_ID = 0x2C;     // int16 'id' attribute from the XML
constexpr uintptr_t VT_COMBOBOX = 0x01150F88, VT_CHECKBOX = 0x011542C0, VT_SLIDER = 0x01158CA8, VT_BUTTON = 0x010BAFE8;
constexpr int CHECKBOX_STATE = 0x2E0;
constexpr int SLIDER_VALUE = 0x2E4, SLIDER_MIN = 0x2E8, SLIDER_MAX = 0x2EC;
constexpr int COMBO_COUNT = 0x2E4, COMBO_ITEMS = 0x2F4, COMBO_SELECTED = 0x928, ITEM_ID = 0x0C;

struct Sig {
    uintptr_t va;
    unsigned char bytes[8];
};
const Sig kSigs[] = {
    {FN_BUTTON, {0x81, 0xEC, 0x6C, 0x01, 0x00, 0x00, 0x53, 0x55}},
    {FN_CHECKBOX, {0x53, 0x55, 0x56, 0x57, 0x6A, 0x01, 0x68, 0xAC}},
    {FN_SLIDER, {0x83, 0xEC, 0x08, 0x8B, 0x44, 0x24, 0x10, 0x53}},
    {FN_COMBO, {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56, 0x57, 0x8B}},
    {FN_HANDLE_TEXT, {0x8A, 0x46, 0x0A, 0xA8, 0x01, 0x74, 0x18, 0xA8}},
};

typedef int(__thiscall* Ctl1_t)(void* self, void* ctl);
typedef int(__thiscall* Ctl2_t)(void* self, void* ctl, int value);
typedef void*(__thiscall* AsWindow_t)(void* self);
Ctl1_t o_button = nullptr, o_checkbox = nullptr;
Ctl2_t o_slider = nullptr, o_combo = nullptr;

volatile LONG g_events = 0, g_ours = 0, g_commits = 0;

// ---------------------------------------------------------------- the tab's settings
enum {
    K_ANISO, K_HD, K_ZOOM_OUT, K_ZOOM_STEPS, K_ATOC, K_LOD, K_BORDERLESS, K_SHADOW, K_SHADOW_FIT, K_UI_SCALE,
    K_ZOOM_SPRING, K_ANIM, K_SOFT, K_SSAO, K_GAMMA, K_SHARPEN, K_VIBRANCE, K_COUNT
};
const wchar_t* const kCtlName[K_COUNT] = {L"RolAnisotropy", L"RolHdTextures", L"RolZoomOut",   L"RolZoomSteps",
                                          L"RolAtoc",       L"RolLodDetail",  L"RolBorderless", L"RolShadowSize",
                                          L"RolShadowFalloff", L"RolUiScale",  L"RolZoomSpring", L"RolSmoothAnim",
                                          L"RolSoftShadows", L"RolSsao",  L"RolGammaLight", L"RolSharpen",
                                          L"RolVibrance"};
const uintptr_t kCtlVt[K_COUNT] = {VT_COMBOBOX, VT_CHECKBOX, VT_COMBOBOX, VT_COMBOBOX, VT_CHECKBOX, VT_COMBOBOX,
                                   VT_CHECKBOX, VT_COMBOBOX, VT_COMBOBOX, VT_COMBOBOX, VT_COMBOBOX, VT_CHECKBOX,
                                   VT_COMBOBOX, VT_COMBOBOX, VT_CHECKBOX, VT_COMBOBOX, VT_COMBOBOX};
struct Key {
    const wchar_t* sec;
    const wchar_t* key;
    int game_default;   // what the game does when no mod sets the key
};
const Key kKey[K_COUNT] = {{L"graphics", L"anisotropy", 0},
                           {L"textures", L"override", 0},
                           {L"camera", L"zoom_out", 7500},
                           {L"camera", L"zoom_steps", 8},
                           {L"graphics", L"atoc", 0},
                           {L"graphics", L"lod_thresholds", 100},
                           {L"graphics", L"borderless", 0},
                           {L"graphics", L"shadow_size", 0},
                           {L"graphics", L"shadow_fit", 0},
                           {L"ui", L"layout_scale", 100},
                           {L"camera", L"zoom_spring", 3},
                           {L"graphics", L"anim_pose_ms", 0},
                           {L"graphics", L"soft_shadows", 0},
                           {L"graphics", L"ssao", 0},
                           {L"graphics", L"gamma_light", 0},
                           {L"graphics", L"sharpen", 0},
                           {L"graphics", L"vibrance", 0}};
// settings the tab no longer has (rim light and specular, tried and dropped): their keys go from zz_options.ini at a
// commit
const Key kRetired[] = {{L"graphics", L"rim_light", 0}, {L"graphics", L"specular", 0}};

struct Values {
    int v[K_COUNT];
};

Values current() {
    Values x;
    x.v[K_ANISO] = g_cfg.anisotropy;
    x.v[K_HD] = g_cfg.tex_override ? 1 : 0;
    x.v[K_ZOOM_OUT] = g_cfg.zoom_out > 0 ? g_cfg.zoom_out : kKey[K_ZOOM_OUT].game_default;
    x.v[K_ZOOM_STEPS] = g_cfg.zoom_steps > 0 ? g_cfg.zoom_steps : kKey[K_ZOOM_STEPS].game_default;
    x.v[K_ATOC] = g_cfg.atoc;
    x.v[K_LOD] = g_cfg.lod_thresholds;
    x.v[K_BORDERLESS] = g_cfg.borderless ? 1 : 0;
    x.v[K_SHADOW] = g_cfg.shadow_size;
    x.v[K_SHADOW_FIT] = g_cfg.shadow_fit;
    x.v[K_UI_SCALE] = g_cfg.ui_layout_choice;
    x.v[K_ZOOM_SPRING] = g_cfg.zoom_spring;
    x.v[K_ANIM] = g_cfg.anim_pose_ms;
    x.v[K_SOFT] = g_cfg.soft_shadows;
    x.v[K_SSAO] = g_cfg.ssao;
    x.v[K_GAMMA] = g_cfg.gamma_light;
    x.v[K_SHARPEN] = g_cfg.sharpen;
    x.v[K_VIBRANCE] = g_cfg.vibrance;
    return x;
}

// Values the mods set (rolhook.ini + rolhook.d\*.ini without the user's zz_options.ini).
Values defaults() {
    Values x;
    for (int k = 0; k < K_COUNT; ++k) {
        int v = config_int(kKey[k].sec, kKey[k].key, kKey[k].game_default, false);
        // zoom: 0 in the ini means "keep gamecamera.xml"; for the LOD share 0 is a real value (always full detail)
        x.v[k] = ((k == K_ZOOM_OUT || k == K_ZOOM_STEPS) && v <= 0) ? kKey[k].game_default : v;
    }
    return x;
}

// ---------------------------------------------------------------- engine access
__declspec(naked) const wchar_t* __stdcall eng_handle_text(const void* /*handle*/) {
    __asm {
        push esi
        mov esi, [esp + 8]
        mov eax, 0x00464BE0
        call eax
        pop esi
        ret 4
    }
}

// Copies the control's id_string into buf (0-terminated); returns its length, 0 if none, -1 on a fault.
int control_name(const void* ctl, wchar_t* buf, int cap) {
    buf[0] = 0;
    __try {
        const uint8_t* h = static_cast<const uint8_t*>(ctl) + CTL_HANDLE;
        if (!*reinterpret_cast<void* const*>(h)) return 0;
        const wchar_t* p = eng_handle_text(h);
        int len = *reinterpret_cast<const uint16_t*>(h + 8);
        if (!p || len <= 0) return 0;
        if (len >= cap) len = cap - 1;
        for (int i = 0; i < len; ++i) buf[i] = p[i];
        buf[len] = 0;
        return len;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = 0;
        return -1;
    }
}

bool read_bytes(const void* p, int ofs, void* out, int n) {
    __try {
        memcpy(out, static_cast<const uint8_t*>(p) + ofs, size_t(n));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_i32(void* p, int ofs, int v) {
    __try {
        *reinterpret_cast<int*>(static_cast<uint8_t*>(p) + ofs) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int read_i32(const void* p, int ofs, int def = -1) {
    int v = def;
    read_bytes(p, ofs, &v, 4);
    return v;
}

int read_i16(const void* p, int ofs) {
    int16_t v = 0;
    read_bytes(p, ofs, &v, 2);
    return v;
}

uintptr_t vtable_of(const void* p) {
    uintptr_t v = 0;
    read_bytes(p, 0, &v, 4);
    return v;
}

void* as_window(void* ctl) {
    void* w = nullptr;
    __try {
        void** vt = *static_cast<void***>(ctl);
        w = reinterpret_cast<AsWindow_t>(vt[VT_AS_WINDOW])(ctl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        w = nullptr;
    }
    return w;
}

// Finds the tab's controls anywhere under 'win'; logs the whole tree at trace level.
void find_ours(void* win, void* out[K_COUNT], int depth, int& budget) {
    if (!win || depth > 12 || budget <= 0) return;
    int n = read_i32(win, WIN_CHILD_COUNT, 0);
    void** arr = nullptr;
    read_bytes(win, WIN_CHILDREN, &arr, 4);
    if (n <= 0 || n > 4096 || !arr) return;
    for (int i = 0; i < n && budget > 0; ++i, --budget) {
        void* c = nullptr;
        if (!read_bytes(arr, i * 4, &c, 4) || !c) continue;
        wchar_t name[96];
        int len = control_name(c, name, 96);
        if (len >= 3 && wcsncmp(name, L"Rol", 3) == 0) {
            for (int k = 0; k < K_COUNT; ++k)
                if (!wcscmp(name, kCtlName[k])) out[k] = c;
        }
        void* sub = as_window(c);
        RLOG(CH_UI, L_TRACE, "  tree %*s%-32s xml id %4d  ctl %p vt %s%s", depth * 2, "", w2u(name).c_str(),
             read_i16(c, CTL_XML_ID), c, addr_desc((void*)vtable_of(c)).c_str(), sub ? "  [window]" : "");
        if (sub) find_ours(sub, out, depth + 1, budget);
    }
}

bool find_ours(void* win, void* out[K_COUNT]) {
    for (int k = 0; k < K_COUNT; ++k) out[k] = nullptr;
    int budget = 6000;
    find_ours(win, out, 0, budget);
    bool any = false;
    for (int k = 0; k < K_COUNT; ++k) {
        if (out[k] && vtable_of(out[k]) != kCtlVt[k]) {
            RLOG(CH_UI, L_WARN, "%s has an unexpected control type (vt %s), left alone", w2u(kCtlName[k]).c_str(),
                 addr_desc((void*)vtable_of(out[k])).c_str());
            out[k] = nullptr;
        }
        any |= out[k] != nullptr;
    }
    return any;
}

// combobox: item array access
int combo_count(void* c) { return read_i32(c, COMBO_COUNT, 0); }
int combo_item_id(void* c, int i) {
    void** items = nullptr;
    void* it = nullptr;
    if (!read_bytes(c, COMBO_ITEMS, &items, 4) || !items || !read_bytes(items, i * 4, &it, 4) || !it) return 0;
    return read_i32(it, ITEM_ID, 0);
}

void set_control(int k, void* c, int v) {
    if (!c) return;
    if (kCtlVt[k] == VT_CHECKBOX) {
        write_i32(c, CHECKBOX_STATE, v ? 1 : 0);
    } else if (kCtlVt[k] == VT_SLIDER) {
        int lo = read_i32(c, SLIDER_MIN, 0), hi = read_i32(c, SLIDER_MAX, 0);
        if (hi > lo) v = v < lo ? lo : (v > hi ? hi : v);
        write_i32(c, SLIDER_VALUE, v);
    } else {
        // the item with exactly this id, else the nearest one (e.g. anisotropy 3 -> 2x)
        int n = combo_count(c), best = -1, best_d = 0x7fffffff;
        if (n <= 0 || n > 256) return;
        for (int i = 0; i < n; ++i) {
            int d = abs(combo_item_id(c, i) - v);
            if (d < best_d) best_d = d, best = i;
        }
        if (best >= 0) write_i32(c, COMBO_SELECTED, best);   // = ComboBox::select(best, notify = false)
    }
}

int get_control(int k, void* c, int def) {
    if (!c) return def;
    if (kCtlVt[k] == VT_CHECKBOX) return read_i32(c, CHECKBOX_STATE, def) ? 1 : 0;
    if (kCtlVt[k] == VT_SLIDER) return read_i32(c, SLIDER_VALUE, def);
    int n = combo_count(c), sel = read_i32(c, COMBO_SELECTED, -1);
    if (n <= 0 || n > 256) return def;
    if (sel < 0 || sel >= n) sel = 0;   // nothing selected: the combobox shows its first item
    return combo_item_id(c, sel);
}

// The first control named 'want' with vtable 'vt' anywhere under 'win', or null; 'path' gets the child windows that
// lead to it (outermost first), for its position.
void* find_control(void* win, const wchar_t* want, uintptr_t vt, int depth, int& budget, void** path, int& path_len) {
    if (!win || depth > 12 || budget <= 0) return nullptr;
    int n = read_i32(win, WIN_CHILD_COUNT, 0);
    void** arr = nullptr;
    read_bytes(win, WIN_CHILDREN, &arr, 4);
    if (n <= 0 || n > 4096 || !arr) return nullptr;
    for (int i = 0; i < n && budget > 0; ++i, --budget) {
        void* c = nullptr;
        if (!read_bytes(arr, i * 4, &c, 4) || !c) continue;
        wchar_t name[96];
        if (control_name(c, name, 96) > 0 && !wcscmp(name, want) && vtable_of(c) == vt) return c;
        if (void* sub = as_window(c)) {
            path[path_len++] = c;
            if (void* hit = find_control(sub, want, vt, depth + 1, budget, path, path_len)) return hit;
            --path_len;
        }
    }
    return nullptr;
}

// A control's rectangle within its parent: the engine's int16 {left, right, top, bottom} (a tab button's found by its
// size in 0.9.20's test run, the same field in every control and window).
constexpr int CTL_RECT = 0x29E;
bool rect_of(const void* c, int16_t q[4]) { return read_bytes(c, CTL_RECT, q, 8); }

// ---------------------------------------------------------------- [test] options_tab
std::wstring g_want_tab;          // the tab a test run wants shown (shots.cpp posts F11, then asks for it)
long g_want_since = 0;            // ... asked at this frame
void* g_listener = nullptr;       // the options window's listener (OptionsWin + 0x6C) in its last event
long g_listener_frame = -1000;    // ... at this frame

void seen_listener(void* self) {
    g_listener = self;
    g_listener_frame = long(g_frame);
}

// ---------------------------------------------------------------- tab state
void* g_synced[K_COUNT] = {};   // controls rolhook last filled; new pointers = the window was (re)built
const wchar_t* g_tab = L"";     // last tab button pressed
wchar_t g_tab_buf[32];

void fill(void* ctl[K_COUNT], const Values& x, const char* why) {
    for (int k = 0; k < K_COUNT; ++k) {
        set_control(k, ctl[k], x.v[k]);
        g_synced[k] = ctl[k];
    }
    RLOG(CH_UI, L_INFO,
         "Mods tab filled (%s): anisotropy %d, HD %d, zoom out %d, %d steps, ATOC %d, LOD %d%%, borderless %d, shadows %d, "
         "shadow fit %d, interface %d%% (0 = auto), zoom spring %d, smooth animation %d, soft shadows %d, ssao %d, "
         "gamma light %d, sharpening %d, vibrance %d",
         why, x.v[K_ANISO], x.v[K_HD], x.v[K_ZOOM_OUT], x.v[K_ZOOM_STEPS], x.v[K_ATOC], x.v[K_LOD], x.v[K_BORDERLESS],
         x.v[K_SHADOW], x.v[K_SHADOW_FIT], x.v[K_UI_SCALE], x.v[K_ZOOM_SPRING], x.v[K_ANIM], x.v[K_SOFT], x.v[K_SSAO],
         x.v[K_GAMMA], x.v[K_SHARPEN], x.v[K_VIBRANCE]);
}

void sync_if_new(void* win, const char* why) {
    void* ctl[K_COUNT];
    if (!find_ours(win, ctl)) return;
    bool same = true;
    for (int k = 0; k < K_COUNT; ++k) same &= ctl[k] == g_synced[k];
    if (!same) fill(ctl, current(), why);
}

void commit(void* win) {
    void* ctl[K_COUNT];
    if (!find_ours(win, ctl)) return;
    for (int k = 0; k < K_COUNT; ++k) {
        if (ctl[k] != g_synced[k]) {   // never filled by rolhook: it shows XML defaults, not the user's choice
            RLOG(CH_UI, L_WARN, "Mods tab not committed: controls were not filled in this window");
            return;
        }
    }
    Values was = current(), now = current(), def = defaults();
    for (int k = 0; k < K_COUNT; ++k) now.v[k] = get_control(k, ctl[k], was.v[k]);
    g_cfg.anisotropy = now.v[K_ANISO] < 0 ? 0 : (now.v[K_ANISO] > 16 ? 16 : now.v[K_ANISO]);
    g_cfg.tex_override = now.v[K_HD] != 0;
    g_cfg.zoom_out = now.v[K_ZOOM_OUT];
    g_cfg.zoom_steps = now.v[K_ZOOM_STEPS];
    // the checkbox is on/off; atoc=2 (blended cut-outs too) set in the ini survives a commit that keeps it on
    if (now.v[K_ATOC]) now.v[K_ATOC] = was.v[K_ATOC] > 0 ? was.v[K_ATOC] : 1;
    g_cfg.atoc = now.v[K_ATOC];
    now.v[K_LOD] = now.v[K_LOD] < 0 ? 0 : (now.v[K_LOD] > 200 ? 200 : now.v[K_LOD]);
    g_cfg.lod_thresholds = now.v[K_LOD];   // lod.cpp applies it at the next frame
    g_cfg.borderless = now.v[K_BORDERLESS] != 0;   // takes effect at the next start (the device is made then)
    // shadow map size: the engine code is patched at startup, so it takes effect at the next start as well
    if (now.v[K_SHADOW] < 0 || now.v[K_SHADOW] > 16384 || (now.v[K_SHADOW] & (now.v[K_SHADOW] - 1))) now.v[K_SHADOW] = 0;
    g_cfg.shadow_size = now.v[K_SHADOW];
    // the shadow map fit is computed every frame: from the next frame on
    now.v[K_SHADOW_FIT] = now.v[K_SHADOW_FIT] < 0 ? 0 : (now.v[K_SHADOW_FIT] > 2 ? 2 : now.v[K_SHADOW_FIT]);
    g_cfg.shadow_fit = now.v[K_SHADOW_FIT];
    // interface scale (0 = auto): the layouts are read at the game's start, so from the next start; the scale in use
    // (g_cfg.ui_layout_scale) stays
    now.v[K_UI_SCALE] = now.v[K_UI_SCALE] <= 0 ? 0 : (now.v[K_UI_SCALE] < 100 ? 100 : (now.v[K_UI_SCALE] > 300 ? 300
                                                                                  : now.v[K_UI_SCALE]));
    g_cfg.ui_layout_choice = now.v[K_UI_SCALE];
    // the zoom's spring: read at every wheel step, so at once
    now.v[K_ZOOM_SPRING] = now.v[K_ZOOM_SPRING] < 0 ? 0 : (now.v[K_ZOOM_SPRING] > 3 ? 3 : now.v[K_ZOOM_SPRING]);
    g_cfg.zoom_spring = now.v[K_ZOOM_SPRING];
    // smooth animation: the checkbox is on/off, an anim_pose_ms set in the ini survives a commit that keeps it on;
    // anim.cpp rewrites the engine's code and variables at once
    if (now.v[K_ANIM]) now.v[K_ANIM] = was.v[K_ANIM] > 0 ? was.v[K_ANIM] : 1;
    g_cfg.anim_pose_ms = now.v[K_ANIM];
    anim_apply(g_cfg.anim_pose_ms);
    // soft shadows: the shader copies are swapped in at SetPixelShader, so at once
    now.v[K_SOFT] = now.v[K_SOFT] < 0 ? 0 : (now.v[K_SOFT] > 3 ? 3 : now.v[K_SOFT]);
    g_cfg.soft_shadows = now.v[K_SOFT];
    // gamma-correct light: the same shader copies, so at once
    now.v[K_GAMMA] = now.v[K_GAMMA] ? 1 : 0;
    g_cfg.gamma_light = now.v[K_GAMMA];
    shaderfx_apply(g_cfg.soft_shadows, g_cfg.gamma_light);
    // ambient occlusion: drawn each frame from the level, so at once
    now.v[K_SSAO] = now.v[K_SSAO] < 0 ? 0 : (now.v[K_SSAO] > 3 ? 3 : now.v[K_SSAO]);
    g_cfg.ssao = now.v[K_SSAO];
    ssao_apply(g_cfg.ssao);
    // sharpening and vibrance: one pass at the scene's end, each frame, so at once
    now.v[K_SHARPEN] = now.v[K_SHARPEN] < 0 ? 0 : (now.v[K_SHARPEN] > 3 ? 3 : now.v[K_SHARPEN]);
    now.v[K_VIBRANCE] = now.v[K_VIBRANCE] < 0 ? 0 : (now.v[K_VIBRANCE] > 3 ? 3 : now.v[K_VIBRANCE]);
    g_cfg.sharpen = now.v[K_SHARPEN];
    g_cfg.vibrance = now.v[K_VIBRANCE];
    post_apply(g_cfg.sharpen, g_cfg.vibrance);
    for (int k = 0; k < K_COUNT; ++k) {
        if (now.v[k] == def.v[k]) user_ini_remove(kKey[k].sec, kKey[k].key);
        else user_ini_set(kKey[k].sec, kKey[k].key, now.v[k]);
    }
    for (const Key& r : kRetired) user_ini_remove(r.sec, r.key);
    InterlockedIncrement(&g_commits);
    RLOG(CH_UI, L_INFO,
         "Mods tab applied: anisotropy %d -> %d, HD textures %d -> %d (textures loaded from now on), zoom out %d -> %d, "
         "steps %d -> %d, ATOC %d -> %d, LOD %d%% -> %d%%, borderless %d -> %d, shadows %d -> %d (next start), shadow "
         "fit %d -> %d, interface %d%% -> %d%% (0 = auto; next start), zoom spring %d -> %d, smooth animation "
         "%d -> %d, soft shadows %d -> %d, ssao %d -> %d, gamma light %d -> %d, sharpening %d -> %d, vibrance "
         "%d -> %d; saved to rolhook.d\\zz_options.ini (keys equal to the mods' defaults removed)",
         was.v[K_ANISO], now.v[K_ANISO], was.v[K_HD], now.v[K_HD], was.v[K_ZOOM_OUT], now.v[K_ZOOM_OUT],
         was.v[K_ZOOM_STEPS], now.v[K_ZOOM_STEPS], was.v[K_ATOC], now.v[K_ATOC], was.v[K_LOD], now.v[K_LOD],
         was.v[K_BORDERLESS], now.v[K_BORDERLESS], was.v[K_SHADOW], now.v[K_SHADOW], was.v[K_SHADOW_FIT],
         now.v[K_SHADOW_FIT], was.v[K_UI_SCALE], now.v[K_UI_SCALE], was.v[K_ZOOM_SPRING], now.v[K_ZOOM_SPRING],
         was.v[K_ANIM], now.v[K_ANIM], was.v[K_SOFT], now.v[K_SOFT], was.v[K_SSAO], now.v[K_SSAO], was.v[K_GAMMA],
         now.v[K_GAMMA], was.v[K_SHARPEN], now.v[K_SHARPEN], was.v[K_VIBRANCE], now.v[K_VIBRANCE]);
    if (now.v[K_ZOOM_OUT] != was.v[K_ZOOM_OUT] || now.v[K_ZOOM_STEPS] != was.v[K_ZOOM_STEPS]) camera_apply_now();
}

void reset_ours(void* win) {
    void* ctl[K_COUNT];
    if (!find_ours(win, ctl)) return;
    fill(ctl, defaults(), "Defaults: the mods' defaults, applied with Apply/OK");
}

bool is_tab(const wchar_t* n) {
    static const wchar_t* const tabs[] = {L"Game", L"Mouse", L"Keyboard", L"Performance", L"Graphics", L"Audio",
                                          L"Player", L"Mods"};
    for (const wchar_t* t : tabs)
        if (!wcscmp(n, t)) return true;
    return false;
}

// ---------------------------------------------------------------- hooks
void* window_of(void* self) { return static_cast<uint8_t*>(self) - OPTIONSWIN_LISTENER_OFS; }

void log_event(const char* kind, void* ctl, const wchar_t* name, int len, int value, bool has_value) {
    InterlockedIncrement(&g_events);
    bool ours = len >= 3 && wcsncmp(name, L"Rol", 3) == 0;
    if (ours) InterlockedIncrement(&g_ours);
    char val[24] = "";
    if (has_value) _snprintf_s(val, sizeof(val), _TRUNCATE, "%d", value);
    RLOG(CH_UI, L_DEBUG, "%-8s %-28s %6s  ctl %p", kind, len > 0 ? w2u(name).c_str() : "(no name)", val, ctl);
}

int __fastcall h_button(void* self, void* /*edx*/, void* ctl) {
    seen_listener(self);
    wchar_t name[96];
    int len = control_name(ctl, name, 96);
    log_event("button", ctl, name, len, 0, false);
    void* win = window_of(self);
    if (len > 0 && is_tab(name)) {
        wcsncpy_s(g_tab_buf, name, _TRUNCATE);
        g_tab = g_tab_buf;
    }
    // before the engine's handler: OK / Cancel destroy the window, Apply rebuilds it
    if (len > 0 && (!_wcsicmp(name, L"apply") || !_wcsicmp(name, L"accept"))) commit(win);
    if (len > 0 && !_wcsicmp(name, L"reset") && !wcscmp(g_tab, L"Mods")) {
        reset_ours(win);
        return 0;   // the engine's reset would act on a tab it does not know
    }
    int r = o_button(self, ctl);
    // only controls rolhook has not filled yet: switching tabs back and forth keeps unapplied changes
    if (len > 0 && !wcscmp(name, L"Mods")) sync_if_new(win, "tab opened");
    return r;
}

int __fastcall h_checkbox(void* self, void* /*edx*/, void* ctl) {
    seen_listener(self);
    int r = o_checkbox(self, ctl);
    wchar_t name[96];
    int len = control_name(ctl, name, 96);
    log_event("checkbox", ctl, name, len, read_i32(ctl, CHECKBOX_STATE), true);
    sync_if_new(window_of(self), "window built");
    return r;
}

int __fastcall h_slider(void* self, void* /*edx*/, void* ctl, int value) {
    seen_listener(self);
    int r = o_slider(self, ctl, value);
    wchar_t name[96];
    int len = control_name(ctl, name, 96);
    log_event("slider", ctl, name, len, value, true);
    sync_if_new(window_of(self), "window built");
    return r;
}

int __fastcall h_combo(void* self, void* /*edx*/, void* ctl, int value) {
    seen_listener(self);
    int r = o_combo(self, ctl, value);
    wchar_t name[96];
    int len = control_name(ctl, name, 96);
    log_event("combo", ctl, name, len, value, true);
    // the engine fills its own comboboxes right after building the window: the first of these events is the moment
    // to fill ours
    sync_if_new(window_of(self), "window built");
    return r;
}

}  // namespace

void ui_install(HMODULE exe) {
    if (reinterpret_cast<uintptr_t>(exe) != kImageBase) {
        RLOG(CH_UI, L_WARN, "options window hooks off: legends.exe loaded at %p, expected 0x00400000", exe);
        return;
    }
    for (const Sig& s : kSigs) {
        if (memcmp(reinterpret_cast<const void*>(s.va), s.bytes, sizeof(s.bytes)) != 0) {
            RLOG(CH_UI, L_WARN, "options window hooks off: unknown legends.exe build (code at 0x%08x differs)",
                 unsigned(s.va));
            return;
        }
    }
    void** vt = reinterpret_cast<void**>(VT_LISTENER);
    const struct {
        int slot;
        uintptr_t fn;
        void* hook;
        void** orig;
    } slots[] = {
        {SLOT_BUTTON, FN_BUTTON, (void*)h_button, (void**)&o_button},
        {SLOT_CHECKBOX, FN_CHECKBOX, (void*)h_checkbox, (void**)&o_checkbox},
        {SLOT_SLIDER, FN_SLIDER, (void*)h_slider, (void**)&o_slider},
        {SLOT_COMBO, FN_COMBO, (void*)h_combo, (void**)&o_combo},
    };
    for (const auto& s : slots) {
        if (reinterpret_cast<uintptr_t>(vt[s.slot]) != s.fn) {
            RLOG(CH_UI, L_WARN, "options window hooks off: vtable slot %d holds %p, expected 0x%08x", s.slot,
                 vt[s.slot], unsigned(s.fn));
            return;
        }
    }
    int ok = 0;
    for (const auto& s : slots) ok += patch_slot(&vt[s.slot], s.hook, s.orig);
    Values c = current();
    RLOG(CH_UI, L_INFO, "options window hooks installed (%d/4); Mods tab values: anisotropy %d, HD %d, zoom out %d, %d steps",
         ok, c.v[K_ANISO], c.v[K_HD], c.v[K_ZOOM_OUT], c.v[K_ZOOM_STEPS]);
}

#ifndef ROLHOOK_PLAYER   // test runs only: not in the players' build (player_stubs.cpp)
void ui_test_open_tab(const wchar_t* tab) {
    g_want_tab = tab ? tab : L"";
    g_want_since = long(g_frame);
}

// The options window builds itself with a burst of its listener's events (its comboboxes filled); a few frames later
// the wanted tab's button is clicked for real - the tab row switches tabs on a click of its own (the listener only
// hears of it, so calling the listener does not switch). Its position: its rectangle within its parent plus those of
// the windows above it up to the options window's own; the cursor goes to its middle and a left click is sent
// (SendInput: the game must be in front).
void ui_test_tick() {
    if (g_want_tab.empty() || !o_button) return;
    const long f = long(g_frame);
    if (f - g_want_since > 600) {
        RLOG(CH_UI, L_WARN, "test: no options window within 600 frames - tab %s not shown",
             w2u(g_want_tab.c_str()).c_str());
        g_want_tab.clear();
        return;
    }
    if (!g_listener || g_listener_frame < g_want_since || f - g_listener_frame < 5) return;   // not up, or still building
    int budget = 6000, path_len = 0;
    void* path[16] = {};
    void* root = window_of(g_listener);
    void* btn = find_control(root, g_want_tab.c_str(), VT_BUTTON, 0, budget, path, path_len);
    const std::string tab = w2u(g_want_tab.c_str());
    g_want_tab.clear();
    if (!btn) {
        RLOG(CH_UI, L_WARN, "test: the options window has no tab button %s", tab.c_str());
        return;
    }
    int16_t q[4] = {};
    int x0 = 0, y0 = 0;
    std::string chain;
    char part[96];
    for (int i = -1; i < path_len; ++i) {   // -1: the options window itself
        const void* w = i < 0 ? root : path[i];
        if (!rect_of(w, q)) break;
        x0 += q[0];
        y0 += q[2];
        _snprintf_s(part, _TRUNCATE, "%s%d..%d x %d..%d", i < 0 ? "" : " / ", q[0], q[1], q[2], q[3]);
        chain += part;
    }
    if (!rect_of(btn, q) || q[1] <= q[0] || q[3] <= q[2]) {
        RLOG(CH_UI, L_WARN, "test: tab button %s (%p) has no rectangle - not clicked", tab.c_str(), btn);
        return;
    }
    POINT p = {x0 + (q[0] + q[1]) / 2, y0 + (q[2] + q[3]) / 2};
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (!w || pid != GetCurrentProcessId()) {
        RLOG(CH_UI, L_WARN, "test: the game is not in front - tab %s not clicked", tab.c_str());
        return;
    }
    ClientToScreen(w, &p);
    SetCursorPos(p.x, p.y);
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(2, in, sizeof(INPUT));
    RLOG(CH_UI, L_INFO, "test: tab %s clicked at %ld,%ld (its rectangle %d..%d x %d..%d in the windows %s; %u inputs "
         "sent, frame %ld)", tab.c_str(), p.x, p.y, q[0], q[1], q[2], q[3], chain.c_str(), sent, f);
}
#endif   // ROLHOOK_PLAYER

void ui_summary() {
    RLOG(CH_UI, L_INFO, "summary: %ld options-window events, %ld from the Mods tab, %ld applies", g_events, g_ours,
         g_commits);
}

}  // namespace rol
