// The players' build (ROLHOOK_PLAYER, mk.bat: build\native\player\ddraw.dll, the one the release ships): rolhook
// without its developer tools. These modules are not compiled into it, and what the rest of rolhook calls in them
// does nothing here:
//   probe.cpp     hardware watchpoints and the UI-root probe ([debug] watch / ui_probe)
//   shots.cpp     screenshots on a key and the automated test runs ([debug] shot_key, [test])
//   capture.cpp   one frame's draw calls to a file ([debug] capture_key / capture_at)
//   motion.cpp    the motion probe ([debug] motion_probe)
//   calls.cpp     engine calls counted per frame ([debug] count_calls)
//   scriptui.cpp  the campaign scripts' window functions traced ([debug] log_script_ui)
// and, fenced off in their own files: the sprite log (uilayout.cpp, [debug] log_sprites), the test runs' clicks on the
// options window (ui.cpp) and the memory dump on a crash (crash.cpp; the crash log keeps the stack and registers).
// So the players' DLL holds no keyboard or mouse input of its own, no thread control, no reading of memory through
// the process API and no memory dumps - only what draws the game (tools/release.py checks its imports).
#include "rolhook.h"

namespace rol {

// names of textures and shaders: kept for frame captures and the probes, nothing reads them in the players' build
void name_texture(void*, const char*) {}
std::string texture_name(void*) { return "?"; }
void name_shader(void*, const char*) {}

bool capturing() { return false; }
bool capture_request() { return false; }
void capture_event(const char*, ...) {}
void capture_draw(::IDirect3DDevice9*, const char*, UINT, const void*, UINT) {}
void capture_on_present() {}

bool g_shots_watch = false;
void shots_install(HMODULE) {}
void shots_terrain_texture(void*) {}
void shots_before_draw(::IDirect3DDevice9*) {}
void shots_on_present(::IDirect3DDevice9*) {}
void shots_tick() {}
void shots_summary() {}
double test_clock() { return -1.0; }
void ui_test_open_tab(const wchar_t*) {}
void ui_test_tick() {}

void probe_install(HMODULE) {}
void probe_on_present() {}
void probe_summary() {}
bool probe_watch_now(uintptr_t, int, bool, const char*) { return false; }

void spritelog_install(HMODULE) {}
void spritelog_texture(uintptr_t) {}
void scriptui_install(HMODULE) {}
void scriptui_summary() {}

bool g_motion_on = false;
void motion_install() {}
void motion_device(::IDirect3DDevice9*) {}
void motion_draw(::IDirect3DDevice9*, INT, UINT, UINT) {}
void motion_on_present() {}
void motion_summary() {}

void calls_install() {}
void calls_on_present() {}

}  // namespace rol
