// Copies of the effects' pixel shaders, made when the shaders are created and swapped in at SetPixelShader:
//   soft shadows ([graphics] soft_shadows = 0 off | 1 light | 2 medium | 3 strong): the shadow on buildings, units,
//     grass, decals and the ground filtered over a box of shadow-map texels instead of one tap with a hard, stepped
//     edge (or the ground's bilinear quad);
//   gamma-correct light ([graphics] gamma_light = 0 / 1): on buildings and units the sun's falloff across a surface
//     as light summed in linear space gives it, full sun and full shade kept.
//
// The effects' pixel shaders that receive the shadow - dot3shadow_2, metal_and_dot3shadow_2, unitdot3teamcolor_2,
// dot3teamcolorshadow_2, clockwork_2, flamingunit_2, grass_2, splatsdot3_2: 14 in a battle (ENGINE_NOTES 5) - test the
// fragment against the shadow map once; the buildings' and units' ones light with a normal map, the sun vector (t1),
// light_diffuse and a hemispheric ambient. When a shader is created rolhook disassembles it (D3DXDisassembleShader),
// finds what each change needs in it (shadergen.cpp: the shadow test by its data flow, the terrain's shadow pass by its
// code's hash, N.L and the light's constants by data flow and the constants' names - in the buildings' and units'
// effects only, sg::gamma_effect: the ground keeps the game's light) and keeps it. The copy is written -
// the soft-shadow rewrite, then the gamma change over it (shadertest.exe runs the same code on dumped shaders) - and
// assembled (D3DXAssembleShader) when the shader is first set with them on (soft shadows also need the shadow map's
// size), again whenever a setting or the map changes, so the Mods tab switches at once.
// Box widths 2 / 4 / 6 texels, each tap compared against the receiving surface's depth under it ([debug]
// soft_shadows_plane_bias=0: against the fragment's, as the game's one tap).
#include "rolhook.h"

#include <d3d9.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "shadergen.h"

namespace rol {
namespace {

typedef HRESULT(WINAPI* Disasm_t)(const DWORD*, BOOL, LPCSTR, IUnknown**);
typedef HRESULT(WINAPI* Assemble_t)(LPCSTR, UINT, const void*, void*, DWORD, IUnknown**, IUnknown**);
Disasm_t g_disasm = nullptr;
Assemble_t g_asm = nullptr;
bool g_api_tried = false;

struct Key {
    int soft = 0, gamma = 0, size = 0;
    bool operator==(const Key& o) const { return soft == o.soft && gamma == o.gamma && size == o.size; }
    bool any() const { return soft || gamma; }
};

struct Entry {
    sg::ShadowPlan shadow;
    sg::LightPlan light;
    bool has_shadow = false, has_light = false;
    std::string effect;
    IDirect3DPixelShader9* copy = nullptr;
    Key built;
    bool tried = false;   // a copy was made (or failed) for 'built'
    bool failed = false;
};
std::unordered_map<IDirect3DPixelShader9*, Entry*> g_entries;   // by the original shader (kept alive: AddRef)
SRWLOCK g_lock = SRWLOCK_INIT;
volatile LONG g_soft = 0, g_gamma = 0;
int g_plane_bias = -1, g_dump = -1;
long g_found_shadow = 0, g_found_light = 0, g_unmatched = 0, g_built = 0, g_build_failed = 0;
volatile LONG g_swaps = 0;

bool load_api() {
    if (!g_api_tried) {
        g_api_tried = true;
        HMODULE dx = GetModuleHandleW(L"d3dx9_27.dll");
        if (dx) {
            g_disasm = reinterpret_cast<Disasm_t>(::GetProcAddress(dx, "D3DXDisassembleShader"));
            g_asm = reinterpret_cast<Assemble_t>(::GetProcAddress(dx, "D3DXAssembleShader"));
        }
        if (!g_disasm || !g_asm) RLOG(CH_D3D, L_WARN, "shader copies: d3dx9_27.dll's shader assembler not found - off");
    }
    return g_disasm && g_asm;
}

std::string buffer_text(IUnknown* buf) {
    void** vt = *reinterpret_cast<void***>(buf);
    auto get_ptr = reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>(vt[3]);
    auto get_size = reinterpret_cast<DWORD(STDMETHODCALLTYPE*)(IUnknown*)>(vt[4]);
    const char* p = static_cast<const char*>(get_ptr(buf));
    return std::string(p ? p : "", p ? strnlen(p, get_size(buf)) : 0);
}

void release(IUnknown* u) {
    if (u) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>((*reinterpret_cast<void***>(u))[2])(u);
}

// The copy for the key; false when it cannot be made (the original is used).
bool build(IDirect3DDevice9* d, Entry& e, const Key& k) {
    if (e.copy) {
        release(e.copy);
        e.copy = nullptr;
    }
    e.built = k;
    e.tried = true;
    e.failed = false;
    if (g_plane_bias < 0) g_plane_bias = config_int(L"debug", L"soft_shadows_plane_bias", 1, true) != 0;
    if (g_dump < 0) g_dump = config_int(L"debug", L"dump_shaders", 0, true);
    std::string src = k.soft ? sg::soft_shadow_source(e.shadow, sg::kSoftWidth[k.soft], k.size, g_plane_bias != 0)
                             : sg::code_text(e.has_shadow ? e.shadow.code : e.light.code);
    if (!src.empty() && k.gamma) src = sg::light_source(src, e.light, {true});
    char what[64];
    _snprintf_s(what, _TRUNCATE, "soft %d, gamma %d", k.soft, k.gamma);
    if (src.empty()) {
        RLOG(CH_D3D, L_WARN, "shader copies: %s - no room for the copy (%s)", e.effect.c_str(), what);
        ++g_build_failed;
        e.failed = true;
        return false;
    }
    if (g_dump) {   // the copy's assembly next to the dumped originals
        std::string name = e.effect;
        for (char& c : name)
            if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.' && c != '-') c = '+';
        const std::wstring dir = g_cfg.session_dir + L"shaders\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t file[400];
        _snwprintf_s(file, _countof(file), _TRUNCATE, L"%s%S.s%dg%d.%p.asm", dir.c_str(), name.c_str(), k.soft, k.gamma,
                     static_cast<void*>(&e));
        FILE* f = nullptr;
        if (_wfopen_s(&f, file, L"wb") == 0 && f) {
            fwrite(src.data(), 1, src.size(), f);
            fclose(f);
        }
    }
    IUnknown *code = nullptr, *errs = nullptr;
    HRESULT hr = g_asm(src.c_str(), UINT(src.size()), nullptr, nullptr, 0, &code, &errs);
    if (FAILED(hr) || !code) {
        const std::string why = errs ? buffer_text(errs) : std::string("?");
        RLOG(CH_D3D, L_WARN, "shader copies: %s - the copy (%s) did not assemble (0x%08lx): %.300s", e.effect.c_str(), what,
             (unsigned long)hr, why.c_str());
        release(errs);
        release(code);
        ++g_build_failed;
        e.failed = true;
        return false;
    }
    release(errs);
    void** vt = *reinterpret_cast<void***>(code);
    const DWORD* fn = static_cast<const DWORD*>(reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>(vt[3])(code));
    IDirect3DPixelShader9* ps = nullptr;
    hr = gfx_create_pixel_shader(d, fn, &ps);
    release(code);
    if (FAILED(hr) || !ps) {
        RLOG(CH_D3D, L_WARN, "shader copies: %s - the device refused the copy (%s, 0x%08lx)", e.effect.c_str(), what,
             (unsigned long)hr);
        ++g_build_failed;
        e.failed = true;
        return false;
    }
    e.copy = ps;
    ++g_built;
    RLOG(CH_D3D, L_DEBUG, "shader copies: %s - copy made (%s, shadow map %d)", e.effect.c_str(), what, k.size);
    return true;
}

}  // namespace

void shaderfx_created(const DWORD* fn, IDirect3DPixelShader9* ps, const char* effect) {
    if (!fn || !ps || !load_api()) return;
    IUnknown* text = nullptr;
    if (FAILED(g_disasm(fn, FALSE, nullptr, &text)) || !text) return;
    const std::string asm_text = buffer_text(text);
    release(text);
    const std::string name = effect ? effect : "?";
    const bool shadowy = asm_text.find("//   shadow_sampler ") != std::string::npos;
    const bool lit = asm_text.find("//   light_diffuse ") != std::string::npos && sg::gamma_effect(name);
    if (!shadowy && !lit) return;
    Entry* e = new Entry();
    e->effect = name;
    std::string why_shadow, why_light;
    e->has_shadow = shadowy && sg::analyse_shadow(asm_text, e->shadow, &why_shadow);
    e->has_light = lit && sg::analyse_light(asm_text, e->light, &why_light);
    if (shadowy && !e->has_shadow) {
        ++g_unmatched;
        RLOG(CH_D3D, L_INFO, "shader copies: %s - a shadow-receiving pixel shader whose test is left as it is (%s)",
             e->effect.c_str(), why_shadow.c_str());
    }
    if (!e->has_shadow && !e->has_light) {
        delete e;
        return;
    }
    ps->AddRef();   // kept: a pointer freed and reused for another shader would get this one's copy
    AcquireSRWLockExclusive(&g_lock);
    g_entries[ps] = e;
    ReleaseSRWLockExclusive(&g_lock);
    if (e->has_shadow) ++g_found_shadow;
    if (e->has_light) ++g_found_light;
    char shadow[96] = "", light[96] = "";
    if (e->has_shadow) {
        if (e->shadow.terrain) _snprintf_s(shadow, _TRUNCATE, "the terrain's shadow pass");
        else _snprintf_s(shadow, _TRUNCATE, "one-tap shadow test (%s, tap at %d)", e->shadow.code[0].c_str(), e->shadow.tap);
    }
    if (e->has_light)
        _snprintf_s(light, _TRUNCATE, "%slight: %d N.L for the gamma", e->has_shadow ? "; " : "",
                    int(e->light.targets.size()));
    RLOG(CH_D3D, L_INFO, "shader copies: %s - %s%s", e->effect.c_str(), shadow, light);
}

IDirect3DPixelShader9* shaderfx_swap(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
    if (!ps || g_entries.empty()) return ps;
    const int soft = int(g_soft), gamma = int(g_gamma);
    if (!soft && !gamma) return ps;
    AcquireSRWLockShared(&g_lock);
    auto it = g_entries.find(ps);
    Entry* e = it == g_entries.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&g_lock);
    if (!e) return ps;
    Key k;
    k.soft = e->has_shadow ? soft : 0;
    k.size = k.soft ? shadow_map_size() : 0;
    if (k.soft && k.size <= 0) k.soft = 0;   // no shadow map yet: the lighting alone
    k.gamma = e->has_light && e->light.gamma ? gamma : 0;
    if (!k.any()) return ps;
    if (!e->tried || !(e->built == k))
        if (!build(d, *e, k)) return ps;
    if (e->failed || !e->copy) return ps;
    InterlockedIncrement(&g_swaps);
    return e->copy;
}

void shaderfx_apply(int soft, int gamma) {
    soft = soft < 0 ? 0 : (soft > 3 ? 3 : soft);
    gamma = gamma ? 1 : 0;
    if (soft == int(g_soft) && gamma == int(g_gamma)) return;
    InterlockedExchange(&g_soft, soft);
    InterlockedExchange(&g_gamma, gamma);
    static const char* const softs[] = {"off (one tap, as the game)", "light (a box 2 texels wide)", "medium (4 texels)",
                                        "strong (6 texels)"};
    RLOG(CH_D3D, L_INFO, "shader copies: soft shadows %s, gamma-correct light %s", softs[soft], gamma ? "on" : "off");
}

void shaderfx_summary() {
    if (!g_found_shadow && !g_found_light && !g_unmatched) return;
    RLOG(CH_D3D, L_INFO,
         "shader copies: %ld shaders with the shadow test (%ld others left as they were), %ld with N.L for the gamma; "
         "%ld copies made (%ld failed), %ld draws' shaders swapped",
         g_found_shadow, g_unmatched, g_found_light, g_built, g_build_failed, long(g_swaps));
}

}  // namespace rol
