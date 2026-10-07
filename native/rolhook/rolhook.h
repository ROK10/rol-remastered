// rolhook — diagnostics layer for Rise of Legends, loaded as a ddraw.dll proxy.
// Shared declarations: logger, config, helpers, subsystem entry points.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct IDirect3DDevice9;   // d3d9.h declares it the same way (DECLARE_INTERFACE_ -> struct)
struct IDirect3DSurface9;
struct IDirect3DPixelShader9;
struct _D3DPRESENT_PARAMETERS_;
struct _D3DVIEWPORT9;
typedef struct _D3DVIEWPORT9 D3DVIEWPORT9;
struct _D3DRECT;
typedef struct _D3DRECT D3DRECT;

namespace rol {

enum Level { L_OFF = 0, L_ERROR, L_WARN, L_INFO, L_DEBUG, L_TRACE };
enum Channel { CH_CORE = 0, CH_FILES, CH_BIGS, CH_TEXTURES, CH_D3D, CH_FRAME, CH_MEMORY, CH_UI, CH_PROBE, CH_COUNT };

struct Config {
    bool enabled = true;
    std::wstring game_dir;      // folder of the host exe, ends with '\'
    std::wstring log_root;      // where session folders are created
    std::wstring session_dir;   // this session, ends with '\'
    int flush_ms = 250;
    bool assert_ignore = true;  // auto-answer engine assert boxes with Ignore
    bool crash_dump = true;
    bool fix_lockrect = false;  // retry failed LockRect without D3DLOCK_DISCARD
    int frame_interval_s = 1;
    int memory_interval_s = 5;
    bool tex_override = false;      // serve <tex_override_dir>\<entry path>.dds instead of the archive texture
    std::wstring tex_override_dir;  // absolute, ends with '\'
    int anisotropy = 0;             // >1: turn the engine's LINEAR minification into anisotropic filtering
    int zoom_in = 0;                // camera zoom range (0 = keep data\gamecamera.xml): closest distance,
    int zoom_out = 0;               // farthest distance (extended zoom and scroll speed follow it),
    int zoom_steps = 0;             // number of zoom steps
    int zoom_spring = 3;            // overshoot of a wheel step allowed to the zoom animation: 0 none, 1 weak,
                                    // 2 medium, 3 as the game (each step carries the speed of the last one)
    int atoc = 0;                   // alpha-to-coverage on MSAA targets: 1 alpha-tested draws without blending,
                                    // 2 also alpha-tested draws that blend
    int lod_thresholds = 100;       // model LOD switch sizes in % of data\lod.xml (smaller = detailed farther, 0 = full)
    bool borderless = false;        // fullscreen as a borderless window over the monitor (windowed D3D device)
    bool shadow_linear = false;     // sample the engine's R32F shadow map with LINEAR instead of POINT filtering
    int shadow_size = 0;            // shadow map resolution (0 = as the game: the options capped at 2048)
    int shadow_fit = 0;             // how the shadow map is fitted to the view: 0 the engine's trapezoid over the whole
                                    // frustum, 1 the part of the map in view, 2 a layer of relief around the look-at point
    int shadow_fit_margin = 25;     // level 2: that layer reaches this % of the zoom distance above and below
    int anim_pose_ms = 0;           // smooth animation: the age (ms) a shared cached skeleton pose may have, 0 = as the
                                    // game (66); anim.cpp
    int anim_clock = 1;             // with smooth animation: the game world's clock smoothed over its tick steps (0 =
                                    // the engine's clock even then); anim.cpp
    int soft_shadows = 0;           // shadows on buildings and units filtered over a box of shadow-map texels: 0 off (one
                                    // tap, as the game), 1 / 2 / 3 = 2 / 4 / 6 texels wide; shaderfx.cpp
    int ssao = 0;                   // ambient occlusion in the battle scene: 0 off, 1 light, 2 medium, 3 strong; ssao.cpp
    int sharpen = 0;                // the battle scene sharpened (CAS) before the interface: 0 off, 1 / 2 / 3; ssao.cpp,
                                    // post.cpp
    int vibrance = 0;               // ... and its dull colours livened: 0 off, 1 / 2 / 3
    int gamma_light = 0;            // buildings' and units' light falloff as linear-space light gives it: 0 / 1;
                                    // shaderfx.cpp
    int ui_layout_scale = 100;      // interface laid out bigger (ui_scale mod): reads served from the overlay below;
                                    // fixed at the start
    int ui_layout_choice = 100;     // [ui] layout_scale as set (the Mods tab's value): 0 = auto, the scale for the
                                    // height the game runs at (uiauto.h, ui_screen_height); else the scale itself
    std::string ui_layout_auto;     // how auto chose (for the log)
    std::wstring ui_layout_overlay; // <game>\<[ui] layout_overlay>\<scale>\ (ends with '\'), empty = off
    int capture_vk = 0x6A;          // frame capture key (virtual-key code, Num *); 0 = off
    int capture_at = 0;             // also capture frame number N automatically (0 = off)
    int shot_vk = 0;                // screenshot key: the next frame -> <session>\shot_<frame>.png; 0 = off
    // Automated test run ([test], normally set only by the ROLHOOK_EXTRA_INI layer of modkit\tools\autotest.py):
    std::wstring extra_ini;             // that layer, if the variable names an existing file
    bool test_start_first_frame = false;   // the test clock starts at the first frame, not at the match start
    std::vector<double> test_shots;     // screenshots at these seconds of the test clock
    std::vector<std::pair<double, int>> test_wheel;   // mouse wheel notches (+ = zoom in) at seconds of the clock
    struct TestKey {
        double at;   // second of the test clock
        int vk;      // virtual-key code
        double ms;   // held this long
    };
    std::vector<TestKey> test_keys;     // keys held at seconds of the clock (the arrows pan the camera)
    bool test_capture = false;          // a frame capture with each of them
    double test_options = 0;            // the options window opened (F11) at this second of the clock (0 = never)
    std::wstring test_options_tab;      // ... and this tab of it shown (its button's id_string: Mods, Graphics...)
    double test_quit = 0;               // end the game at this second of the test clock (0 = never)
    double test_timeout = 0;            // end it this many seconds after the start of the process (0 = never)
    std::vector<std::wstring> pristine; // archive names served from modkit\backup\BIGS instead of BIGS
};

extern Config g_cfg;
extern volatile int g_level[CH_COUNT];
extern volatile LONG g_frame;

// log.cpp
void log_init();
void log_shutdown();
void log_flush();
void logf(int ch, int lvl, const char* fmt, ...);
double now_ms();
std::string config_sources();   // "rolhook.ini + rolhook.d\x.ini ..." — files the config was layered from
// The user's own layer: rolhook.d\zz_options.ini, written by the options tab, read last (overrides the mods).
int config_int(const wchar_t* sec, const wchar_t* key, int def, bool include_user);
std::wstring config_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def);
int config_key(const wchar_t* sec, const wchar_t* key, int def);   // F1..F24 / key code / off
void user_ini_set(const wchar_t* sec, const wchar_t* key, int value);
void user_ini_remove(const wchar_t* sec, const wchar_t* key);
inline bool on(int ch, int lvl) { return lvl <= g_level[ch]; }

// util.cpp
std::string w2u(const wchar_t* s);
std::wstring a2w(const char* s);
std::string a2u(const char* s);
std::string rel_path(const wchar_t* path);     // relative to the game folder when inside it, UTF-8
std::string module_name(HMODULE m);
std::string addr_desc(const void* p);           // "module+0xoffset"
std::string stack_desc(int skip, int frames);
int game_callers(const void* sp, uint32_t* out, int cap);   // return addresses into legends.exe code on the stack
bool patch_slot(void** slot, void* hook, void** orig);
bool patch_code(uintptr_t at, const void* expect, const void* bytes, int len);   // expect may be null
void* detour(uintptr_t target, const unsigned char* expect, int len, void* hook);   // -> trampoline or null
bool patch_iat(HMODULE mod, const char* dll, const char* func, void* hook, void** orig);

// files.cpp — file I/O and .big archive tracing
void files_install(HMODULE exe);
void files_summary();
struct EntryRef {
    char archive[96];
    char name[200];
    uint32_t usize;
    double t;
};
const EntryRef* last_entry();   // last .big entry read by the calling thread (nullptr if none)
const char* last_fx_file();     // shader file (.fxv cache / .fx source) the calling thread read last (recently)
bool fx_file_of(const void* data, size_t size, char* out, size_t cap);   // the shader file that gave exactly this
bool fx_entry_of_size(size_t size, char* out, size_t cap);   // fxobjects.big entries unpacking to this size ("a/b")

// gfx.cpp — module loading, Direct3D 9 and D3DX tracing
void gfx_install(HMODULE exe);
void gfx_summary();
int shadow_map_size();   // side of the engine's shadow map render target (0 until it is made)
// The view camera's projection, read live: near / far plane, tan of half the field of view across / up.
bool view_projection(float* n, float* f, float* tx, float* ty);
bool view_focus(float* focus);   // the eye-target distance (+0x26C)
bool view_eye_target(float eye[3], float target[3]);   // +0x2A8 / +0x288, world (z up)
float texture_hd_factor(int axis);   // HD texels per logical texel of the interface sprite being drawn (1 = original)

// capture.cpp — one frame's draw calls to a file on the capture key
void name_texture(void* tex, const char* name);
std::string texture_name(void* tex);   // the name name_texture gave it ("?" unknown)
void* record_texture(uintptr_t record);   // gfx.cpp: the IDirect3DTexture9 of a texture manager record
void name_shader(void* sh, const char* name);
bool capturing();
bool capture_request();   // capture the next frame (false when a capture is already under way)
void capture_event(const char* fmt, ...);
void capture_draw(::IDirect3DDevice9* d, const char* kind, UINT prims, const void* up, UINT up_stride);
void capture_on_present();

// shots.cpp — screenshots ([debug] shot_key) and the automated test run ([test]): timed shots, captures, quit
void shots_install(HMODULE exe);
void shots_terrain_texture(void* tex);                 // a terrain tile texture was created (marks the match start)
extern bool g_shots_watch;                             // shots_before_draw() wanted (the match start not seen yet)
void shots_before_draw(::IDirect3DDevice9* d);
void shots_on_present(::IDirect3DDevice9* d);          // at every Present, before the capture is closed
void shots_tick();                                     // background thread: hard stop of a hung test run
void shots_summary();
double test_clock();                                   // seconds of the test clock, -1 before it starts
void exit_summaries(const char* why);                  // main.cpp: the summaries written at process exit

// crash.cpp — engine asserts, unhandled exceptions, minidumps
void crash_install(HMODULE exe);

// ui.cpp — the "Mods" tab of the options window
void ui_install(HMODULE exe);
void ui_summary();
void ui_test_open_tab(const wchar_t* tab);   // [test] options_tab: shown once the options window is up
void ui_test_tick();                         // at Present, test runs only: does it

// camera.cpp — zoom range on the live game camera
void camera_install(HMODULE exe);
void camera_apply_now();
std::string camera_describe();   // the live camera's zoom distance and range, for frame captures
void camera_trace();             // [debug] log_camera: zoom state changes to ui.log, every Present

// window.cpp — borderless fullscreen ([graphics] borderless): windowed device over the monitor, cursor kept inside
void borderless_install(HMODULE exe);
bool borderless_params(const ::_D3DPRESENT_PARAMETERS_* in, ::_D3DPRESENT_PARAMETERS_& out);
void borderless_window(HWND w, UINT bb_w, UINT bb_h);
void borderless_present();
void borderless_tick();   // background thread: lets the cursor go when the game is not in front
void borderless_summary();

// shadow.cpp — shadow map resolution ([graphics] shadow_size), engine code patched at startup; the shadow map fit
void shadow_install(HMODULE exe);
void shadow_fit_install();              // records the engine's shadow map fit every frame (frame captures)
std::string shadow_matrix_describe();   // ShadowMapProjection and the last fit (light, TSM, view camera), for captures

// anim.cpp — smooth animation ([graphics] anim_pose_ms): the age a shared cached skeleton pose may have, and the
// game world's clock smoothed over the simulation's ticks ([graphics] anim_clock)
void anim_install(HMODULE exe);
void anim_apply(int ms);   // at once (the Mods tab); 0 = the game's
bool anim_clock_values(int* engine, int* given, int* left);   // the tick's part last frame: the engine's, the one
                                                              // given, the timer's signed time left (ms)
void anim_summary();

// shaderfx.cpp — copies of the effects' pixel shaders (shadergen.cpp): soft shadows ([graphics] soft_shadows) and
// gamma-correct light on buildings and units (gamma_light); the copies go in at SetPixelShader
void shaderfx_created(const DWORD* fn, IDirect3DPixelShader9* ps, const char* effect);
IDirect3DPixelShader9* shaderfx_swap(IDirect3DDevice9* d, IDirect3DPixelShader9* ps);
void shaderfx_apply(int soft_shadows, int gamma_light);   // at once (the Mods tab)
void shaderfx_summary();
HRESULT gfx_create_pixel_shader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** out);   // unhooked

// ssao.cpp — ambient occlusion ([graphics] ssao): the scene's depth drawn again into a single-sampled INTZ, the AO
// (ao.cpp) multiplied in when the scene is done; then, there too, the sharpening and vibrance ([graphics] sharpen /
// vibrance, post.cpp). The redraw itself and the raw device calls are in gfxraw.h
void ssao_shader_created(void* shader, const char* effect);
void ssao_set_vs(void* vs);
void ssao_set_ps(void* ps);
void ssao_set_rt(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s);   // before the game's SetRenderTarget
int ssao_before_draw(IDirect3DDevice9* d);   // before the game's draw: non-zero = draw it again (ssao_redraw) after
void ssao_present(IDirect3DDevice9* d);
void ssao_reset();                            // before the device's Reset: its resources released
void ssao_apply(int level);                   // at once (the Mods tab)
void post_apply(int sharpen, int vibrance);   // at once (the Mods tab)
void ssao_summary();

// lod.cpp — model LOD thresholds ([graphics] lod_thresholds), kept every frame
void lod_install(HMODULE exe);
void lod_tick();

// probe.cpp — hardware watchpoints and the UI-root probe (developer tools, [debug] watch / ui_probe)
void probe_install(HMODULE exe);
void probe_on_present();
void probe_summary();
bool probe_watch_now(uintptr_t addr, int len, bool write_only, const char* label);   // a watchpoint set from code

// uiscale.cpp — interface scale, D3D side: pretransformed draws redrawn from virtual to real pixels ([ui] scale)
void uiscale_install(HMODULE exe);
void uiscale_window(HWND w);                                  // the game window (from CreateDevice)
void uiscale_device(::IDirect3DDevice9* d, UINT w, UINT h);   // after CreateDevice / Reset: the real backbuffer
int uiscale_percent();                                        // 100 when the scale is off
void uiscale_viewport(::IDirect3DDevice9* d, D3DVIEWPORT9& vp);
void uiscale_scissor(::IDirect3DDevice9* d, RECT& r);
bool uiscale_clear_rects(::IDirect3DDevice9* d, DWORD n, const D3DRECT* in, D3DRECT* out);
void uiscale_stretch(::IDirect3DSurface9* src, const RECT*& sr, RECT& sbuf, ::IDirect3DSurface9* dst, const RECT*& dr,
                     RECT& dbuf);
bool uiscale_begin(::IDirect3DDevice9* d);
void uiscale_end(::IDirect3DDevice9* d);
void uiscale_summary();

// uilayout.cpp — engine side of the ui_scale mod ([ui] layout_scale): fonts rasterized at the scale, magnified
// sprites kept inside their atlas cells; both only once the overlay of scaled layouts is in use
void uilayout_install(HMODULE exe);
// The height the game will run at, for [ui] layout_scale=0 (auto); 'from' says where it came from. Registry and file
// reads only: it runs while the config is read in DllMain.
int ui_screen_height(std::string& from);
void uilayout_overlay_used();   // files.cpp: the overlay served a file
void uilayout_summary();
void spritelog_install(HMODULE exe);   // [debug] log_sprites: distinct interface pictures to ui.log
void spritelog_texture(uintptr_t record);   // gfx.cpp: the sprite being drawn asked its texture size

// scriptui.cpp — [debug] log_script_ui: the game scripts' window functions (campaign map windows...) traced to ui.log
void scriptui_install(HMODULE exe);
void scriptui_summary();

// motion.cpp — [debug] motion_probe: how often the units on screen move and change pose, frame by frame
extern bool g_motion_on;                     // the probe's window is open: draws and vertex buffer writes are read
void motion_install();
void motion_device(::IDirect3DDevice9* d);   // after CreateDevice: the vertex buffers' Lock / Unlock
void motion_draw(::IDirect3DDevice9* d, INT base_vertex, UINT min_index, UINT num_vertices);
void motion_on_present();
void motion_summary();

// calls.cpp — [debug] count_calls: engine functions' calls per frame (calls.log), a developer tool
void calls_install();
void calls_on_present();

// main.cpp
void memory_tick();

}  // namespace rol

#define RLOG(ch, lvl, ...) \
    do { if (::rol::on((ch), (lvl))) ::rol::logf((ch), (lvl), __VA_ARGS__); } while (0)
