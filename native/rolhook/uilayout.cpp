// Interface scale, engine side of the ui_scale mod: [ui] layout_scale > 100, the windows' layouts read from the
// overlay of scaled variants (files.cpp). Both hooks below act only once the overlay has served a file, so a
// missing variant leaves the game as it is.
//
// Fonts. Text asks for a font by nominal size: FONT size="18" in the layouts, the sizes of fonts.xml's lookups and
// many constants in the code (a control's default 12, timers' 14...). The font manager (0x01464C50) answers with
// the closest of the fonts fonts.xml lists (0x00EA9F20). Scaling the sizes in the data (ui_scale 0.5) left every
// size the code asks for on its 100 % font, matched to whatever scaled font came closest. So the sizes stay
// nominal and the font objects themselves are made bigger: 0x00EA6B40 — called once per FONT of fonts.xml from the
// registration 0x00EA9C90, stdcall (size, tex_size, style), edi = result, ebx = name — gets size and tex_size times
// the scale. The registered entry keeps its nominal size (+0x18) for matching; GDI rasterizes the glyph atlas at
// the scaled size (lfHeight = -size, 0x00E6DA30) and the text metrics follow it (GetTextMetrics in the font).
//
// Sprites. UIRenderMgr's sprite (0x00F72BB0, ~140 callers: icons, buttons, frames) maps a texel rectangle of an
// atlas onto a screen rectangle, at 100 % mostly 1:1. Magnified with bilinear filtering, the edge pixels sample up
// to half a texel outside the rectangle: the atlases' magenta separators and grid lines show as square frames
// around round icons. At 0x00F732E2, once the quad's UVs are computed, the rectangle is pulled in by
// d = 0.5 (W - T) / (W - 1) texels on each side (T texels on W pixels), so the edge pixels sample the edge texels'
// centres; 0 at 1:1 and when minified.
//
// Vertical expanders. EXPANDER vert="1" with three pieces (top cap, middle, bottom cap; the campaign map's tooltips,
// the vertical scroll bars, the transport and growing bars of uimain.xml) is drawn by 0x00F73900 at 1:1 in every
// direction: each piece's texel width is set to the expander's width in pixels (right = left + W, so a scaled,
// wider expander ran past its atlas cell into the neighbours - the tooltip frame showed the atlas' icon column and a
// second frame), the caps keep their texel heights and the middle is tiled at its texel height (0x00F77880). With
// the scaled layouts in use, h_vexpander draws the same three pieces magnified: W / scale texels across, caps and
// middle tiles at their texel heights times the scale (the caps shrunk together when they would not fit). Register
// arguments: ecx = the screen rectangle (int16 left, right, top, bottom), eax / ebx = texel offsets added to every
// piece's rectangle (top/bottom, left/right), edi = the pieces (TEX 0x40 bytes each: top +0x00, middle +0x40, bottom
// +0x80, texel rectangle int16 at +0x34); stack: two values passed on to the sprite, a flag that counts a layer;
// ret 0xC. Pieces are drawn by the sprite 0x00F72BB0 (stdcall: UIRenderMgr, TEX, x, y, w, h, a0, a1, layer flag).
//
// Addresses are for legends.exe of patch 2.5, image base 0x400000; the code is checked before it is patched.
#include "rolhook.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

namespace rol {
namespace {

constexpr uintptr_t FN_FONT_CREATE = 0x00EA6B40;
const unsigned char kFontSig[8] = {0x83, 0xEC, 0x14,          // sub esp, 0x14
                                   0x66, 0x83, 0x7B, 0x08, 0x00};   // cmp word ptr [ebx + 8], 0
// in the sprite, between "push edx" (v0 for the first UV) and the first UV push (call 0x00464C50)
constexpr uintptr_t AT_SPRITE_UV = 0x00F732E2;
const unsigned char kUvSig[12] = {0xF3, 0x0F, 0x11, 0x44, 0x24, 0x38,    // movss [esp + 0x38], xmm0   (u0)
                                  0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x28};   // movss [esp + 0x28], xmm1   (v1)

constexpr uintptr_t FN_VEXPANDER = 0x00F73900;
constexpr uintptr_t FN_HEXPANDER = 0x00F73730;
const unsigned char kHexpSig[5] = {0x83, 0xEC, 0x14,   // sub esp, 0x14
                                   0x85, 0xC0};        // test eax, eax
constexpr uintptr_t FN_NINE_EXPANDER = 0x00F73AD0;
const unsigned char kNineSig[8] = {0x83, 0xEC, 0x14,         // sub esp, 0x14
                                   0x55,                     // push ebp
                                   0x8B, 0x6C, 0x24, 0x24};  // mov ebp, [esp + 0x24]
const unsigned char kVexpSig[5] = {0x83, 0xEC, 0x10,   // sub esp, 0x10
                                   0x85, 0xC9};        // test ecx, ecx
constexpr uintptr_t FN_SPRITE = 0x00F72BB0;
constexpr uintptr_t UI_RENDER_MGR = 0x01461DA0;
constexpr uintptr_t UI_LAYER_COUNTER = 0x014649F4;
typedef void(__stdcall* Sprite_t)(void* mgr, void* piece, int x, int y, int w, int h, int a0, int a1, int layer);

void* g_font_tramp = nullptr;
void* g_uv_tramp = nullptr;
void* g_vexp_tramp = nullptr;
void* g_hexp_tramp = nullptr;
void* g_nine_tramp = nullptr;
volatile bool g_active = false;   // the overlay served a file: the scaled layouts are in use
int g_pct = 100;
int g_fonts = 0;
long long g_quads = 0, g_inset = 0, g_vexp = 0, g_hexp = 0, g_nine = 0;

int scaled(int v) {   // as layout.scaled(): v * pct / 100 rounded half away from zero
    return int(std::floor(double(v) * g_pct / 100.0 + 0.5));
}

void __stdcall on_font_create(int* args) {   // args[0] size, args[1] tex_size, args[2] style
    if (!g_active || args[0] <= 0) return;
    const int size = args[0], tex = args[1];
    args[0] = scaled(size);
    if (tex > 0) args[1] = scaled(tex);
    ++g_fonts;
    RLOG(CH_UI, L_INFO, "interface scale %d%%: font %d px (style 0x%x) rasterized at %d px", g_pct, size, args[2],
         args[0]);
}

__declspec(naked) void h_font_create() {
    __asm {
        pushad
        lea eax, [esp + 36]   // the arguments: above the 32 bytes of pushad and the return address
        push eax
        call on_font_create
        popad
        jmp dword ptr [g_font_tramp]
    }
}

// a..b: the UV range of one axis (b < a when the picture is mirrored), px: its length on screen, inv: 1 / texture side
// as the sprite was told (an HD replacement's original side), hd: the replacement's texels per such texel
void inset(float& a, float& b, int px, float inv, float hd) {
    if (px < 2 || !(inv > 0.0f) || !(hd > 0.0f)) return;
    const float texels = std::fabs(b - a) / inv * hd;   // real texels under the picture
    if (!(texels > 0.0f) || float(px) <= texels + 0.01f) return;   // 1:1 or minified
    float d = 0.5f * (float(px) - texels) / float(px - 1);
    if (d > 0.5f * texels) d = 0.5f * texels;
    d *= inv / hd;
    if (b >= a) {
        a += d;
        b -= d;
    } else {
        a -= d;
        b += d;
    }
}

// r: xmm0..xmm3 at the hook = u0, v1, 1 / texture height, 1 / texture width; sp: esp at the hook. The sprite's
// frame there: [sp] v0 (the pushed argument of the first UV), [sp + 0x84] v0, [sp + 0x2C] u1, and the screen
// rectangle after clipping as int16 left, right, top, bottom at [sp + 0x40].
void __stdcall on_sprite_uv(float* r, uint8_t* sp) {
    if (!g_active) return;
    ++g_quads;
    const int16_t* rc = reinterpret_cast<const int16_t*>(sp + 0x40);
    float& u1 = *reinterpret_cast<float*>(sp + 0x2C);
    float& v0_arg = *reinterpret_cast<float*>(sp);
    float& v0 = *reinterpret_cast<float*>(sp + 0x84);
    const float u0_was = r[0], v1_was = r[1];
    inset(r[0], u1, rc[1] - rc[0], r[3], texture_hd_factor(0));
    float v = v0;
    inset(v, r[1], rc[3] - rc[2], r[2], texture_hd_factor(1));
    v0 = v0_arg = v;
    if (r[0] != u0_was || r[1] != v1_was) ++g_inset;
}

__declspec(naked) void h_sprite_uv() {
    __asm {
        sub esp, 16
        movss dword ptr [esp], xmm0
        movss dword ptr [esp + 4], xmm1
        movss dword ptr [esp + 8], xmm2
        movss dword ptr [esp + 12], xmm3
        pushad
        lea eax, [esp + 48]   // esp at the hook
        push eax
        lea eax, [esp + 36]   // the saved xmm0..xmm3
        push eax
        call on_sprite_uv
        popad
        movss xmm0, dword ptr [esp]
        movss xmm1, dword ptr [esp + 4]
        movss xmm2, dword ptr [esp + 8]
        movss xmm3, dword ptr [esp + 12]
        add esp, 16
        jmp dword ptr [g_uv_tramp]
    }
}

struct TexRect {   // a TEX piece's texel rectangle, int16 at +0x34
    int16_t l, r, t, b;
};

TexRect& tex_rect(uint8_t* piece) { return *reinterpret_cast<TexRect*>(piece + 0x34); }

void draw_piece(uint8_t* piece, const TexRect& texels, int x, int y, int w, int h, int a0, int a1) {
    if (w <= 0 || h <= 0) return;
    TexRect& r = tex_rect(piece);
    const TexRect saved = r;
    r = texels;
    reinterpret_cast<Sprite_t>(FN_SPRITE)(reinterpret_cast<void*>(UI_RENDER_MGR), piece, x, y, w, h, a0, a1, 0);
    r = saved;
}

void __stdcall vexpander(const int16_t* rect, int offy, int offx, uint8_t* pieces, int a0, int a1, int layer) {
    if (!rect || !pieces) return;
    if (layer) *reinterpret_cast<volatile int*>(UI_LAYER_COUNTER) += 2;
    const double s = g_pct / 100.0;
    const int L = rect[0], R = rect[1], T = rect[2], B = rect[3];
    const int W = R - L;
    if (W <= 0 || B <= T) return;
    ++g_vexp;
    uint8_t* top = pieces;
    uint8_t* mid = pieces + 0x40;
    uint8_t* bot = pieces + 0x80;
    const int tw = std::max(1, int(std::lround(W / s)));   // texels across: what 100 % shows of the pieces
    auto texels = [&](uint8_t* p) {
        TexRect r = tex_rect(p);
        r.l = int16_t(r.l + offx);
        r.t = int16_t(r.t + offy);
        r.b = int16_t(r.b + offy);
        r.r = int16_t(r.l + tw);
        return r;
    };
    const TexRect tt = texels(top), mt = texels(mid), bt = texels(bot);
    const int th = tt.b - tt.t, mh = mt.b - mt.t, bh = bt.b - bt.t;
    int ths = th > 0 ? int(std::lround(th * s)) : 0;
    int bhs = bh > 0 ? int(std::lround(bh * s)) : 0;
    if (ths + bhs > B - T) {   // the caps do not fit: shrunk alike
        const double f = double(B - T) / double(ths + bhs);
        ths = int(ths * f);
        bhs = int(bhs * f);
    }
    draw_piece(top, tt, L, T, W, ths, a0, a1);
    draw_piece(bot, bt, L, B - bhs, W, bhs, a0, a1);
    const int y0 = T + ths, y1 = B - bhs;   // the middle, tiled: mh texels per mh * s pixels, the last tile cut
    if (mh <= 0) return;
    for (int k = 0;; ++k) {
        const int ya = y0 + int(std::lround(k * mh * s));
        if (ya >= y1) break;
        int yb = y0 + int(std::lround((k + 1) * mh * s));
        TexRect r = mt;
        if (yb > y1) {
            yb = y1;
            r.b = int16_t(r.t + std::max(1, std::min(mh, int(std::lround((yb - ya) / s)))));
        }
        draw_piece(mid, r, L, ya, W, yb - ya, a0, a1);
    }
}

__declspec(naked) void h_vexpander() {
    __asm {
        cmp byte ptr [g_active], 0
        je original
        push dword ptr [esp + 12]   // layer flag
        push dword ptr [esp + 12]   // a1
        push dword ptr [esp + 12]   // a0
        push edi                    // pieces
        push ebx                    // texel offset, left / right
        push eax                    // texel offset, top / bottom
        push ecx                    // screen rectangle
        call vexpander
        ret 12
    original:
        jmp dword ptr [g_vexp_tramp]
    }
}

// The texel rectangle of a piece with the texel offsets added (as the engine does before drawing).
TexRect texels_of(uint8_t* p, int offx, int offy) {
    TexRect r = tex_rect(p);
    r.l = int16_t(r.l + offx);
    r.r = int16_t(r.r + offx);
    r.t = int16_t(r.t + offy);
    r.b = int16_t(r.b + offy);
    return r;
}

bool present(uint8_t* p) {   // the three-piece expander skips a piece whose +0x3C is negative (no texture)
    return *reinterpret_cast<const int*>(p + 0x3C) >= 0;
}

int scaled_len(int texels, double s) { return texels > 0 ? int(std::lround(texels * s)) : 0; }

// Two caps on a span: scaled, shrunk alike when they do not fit.
void fit_caps(int& a, int& b, int span) {
    if (a + b > span && a + b > 0) {
        const double f = double(std::max(0, span)) / double(a + b);
        a = int(a * f);
        b = int(b * f);
    }
}

// A piece tiled across [x0, x1) at y, h px tall: |tw| texels per |tw| * s px, the last tile cut (mirrored pieces too).
void tile_h(uint8_t* piece, const TexRect& tr, int x0, int x1, int y, int h, double s, int a0, int a1) {
    const int tw = tr.r - tr.l, aw = std::abs(tw);
    if (!aw || x1 <= x0 || h <= 0) return;
    for (int k = 0;; ++k) {
        const int xa = x0 + int(std::lround(k * aw * s));
        if (xa >= x1) break;
        int xb = x0 + int(std::lround((k + 1) * aw * s));
        TexRect r = tr;
        if (xb > x1) {
            xb = x1;
            const int n = std::max(1, std::min(aw, int(std::lround((xb - xa) / s))));
            r.r = int16_t(r.l + (tw > 0 ? n : -n));
        }
        draw_piece(piece, r, xa, y, xb - xa, h, a0, a1);
    }
}

// A piece tiled down [y0, y1) at x, w px wide: |th| texels per |th| * s px, the last tile cut.
void tile_v(uint8_t* piece, const TexRect& tr, int x, int w, int y0, int y1, double s, int a0, int a1) {
    const int th = tr.b - tr.t, ah = std::abs(th);
    if (!ah || y1 <= y0 || w <= 0) return;
    for (int k = 0;; ++k) {
        const int ya = y0 + int(std::lround(k * ah * s));
        if (ya >= y1) break;
        int yb = y0 + int(std::lround((k + 1) * ah * s));
        TexRect r = tr;
        if (yb > y1) {
            yb = y1;
            const int n = std::max(1, std::min(ah, int(std::lround((yb - ya) / s))));
            r.b = int16_t(r.t + (th > 0 ? n : -n));
        }
        draw_piece(piece, r, x, ya, w, yb - ya, a0, a1);
    }
}

// Both ways (the nine-piece expander's centre): rows of |th| * s px, each tiled across.
void tile_2d(uint8_t* piece, const TexRect& tr, int x0, int x1, int y0, int y1, double s, int a0, int a1) {
    const int th = tr.b - tr.t, ah = std::abs(th);
    if (!ah || y1 <= y0 || x1 <= x0) return;
    for (int k = 0;; ++k) {
        const int ya = y0 + int(std::lround(k * ah * s));
        if (ya >= y1) break;
        int yb = y0 + int(std::lround((k + 1) * ah * s));
        TexRect r = tr;
        if (yb > y1) {
            yb = y1;
            const int n = std::max(1, std::min(ah, int(std::lround((yb - ya) / s))));
            r.b = int16_t(r.t + (th > 0 ? n : -n));
        }
        tile_h(piece, r, x0, x1, ya, yb - ya, s, a0, a1);
    }
}

// Horizontal three-piece expander (0x00F73730), magnified: left cap, right cap and the middle's tiles |w| * s px wide,
// all stretched to the height as the engine does. Pieces: left +0x00, middle +0x40, right +0x80.
void hexp3(int L, int R, int T, int B, uint8_t* pieces, int offx, int offy, int a0, int a1, double s) {
    if (!pieces || R <= L || B <= T) return;
    uint8_t* left = pieces;
    uint8_t* mid = pieces + 0x40;
    uint8_t* right = pieces + 0x80;
    const TexRect lt = texels_of(left, offx, offy), mt = texels_of(mid, offx, offy), rt = texels_of(right, offx, offy);
    int lw = present(left) ? scaled_len(std::abs(lt.r - lt.l), s) : 0;
    int rw = present(right) ? scaled_len(std::abs(rt.r - rt.l), s) : 0;
    fit_caps(lw, rw, R - L);
    if (present(left)) draw_piece(left, lt, L, T, lw, B - T, a0, a1);
    if (present(right)) draw_piece(right, rt, R - rw, T, rw, B - T, a0, a1);
    if (present(mid)) tile_h(mid, mt, L + lw, R - rw, T, B - T, s, a0, a1);
}

// eax = the screen rectangle (int16 left, right, top, bottom), ecx = texel offset top/bottom, ebx = texel offset
// left/right, edi = the pieces; stack a0, a1, layer flag; ret 0xC
void __stdcall hexpander(const int16_t* rect, int offy, int offx, uint8_t* pieces, int a0, int a1, int layer) {
    if (!rect || !pieces) return;
    if (layer) *reinterpret_cast<volatile int*>(UI_LAYER_COUNTER) += 2;
    ++g_hexp;
    hexp3(rect[0], rect[1], rect[2], rect[3], pieces, offx, offy, a0, a1, g_pct / 100.0);
}

__declspec(naked) void h_hexpander() {
    __asm {
        cmp byte ptr [g_active], 0
        je original
        push dword ptr [esp + 12]   // layer flag
        push dword ptr [esp + 12]   // a1
        push dword ptr [esp + 12]   // a0
        push edi                    // pieces
        push ebx                    // texel offset, left / right
        push ecx                    // texel offset, top / bottom
        push eax                    // screen rectangle
        call hexpander
        ret 12
    original:
        jmp dword ptr [g_hexp_tramp]
    }
}

// Nine-piece expander (0x00F73AD0), magnified: stdcall (UIRenderMgr, rectangle, pieces, a0, a1, texel offset
// left/right, top/bottom, layer flag). Pieces 0x40 apart: top-left, top, top-right, left, centre, right, bottom-left,
// bottom, bottom-right. The engine: top row (three-piece, the top-left's height), left edge tiled down (its width),
// centre tiled both ways, right edge, bottom row (the bottom-left's height) - here with heights, widths and tiles
// times the scale.
void __stdcall nine_expander(void*, const int16_t* rect, uint8_t* pieces, int a0, int a1, int offx, int offy,
                             int layer) {
    if (!rect || !pieces) return;
    if (layer) *reinterpret_cast<volatile int*>(UI_LAYER_COUNTER) += 2;
    ++g_nine;
    const double s = g_pct / 100.0;
    const int L = rect[0], R = rect[1], T = rect[2], B = rect[3];
    uint8_t* p = pieces;
    const TexRect tl = texels_of(p, offx, offy), le = texels_of(p + 0xC0, offx, offy),
                  ce = texels_of(p + 0x100, offx, offy), re = texels_of(p + 0x140, offx, offy),
                  bl = texels_of(p + 0x180, offx, offy);
    int top = scaled_len(std::abs(tl.b - tl.t), s), bottom = scaled_len(std::abs(bl.b - bl.t), s);
    int lw = scaled_len(std::abs(le.r - le.l), s), rw = scaled_len(std::abs(re.r - re.l), s);
    fit_caps(top, bottom, B - T);
    fit_caps(lw, rw, R - L);
    hexp3(L, R, T, T + top, p, offx, offy, a0, a1, s);
    tile_v(p + 0xC0, le, L, lw, T + top, B - bottom, s, a0, a1);
    tile_2d(p + 0x100, ce, L + lw, R - rw, T + top, B - bottom, s, a0, a1);
    tile_v(p + 0x140, re, R - rw, rw, T + top, B - bottom, s, a0, a1);
    hexp3(L, R, B - bottom, B, p + 0x180, offx, offy, a0, a1, s);
}

__declspec(naked) void h_nine_expander() {
    __asm {
        cmp byte ptr [g_active], 0
        je original
        jmp nine_expander   // same stack, same ret 0x20
    original:
        jmp dword ptr [g_nine_tramp]
    }
}

// Pictures the code draws at their natural size - the texel rectangle's size, at a place it works out itself - stay
// 1:1 among scaled windows. Their calls of the sprite go through a thunk: width and height times the scale, anchored
// at the top left. The campaign scripts' add_sprite / add_sprite_alpha (the army grid's unit pictures; ui_scale
// scales the grid's blocks in the scripts) and the list bullet, drawn by 0x00F734D0 (a picture at its natural size
// at a point; its other callers keep theirs) - the code laying the bullet out sees its size scaled (kWordSites).
uintptr_t g_fn_sprite = FN_SPRITE;
long long g_natural = 0;
int g_natural_sites = 0;

void __stdcall magnify_natural(int* args, uintptr_t only_tex) {   // args: UIRenderMgr, TEX, x, y, w, h, a0, a1, layer
    if (!g_active || (only_tex && uintptr_t(args[1]) != only_tex)) return;
    args[4] = int(std::lround(args[4] * (g_pct / 100.0)));
    args[5] = int(std::lround(args[5] * (g_pct / 100.0)));
    ++g_natural;
}

__declspec(naked) void h_natural_sprite() {
    __asm {
        lea eax, [esp + 4]   // the arguments, above the return address (eax, ecx, edx are free at a call)
        push 0
        push eax
        call magnify_natural
        jmp dword ptr [g_fn_sprite]
    }
}

__declspec(naked) void h_bullet_sprite() {
    __asm {
        lea eax, [esp + 4]
        push 0x01463700   // only the bullet's TEX
        push eax
        call magnify_natural
        jmp dword ptr [g_fn_sprite]
    }
}

struct NaturalSite {
    uintptr_t call;   // a call of the sprite (E8 rel32)
    void* hook;
    const char* what;
};
const NaturalSite kNaturalSites[] = {
    {0x00B51748, (void*)h_natural_sprite, "add_sprite"},
    {0x00B5185A, (void*)h_natural_sprite, "add_sprite_alpha"},
    {0x00F73503, (void*)h_bullet_sprite, "the list bullet"},
};

int install_natural_sites() {
    int ok = 0;
    for (const NaturalSite& s : kNaturalSites) {
        unsigned char expect[5] = {0xE8, 0, 0, 0, 0}, call[5] = {0xE8, 0, 0, 0, 0};
        *reinterpret_cast<int32_t*>(expect + 1) = int32_t(FN_SPRITE - (s.call + 5));
        *reinterpret_cast<int32_t*>(call + 1) = int32_t(reinterpret_cast<uintptr_t>(s.hook) - (s.call + 5));
        if (patch_code(s.call, expect, call, 5))
            ++ok;
        else
            RLOG(CH_UI, L_WARN, "interface scale: %s at 0x%08X not patched (code differs)", s.what, unsigned(s.call));
    }
    return ok;
}

// Natural sizes the code keeps for its own layout: a picture's width or height, taken from its TEX into a 16-bit
// register, is scaled there before the code stores or uses it, so the layout built on it grows with the windows.
//   hero buttons (HEROESWIN, top left): 0x00987900 takes the hero icons' sizes from their TEX - two kinds, width and
//     height each, +0x764 .. +0x770 of the window - and lays the column out by them with the XML's (scaled)
//     distances; the icons stayed 38 px, the step 38 + 8
//   numbers on count buttons (0x00959960, numeral_0..9 of uimain.xml): numeral_0's width and height make the
//     number's width, its centring in the button and the step between the digits
//   icons inside text ($ICON..., the text's segments at [0x01268F9C], 0x38 bytes, the TEX at +0x34): line breaking
//     (0x00F7B060, twice), measuring (0x00F7B210) and drawing (0x00F7B9C0: width and height, then the text goes on
//     after width + 4) - all four, so the lines are measured as they are drawn
//   the bullet of list lines (help boxes): its height centres it, its width indents the text, in drawing and measuring
// Each site's detour enters a generated "push index; jmp word_thunk"; the thunk scales the site's registers and
// returns into the site's trampoline (the moved instructions, then back).
enum : int { R_EBX = 1 << 4, R_EDX = 1 << 5, R_ECX = 1 << 6, R_EAX = 1 << 7 };   // bits = the pushad block's slots
struct WordSite {
    uintptr_t at;
    unsigned char sig[9];
    int len;
    int regs;
    const char* what;
};
const WordSite kWordSites[] = {
    {0x00987AB6, {0x0F, 0xBF, 0xD1, 0x89, 0x96, 0x70, 0x07, 0x00, 0x00}, 9, R_ECX, "hero icon width"},
    {0x00987AC9, {0x0F, 0xBF, 0xD1, 0x89, 0x96, 0x6C, 0x07, 0x00, 0x00}, 9, R_ECX, "hero icon height"},
    {0x00987AEA, {0x0F, 0xBF, 0xD1, 0x89, 0x96, 0x68, 0x07, 0x00, 0x00}, 9, R_ECX, "hero portrait width"},
    {0x00987AFB, {0x0F, 0xBF, 0xD1, 0x89, 0x96, 0x64, 0x07, 0x00, 0x00}, 9, R_ECX, "hero portrait height"},
    // movsx ebx, cx (the height); mov ecx, [esp + 0x54]; dx = the width, taken by movsx edi, dx right after
    {0x00959DD1, {0x0F, 0xBF, 0xD9, 0x8B, 0x4C, 0x24, 0x54}, 7, R_ECX | R_EDX, "digit size"},
    {0x00F7B0DA, {0x0F, 0xBF, 0xC2, 0xBA, 0xFC, 0xFF, 0xFF, 0xFF}, 8, R_EDX, "text icon width (line breaking)"},
    {0x00F7B1E1, {0x0F, 0xBF, 0xC3, 0x8B, 0x5C, 0x24, 0x14}, 7, R_EBX, "text icon width (line breaking, rest)"},
    {0x00F7B915, {0x0F, 0xBF, 0xC2, 0x8B, 0x54, 0x24, 0x2C}, 7, R_EDX, "text icon width (measuring)"},
    {0x00F7BAC0, {0x0F, 0xBF, 0xF0, 0x0F, 0xB7, 0x41, 0x3A}, 7, R_EAX, "text icon width (drawing)"},
    // movsx eax, ax; mov [esp + 0x4c], eax - between "test esi, esi" and its je: the thunk keeps the flags
    {0x00F7BACD, {0x0F, 0xBF, 0xC0, 0x89, 0x44, 0x24, 0x4C}, 7, R_EAX, "text icon height (drawing)"},
    // the bullet of list lines (interface.xml BULLETS, a global TEX at 0x01463700, 7 x 7): drawn by 0x00F7AB00 (two
    // branches: its height centres it on the line, its width - read twice - indents the text after it) and measured
    // by 0x00F7BC80 / 0x00F7BDC0 (width + the bullet gap [0x0129A918]); the picture itself: kBulletSite
    {0x00F7AC8C, {0x0F, 0xBF, 0xD1, 0x89, 0x54, 0x24, 0x1C}, 7, R_ECX, "bullet height (drawing)"},
    {0x00F7ACE4, {0x0F, 0xBF, 0xC9, 0x66, 0x2B, 0xC2}, 6, R_ECX, "bullet width (drawing, the rest of the line)"},
    {0x00F7ACF2, {0x0F, 0xBF, 0xD0, 0x2B, 0xE9}, 5, R_EAX, "bullet width (drawing, the text's x)"},
    {0x00F7AEBF, {0x0F, 0xBF, 0xCA, 0x89, 0x4C, 0x24, 0x20}, 7, R_EDX, "bullet height (drawing, 2)"},
    {0x00F7AF17, {0x0F, 0xBF, 0xC9, 0x66, 0x2B, 0xC2}, 6, R_ECX, "bullet width (drawing 2, the rest of the line)"},
    {0x00F7AF25, {0x0F, 0xBF, 0xD0, 0x2B, 0xF1}, 5, R_EAX, "bullet width (drawing 2, the text's x)"},
    {0x00F7BD2F, {0x0F, 0xBF, 0xEA, 0x03, 0x2D, 0x18, 0xA9, 0x29, 0x01}, 9, R_EDX, "bullet width (measuring)"},
    {0x00F7BE9E, {0x0F, 0xBF, 0xCA, 0x03, 0x0D, 0x18, 0xA9, 0x29, 0x01}, 9, R_EDX, "bullet width (measuring 2)"},
    {0x00F7BFE8, {0x0F, 0xBF, 0xC9, 0x03, 0x0D, 0x18, 0xA9, 0x29, 0x01}, 9, R_ECX, "bullet width (measuring 3)"},
};
constexpr int kWordCount = int(sizeof(kWordSites) / sizeof(kWordSites[0]));
void* g_word_tramp[kWordCount] = {};
uint8_t* g_word_entries = nullptr;
int g_word_sites = 0;
long long g_words = 0;

uint32_t scaled_word(uint32_t reg) {
    const int v = int16_t(reg & 0xFFFF);
    return (reg & 0xFFFF0000u) | uint16_t(int16_t(std::lround(v * (g_pct / 100.0))));
}

// f: the pushad block (edi, esi, ebp, esp, ebx, edx, ecx, eax), then the flags, then the site's index - replaced by
// the trampoline the thunk returns into
void __stdcall scale_site(uint32_t* f) {
    const int i = int(f[9]);
    if (g_active) {
        for (int slot = 4; slot < 8; ++slot)
            if (kWordSites[i].regs & (1 << slot)) f[slot] = scaled_word(f[slot]);
        ++g_words;
    }
    f[9] = uint32_t(reinterpret_cast<uintptr_t>(g_word_tramp[i]));
}

__declspec(naked) void word_thunk() {
    __asm {
        pushfd
        pushad
        mov eax, esp
        push eax
        call scale_site
        popad
        popfd
        ret   // into the trampoline
    }
}

int install_word_sites() {
    g_word_entries = static_cast<uint8_t*>(VirtualAlloc(nullptr, kWordCount * 10, MEM_COMMIT | MEM_RESERVE,
                                                        PAGE_EXECUTE_READWRITE));
    if (!g_word_entries) return 0;
    for (int i = 0; i < kWordCount; ++i) {
        uint8_t* e = g_word_entries + i * 10;
        e[0] = 0x68;   // push i
        *reinterpret_cast<int32_t*>(e + 1) = i;
        e[5] = 0xE9;   // jmp word_thunk
        *reinterpret_cast<int32_t*>(e + 6) =
            int32_t(reinterpret_cast<uintptr_t>(&word_thunk) - (reinterpret_cast<uintptr_t>(e) + 10));
    }
    FlushInstructionCache(GetCurrentProcess(), g_word_entries, kWordCount * 10);
    int ok = 0;
    for (int i = 0; i < kWordCount; ++i) {
        const WordSite& s = kWordSites[i];
        g_word_tramp[i] = detour(s.at, s.sig, s.len, g_word_entries + i * 10);
        if (g_word_tramp[i])
            ++ok;
        else
            RLOG(CH_UI, L_WARN, "interface scale: %s at 0x%08X not patched (code differs)", s.what, unsigned(s.at));
    }
    return ok;
}

#ifndef ROLHOOK_PLAYER   // a developer tool: not in the players' build (player_stubs.cpp)
// [debug] log_sprites=1: every distinct interface picture the sprite (0x00F72BB0) draws - texture, texel rectangle,
// screen rectangle - once, in ui.log, at any interface scale. Two games at 100 and 150 % side by side show what the
// layouts do not scale (code-placed pictures: their screen size stays). Entry: stdcall (UIRenderMgr, TEX, x, y, w,
// h, a0, a1, layer), TEX texel rectangle int16 at +0x34. The texture is known only later in the same call, when the
// sprite asks its size (gfx.cpp logical sizes, 0x00F73261: the texture manager's record) - spritelog_texture()
// completes the pending line there; a sprite clipped away before that is not logged.
const unsigned char kSpriteSig[11] = {0x83, 0xEC, 0x68,                                // sub esp, 0x68
                                      0x83, 0xBC, 0x24, 0x8C, 0x00, 0x00, 0x00, 0x00};   // cmp [esp+0x8C], 0
void* g_sprite_tramp = nullptr;
std::unordered_set<std::string> g_sprites_seen;
int g_sprite_lines = 0;
struct PendingSprite {
    bool on = false;
    int16_t rc[4];
    int x, y, w, h, ox, oy;   // ox, oy: UIRenderMgr's offset (+0x186C / +0x1870) the sprite adds to x, y
    uintptr_t caller, outer;  // the sprite's return address; the next one up the stack (the helper's caller)
} g_pending;

// The first return address into legends.exe above the sprite's arguments: a dword of the stack that points right
// after a direct call (E8) - the caller of a drawing helper such as 0x00F73480 / 0x00F734D0.
uintptr_t outer_caller(const int* args) {
    __try {
        for (int k = 9; k < 40; ++k) {
            const uintptr_t v = uintptr_t(args[k]);
            if (v >= 0x00401005 && v < 0x01100000 && *reinterpret_cast<const uint8_t*>(v - 5) == 0xE8) return v;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

bool read_int(uintptr_t a, int* v) {
    __try {
        *v = *reinterpret_cast<const int*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __stdcall on_sprite_entry(const int* args) {   // args[-1]: the return address
    g_pending.on = false;
    if (g_sprite_lines >= 20000) return;
    const uintptr_t piece = uintptr_t(args[1]);
    int l = 0, t = 0;
    if (!piece || !read_int(piece + 0x34, &l) || !read_int(piece + 0x38, &t)) return;
    g_pending.rc[0] = int16_t(l & 0xFFFF), g_pending.rc[1] = int16_t(l >> 16);
    g_pending.rc[2] = int16_t(t & 0xFFFF), g_pending.rc[3] = int16_t(t >> 16);
    g_pending.x = args[2], g_pending.y = args[3], g_pending.w = args[4], g_pending.h = args[5];
    g_pending.ox = g_pending.oy = 0;
    read_int(uintptr_t(args[0]) + 0x186C, &g_pending.ox);
    read_int(uintptr_t(args[0]) + 0x1870, &g_pending.oy);
    g_pending.caller = uintptr_t(args[-1]);
    g_pending.outer = outer_caller(args);
    g_pending.on = true;
}

__declspec(naked) void h_sprite_entry() {
    __asm {
        pushad
        lea eax, [esp + 36]   // the arguments: above the 32 bytes of pushad and the return address
        push eax
        call on_sprite_entry
        popad
        jmp dword ptr [g_sprite_tramp]
    }
}
#endif   // ROLHOOK_PLAYER

}  // namespace

#ifndef ROLHOOK_PLAYER
void spritelog_texture(uintptr_t record) {
    if (!g_pending.on) return;
    g_pending.on = false;
    const int16_t* rc = g_pending.rc;
    char key[400];
    snprintf(key, sizeof(key), "%s | texels %d,%d %dx%d | screen %dx%d", texture_name(record_texture(record)).c_str(),
             rc[0], rc[2], rc[1] - rc[0], rc[3] - rc[2], g_pending.w, g_pending.h);
    if (!g_sprites_seen.insert(key).second) return;
    ++g_sprite_lines;
    RLOG(CH_UI, L_INFO, "sprite %s at %d,%d (offset %d,%d) from 0x%08X < 0x%08X", key, g_pending.x, g_pending.y,
         g_pending.ox, g_pending.oy, unsigned(g_pending.caller), unsigned(g_pending.outer));
}

void spritelog_install(HMODULE exe) {
    if (!config_int(L"debug", L"log_sprites", 0, true) || reinterpret_cast<uintptr_t>(exe) != 0x00400000) return;
    g_sprite_tramp = detour(FN_SPRITE, kSpriteSig, sizeof(kSpriteSig), (void*)h_sprite_entry);
    RLOG(CH_UI, g_sprite_tramp ? L_INFO : L_ERROR, "sprite log: %s", g_sprite_tramp ? "on (distinct interface pictures "
         "to ui.log)" : "FAILED (code differs)");
}
#endif   // ROLHOOK_PLAYER

namespace {

// The Documents folder as the shell has it (redirected ones too: OneDrive, another drive), where the game keeps
// My Games\Rise Of Legends; read from the registry since shell32 must not be called in DllMain.
std::wstring documents_dir() {
    std::wstring docs;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders",
                      0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        wchar_t raw[MAX_PATH * 2] = {};
        DWORD type = 0, size = DWORD(sizeof(raw) - sizeof(wchar_t));
        if (RegQueryValueExW(k, L"Personal", nullptr, &type, reinterpret_cast<BYTE*>(raw), &size) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            wchar_t full[MAX_PATH * 2] = {};
            if (ExpandEnvironmentStringsW(raw, full, MAX_PATH * 2)) docs = full;
        }
        RegCloseKey(k);
    }
    if (docs.empty()) {
        wchar_t home[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) docs = std::wstring(home) + L"\\Documents";
    }
    return docs;
}

std::wstring personal_value(const std::wstring& ini, const wchar_t* key) {
    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"DEFAULT", key, L"", buf, MAX_PATH, ini.c_str());
    return buf;
}

// graphics_resolution="W x H x bpp" of a profile (XML, the GRAPHICS element the Graphics tab saves); false if none.
bool profile_resolution(const std::wstring& path, int& w, int& h) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string text(std::min<DWORD>(GetFileSize(f, nullptr), 4u << 20), '\0');
    DWORD got = 0;
    const bool ok = !text.empty() && ReadFile(f, &text[0], DWORD(text.size()), &got, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    text.resize(got);
    const char kAttr[] = "graphics_resolution=\"";
    const size_t at = text.find(kAttr);
    return at != std::string::npos && sscanf_s(text.c_str() + at + sizeof(kAttr) - 1, "%d x %d", &w, &h) == 2 &&
           w > 0 && h > 0;
}

}  // namespace

// personal.ini (Documents\My Games\Rise Of Legends) names the profile, CurrentProfileFile, kept in a folder of its
// name under that folder or ProfilesDirectoryOverride; the profile's GRAPHICS graphics_resolution is the full-screen
// mode the Graphics tab set. Windowed (FullScreen=0) the game runs at Windowed Width x Height. Without either - the
// first start - the primary display's current mode.
int ui_screen_height(std::string& from) {
    const std::wstring docs = documents_dir();
    const std::wstring base = docs + L"\\My Games\\Rise Of Legends";
    const std::wstring ini = base + L"\\personal.ini";
    if (!docs.empty() && GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (personal_value(ini, L"FullScreen") == L"0") {
            const int w = _wtoi(personal_value(ini, L"Windowed Width").c_str());
            const int h = _wtoi(personal_value(ini, L"Windowed Height").c_str());
            if (h > 0) {
                char buf[96];
                sprintf_s(buf, "windowed %d x %d (personal.ini)", w, h);
                from = buf;
                return h;
            }
        }
        const std::wstring name = personal_value(ini, L"CurrentProfileFile");
        std::wstring dir = personal_value(ini, L"ProfilesDirectoryOverride");
        if (dir.empty()) dir = base;
        const size_t dot = name.rfind(L'.');
        int w = 0, h = 0;
        if (!name.empty() &&
            profile_resolution(dir + L"\\" + name.substr(0, dot) + L"\\" + name, w, h)) {
            char buf[160];
            sprintf_s(buf, "%d x %d (profile %s)", w, h, w2u(name.c_str()).c_str());
            from = buf;
            return h;
        }
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight > 0) {
        char buf[96];
        sprintf_s(buf, "%lu x %lu (the display: no profile with a resolution yet)", dm.dmPelsWidth, dm.dmPelsHeight);
        from = buf;
        return int(dm.dmPelsHeight);
    }
    from = "unknown (no profile, no display mode)";
    return 1080;
}

void uilayout_overlay_used() {
    if (g_active || g_pct <= 100) return;
    g_active = true;
    RLOG(CH_UI, L_INFO, "interface scale %d%%: scaled layouts in use — fonts %s, sprite UV inset %s", g_pct,
         g_font_tramp ? "rasterized at the scale" : "NOT scaled (hook off)", g_uv_tramp ? "on" : "off (hook off)");
}

void uilayout_install(HMODULE exe) {
    g_pct = g_cfg.ui_layout_scale;
    if (g_cfg.ui_layout_choice == 0)
        RLOG(CH_CORE, L_INFO, "interface scale auto: %s -> %d%%", g_cfg.ui_layout_auto.c_str(), g_pct);
    if (g_pct <= 100 || g_cfg.ui_layout_overlay.empty()) return;
    if (reinterpret_cast<uintptr_t>(exe) != 0x00400000) {
        RLOG(CH_UI, L_WARN, "interface scale: font and sprite hooks off (unknown legends.exe build)");
        return;
    }
    g_font_tramp = detour(FN_FONT_CREATE, kFontSig, sizeof(kFontSig), (void*)h_font_create);
    g_uv_tramp = detour(AT_SPRITE_UV, kUvSig, sizeof(kUvSig), (void*)h_sprite_uv);
    g_vexp_tramp = detour(FN_VEXPANDER, kVexpSig, sizeof(kVexpSig), (void*)h_vexpander);
    g_hexp_tramp = detour(FN_HEXPANDER, kHexpSig, sizeof(kHexpSig), (void*)h_hexpander);
    g_nine_tramp = detour(FN_NINE_EXPANDER, kNineSig, sizeof(kNineSig), (void*)h_nine_expander);
    g_natural_sites = install_natural_sites();
    g_word_sites = install_word_sites();
    const int sites = int(sizeof(kNaturalSites) / sizeof(kNaturalSites[0]));
    const int words = int(sizeof(kWordSites) / sizeof(kWordSites[0]));
    RLOG(CH_UI, g_font_tramp && g_uv_tramp && g_vexp_tramp && g_hexp_tramp && g_nine_tramp && g_natural_sites == sites &&
         g_word_sites == words ? L_INFO : L_ERROR, "interface scale %d%%: font hook %s, sprite UV hook %s, expander hooks "
         "%s/%s/%s (vertical, horizontal, nine pieces), natural-size pictures %d/%d, natural sizes in code %d/%d (all "
         "act once the overlay serves a file)", g_pct, g_font_tramp ? "installed" : "FAILED (code differs)",
         g_uv_tramp ? "installed" : "FAILED (code differs)", g_vexp_tramp ? "ok" : "FAILED",
         g_hexp_tramp ? "ok" : "FAILED", g_nine_tramp ? "ok" : "FAILED", g_natural_sites, sites, g_word_sites, words);
}

void uilayout_summary() {
    if (!g_font_tramp && !g_uv_tramp) return;
    RLOG(CH_UI, L_INFO, "interface scale %d%%: %s; %d fonts rasterized at the scale, %lld of %lld sprites pulled "
         "inside their atlas cell, expanders drawn magnified: %lld vertical, %lld horizontal, %lld nine-piece; %lld "
         "natural-size pictures magnified, %lld natural sizes scaled in code", g_pct,
         g_active ? "scaled layouts were in use" : "the overlay served nothing", g_fonts, g_inset, g_quads, g_vexp,
         g_hexp, g_nine, g_natural, g_words);
}

}  // namespace rol
