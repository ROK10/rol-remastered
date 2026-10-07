// Shadow map resolution ([graphics] shadow_size). The engine renders shadows into a square R32F target (depth as
// colour) of min(shadowmap_size option, cap); the cap ([[0x014651B4] + 0x18] + 0x364) is 2048 here whatever the
// options and game_features.xml say, so the "Huge (4096)" item never took effect (autotest 2026-10-03: the target
// stayed 2048x2048 with 4096 and 8192 in the profile). The size is computed in three places (legends.exe 1.0), all
// made to use N:
//   0x006DDA70  the game app's shadow-map-size method (vtable 0x010CAC90 slot 12), asked by the target setup;
//   0x0071C861  the same min() for the shaders' shadow map size parameter (texel offsets);
//   0x00EC2296  the target setup (0x00EC2240) clamps once more (cmovl) - removed.
// The depth buffer of the shadow pass is created in the same setup with the same size. 0 = as the game.
#include "rolhook.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rol {
namespace {

struct Patch {
    uintptr_t at;
    unsigned char expect[6];
    unsigned char bytes[6];
    int len;
};

bool g_patched = false;

}  // namespace

void shadow_install(HMODULE exe) {
    const int n = g_cfg.shadow_size;
    if (reinterpret_cast<uintptr_t>(exe) != 0x00400000) {
        if (n > 0) RLOG(CH_CORE, L_WARN, "shadow map size: unknown legends.exe build, setting ignored");
        return;
    }
    shadow_fit_install();
    if (n <= 0) return;
    unsigned char nb[4];
    memcpy(nb, &n, 4);
    Patch p[5] = {
        {0x006DDA79, {0xA1, 0x35, 0x2D, 0x3F, 0x01}, {0xB8, nb[0], nb[1], nb[2], nb[3]}, 5},          // mov eax, N
        {0x006DDA86, {0x7D, 0x02}, {0xEB, 0x02}, 2},                                                     // no cap
        {0x0071C869, {0x8B, 0x3D, 0x35, 0x2D, 0x3F, 0x01}, {0xBF, nb[0], nb[1], nb[2], nb[3], 0x90}, 6}, // mov edi, N
        {0x0071C87B, {0x7D, 0x06}, {0xEB, 0x06}, 2},                                                     // no cap
        {0x00EC2296, {0x0F, 0x4C, 0xE9}, {0x90, 0x90, 0x90}, 3},                                         // no cmovl
    };
    for (const auto& x : p)
        if (memcmp(reinterpret_cast<const void*>(x.at), x.expect, size_t(x.len)) != 0) {
            RLOG(CH_CORE, L_WARN, "shadow map size: code at 0x%08x differs, setting ignored", unsigned(x.at));
            return;
        }
    int ok = 0;
    for (const auto& x : p) ok += patch_code(x.at, x.expect, x.bytes, x.len) ? 1 : 0;
    g_patched = ok == 5;
    RLOG(CH_CORE, g_patched ? L_INFO : L_ERROR, "shadow map size %dx%d (the engine's cap of 2048 lifted): %d/5 patches",
         n, n, ok);
}

// How the shadow map is fitted to the camera, for frame captures. Each frame the engine
//   0x00F16C20  points the light's shadow camera (a Camera: view-projection at +0x178) at the bounding sphere of the
//               whole map (0x00F16CC0, orthographic);
//   0x00F172F0  fits a trapezoidal shadow map (TSM) to the view camera ([[0x01464D58] + 8]): the frustum between its
//               shadow planes (+0x280 near = 100, +0x284 far = 2 x ground distance, set by GameCamera 0x006BC180) seen
//               from the light, the 80% line at the focus (+0x26C = eye-target distance, xi = -0.6) -> the light
//               camera's +0x4E0;
//   0x00F169E0  (ecx = the light camera) hands +0x178 and +0x4E0 to the shaders (ShadowMapProjection =
//               [[0x01455970 + 0x18] + 0x40], 4x4 floats at +0x34).
// The view camera: target (the look-at point on the ground) +0x288, eye +0x2A8, near/far +0x274/+0x278, fov +0x27C
// (45: tan(fov/2) times the aspect fields +0x334/+0x338 = width/1920, height/1920 gives the half-extents).
// The map's bounding box (the light is aimed at its sphere): [[0x01464D58] + 4] + 0x54 min xyz, + 0x60 max xyz.
//
// [graphics] shadow_fit 1-2 replaces the trapezoid in +0x4E0 when 0x00F169E0 starts, before it is handed on. The
// trapezoid covers the whole frustum from 100 units in front of the eye to twice the ground distance - the air
// between the camera and the ground and the volume under the ground with it: only ~30% of the map lands on the ground
// in view, at any zoom from 4400 on (autotest 2026-10-04), so the texels per world unit fall with the zoom distance.
//   1  the part of the map's bounding box in view (frustum and box intersected): every visible surface stays inside.
//      The box is the map's whole height (~8000 units): close to the ground that is more than the trapezoid covers,
//      so the trapezoid stays there (up to zoom ~3000) and ours takes over where it is denser;
//   2  only a layer around the look-at point's height (+- shadow_fit_margin % of the zoom distance): deep valleys
//      and high cliffs at the edges of the screen can fall outside and get no shadow.
// Both crop the light's view to the smallest rectangle around that volume, turned to fit it; the texel size moves in
// 1/16-octave steps and the rectangle in whole texels, so panning does not make shadow edges crawl. Lookups outside
// the map read "lit" (BORDER addressing on the shadow map's stage, gfx.cpp).
namespace {
constexpr uintptr_t FN_LIGHT_VARS = 0x00F169E0;
const unsigned char kLightVarsSig[5] = {0x83, 0xEC, 0x50, 0x53, 0x56};   // sub esp, 0x50; push ebx; push esi
void* g_light_tramp = nullptr;

struct FitSnap {
    bool light = false, cam = false, box = false, ours = false;
    const void* light_obj = nullptr;
    const void* cam_obj = nullptr;
    float light_vp[16], tsm[16], fit[16];
    float cam_vp[16], eye[3], target[3], focus, np, fp, fov, snp, sfp, ax, ay;
    float box_lo[3], box_hi[3];
    int level = 0;
    double gain = 0;   // texels per light-space area of ours over the engine's trapezoid
};
FitSnap g_fit;   // written and read on the render thread

// ---- small linear algebra, D3D convention: row vectors, p' = [x y z 1] * M, M row-major
void row_mul(const double v[4], const double* m, double o[4]) {
    for (int j = 0; j < 4; ++j) o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
}

bool inverse4(const double* a, double* inv) {
    double m[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            m[r][c] = a[r * 4 + c];
            m[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r)
            if (fabs(m[r][c]) > fabs(m[piv][c])) piv = r;
        if (fabs(m[piv][c]) < 1e-30) return false;
        if (piv != c)
            for (int k = 0; k < 8; ++k) std::swap(m[c][k], m[piv][k]);
        const double d = 1.0 / m[c][c];
        for (int k = 0; k < 8; ++k) m[c][k] *= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c || m[r][c] == 0) continue;
            const double f = m[r][c];
            for (int k = 0; k < 8; ++k) m[r][k] -= f * m[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inv[r * 4 + c] = m[r][c + 4];
    return true;
}

bool finite_all(const float* v, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

struct P3 {
    double x, y, z;
};
struct P2 {
    double x, y;
};

// The view frustum (camera view-projection C, D3D clip: -w <= x, y <= w, 0 <= z <= w) and a box: the corners of
// their intersection, as the corners of either inside the other plus the edges of either through the faces of the
// other.
struct Volume {
    double c[16];
    P3 lo, hi;
    P3 pts[200];
    int n = 0;

    void clip_of(const P3& p, double o[4]) const {
        const double v[4] = {p.x, p.y, p.z, 1.0};
        row_mul(v, c, o);
    }
    bool in_frustum(const P3& p) const {
        double k[4];
        clip_of(p, k);
        const double e = 1e-4 * fabs(k[3]) + 1e-9;
        return k[3] > 0 && k[0] >= -k[3] - e && k[0] <= k[3] + e && k[1] >= -k[3] - e && k[1] <= k[3] + e &&
               k[2] >= -e && k[2] <= k[3] + e;
    }
    bool in_box(const P3& p) const {
        const double e = 0.5;
        return p.x >= lo.x - e && p.x <= hi.x + e && p.y >= lo.y - e && p.y <= hi.y + e && p.z >= lo.z - e &&
               p.z <= hi.z + e;
    }
    void add(const P3& p) {
        if (n < 200) pts[n++] = p;
    }
    void build(const P3 fr[8]) {
        n = 0;
        P3 bx[8];
        for (int i = 0; i < 8; ++i) bx[i] = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
        for (int i = 0; i < 8; ++i) {
            if (in_box(fr[i])) add(fr[i]);
            if (in_frustum(bx[i])) add(bx[i]);
        }
        // the 12 edges of a cube-like 8-corner set: corners differing in one bit
        for (int i = 0; i < 8; ++i)
            for (int bit = 1; bit < 8; bit <<= 1) {
                if (i & bit) continue;
                const int j = i | bit;
                // frustum edge through the box's 6 planes
                const P3 a = fr[i], b = fr[j];
                const double av[3] = {a.x, a.y, a.z}, bv[3] = {b.x, b.y, b.z};
                const double lov[3] = {lo.x, lo.y, lo.z}, hiv[3] = {hi.x, hi.y, hi.z};
                for (int ax = 0; ax < 3; ++ax)
                    for (int side = 0; side < 2; ++side) {
                        const double pl = side ? hiv[ax] : lov[ax];
                        const double da = av[ax] - pl, db = bv[ax] - pl;
                        if (da * db >= 0) continue;
                        const double t = da / (da - db);
                        const P3 p = {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
                        if (in_box(p)) add(p);
                    }
                // box edge through the frustum's 6 planes (linear in clip space)
                const P3 q = bx[i], r = bx[j];
                double kq[4], kr[4];
                clip_of(q, kq);
                clip_of(r, kr);
                const double fq[6] = {kq[3] + kq[0], kq[3] - kq[0], kq[3] + kq[1], kq[3] - kq[1], kq[2], kq[3] - kq[2]};
                const double fr2[6] = {kr[3] + kr[0], kr[3] - kr[0], kr[3] + kr[1], kr[3] - kr[1], kr[2], kr[3] - kr[2]};
                for (int pl = 0; pl < 6; ++pl) {
                    if (fq[pl] * fr2[pl] >= 0) continue;
                    const double t = fq[pl] / (fq[pl] - fr2[pl]);
                    const P3 p = {q.x + t * (r.x - q.x), q.y + t * (r.y - q.y), q.z + t * (r.z - q.z)};
                    if (in_frustum(p)) add(p);
                }
            }
    }
};

double cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Convex hull (monotone chain), counter-clockwise; returns the number of hull points written to h (capacity 2n).
int hull(P2* p, int n, P2* h) {
    std::sort(p, p + n, [](const P2& a, const P2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    int k = 0;
    for (int i = 0; i < n; ++i) {
        while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {
        while (k >= t && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
        h[k++] = p[i];
    }
    return k > 1 ? k - 1 : k;
}

void extents(const P2* h, int n, double ang, double& u0, double& u1, double& v0, double& v1) {
    const double cs = cos(ang), sn = sin(ang);
    u0 = v0 = 1e300;
    u1 = v1 = -1e300;
    for (int i = 0; i < n; ++i) {
        const double u = h[i].x * cs + h[i].y * sn, v = -h[i].x * sn + h[i].y * cs;
        u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
}

// The fit's state between frames: rotation and texel sizes change only when they must (stable shadow edges).
struct Held {
    const void* obj = nullptr;
    int level = -1;
    double ang = 0, tu = 0, tv = 0;
    double next_log = 0;
    double logged_tu = 0, logged_tv = 0;
    int logged_level = -1;
    bool logged_ours = false;
    bool l1_ours = false;   // level 1: ours is denser than the trapezoid at this zoom
} g_held;

constexpr double kPi = 3.14159265358979323846;
constexpr double kAngStep = kPi / 720;   // 0.25 degree
constexpr double kTexStep = 1.0 / 16;                       // octaves

double quant_up(double t) { return exp2(ceil(log2(t) / kTexStep) * kTexStep); }

// Keeps the held texel size while the content still fits and wastes under 3/16 octave, else a new quantized one.
double hold_texel(double held, double need) {
    if (held > 0 && need <= held && need >= held * exp2(-3 * kTexStep)) return held;
    return quant_up(need);
}

// Light-space area the engine's trapezoid covers: the shadow map's square mapped back through its homography.
double tsm_area(const float* h) {
    // [lx ly 1] -> (X, Y, W): rows 0, 1, 3 and columns 0, 1, 3 of the 4x4 (z passes through)
    const double g[9] = {h[0], h[1], h[3], h[4], h[5], h[7], h[12], h[13], h[15]};
    const double det = g[0] * (g[4] * g[8] - g[5] * g[7]) - g[1] * (g[3] * g[8] - g[5] * g[6]) +
                       g[2] * (g[3] * g[7] - g[4] * g[6]);
    if (fabs(det) < 1e-30) return 0;
    const double inv[9] = {(g[4] * g[8] - g[5] * g[7]) / det,  (g[2] * g[7] - g[1] * g[8]) / det,
                           (g[1] * g[5] - g[2] * g[4]) / det,  (g[5] * g[6] - g[3] * g[8]) / det,
                           (g[0] * g[8] - g[2] * g[6]) / det,  (g[2] * g[3] - g[0] * g[5]) / det,
                           (g[3] * g[7] - g[4] * g[6]) / det,  (g[1] * g[6] - g[0] * g[7]) / det,
                           (g[0] * g[4] - g[1] * g[3]) / det};
    P2 q[4];
    const double sq[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; ++i) {
        const double x = sq[i][0], y = sq[i][1];
        const double X = x * inv[0] + y * inv[3] + inv[6], Y = x * inv[1] + y * inv[4] + inv[7],
                     W = x * inv[2] + y * inv[5] + inv[8];
        if (fabs(W) < 1e-30) return 0;
        q[i] = {X / W, Y / W};
    }
    double a = 0;
    for (int i = 0; i < 4; ++i) a += q[i].x * q[(i + 1) % 4].y - q[(i + 1) % 4].x * q[i].y;
    return fabs(a) / 2;
}

// Our crop for the light camera at obj; false = keep the engine's trapezoid.
bool compute_fit(int level, float out[16], double* gain) {
    if (!g_fit.cam || !g_fit.box) return false;
    if (!finite_all(g_fit.light_vp, 16) || !finite_all(g_fit.cam_vp, 16) || !finite_all(g_fit.tsm, 16)) return false;
    if (g_fit.light_vp[3] != 0 || g_fit.light_vp[7] != 0 || g_fit.light_vp[11] != 0) return false;   // orthographic
    Volume vol;
    double ic[16];
    for (int i = 0; i < 16; ++i) vol.c[i] = g_fit.cam_vp[i];
    if (!inverse4(vol.c, ic)) return false;
    P3 fr[8];
    for (int i = 0; i < 8; ++i) {
        const double v[4] = {i & 1 ? 1.0 : -1.0, i & 2 ? 1.0 : -1.0, i & 4 ? 1.0 : 0.0, 1.0};
        double o[4];
        row_mul(v, ic, o);
        if (!(fabs(o[3]) > 1e-12)) return false;
        fr[i] = {o[0] / o[3], o[1] / o[3], o[2] / o[3]};
    }
    vol.lo = {g_fit.box_lo[0], g_fit.box_lo[1], g_fit.box_lo[2] - 100.0};
    vol.hi = {g_fit.box_hi[0], g_fit.box_hi[1], g_fit.box_hi[2] + 1500.0};   // buildings and trees above the relief
    if (level >= 2) {
        const double dx = g_fit.eye[0] - g_fit.target[0], dy = g_fit.eye[1] - g_fit.target[1],
                     dz = g_fit.eye[2] - g_fit.target[2];
        const double d = sqrt(dx * dx + dy * dy + dz * dz), m = d * g_cfg.shadow_fit_margin / 100.0;
        vol.lo.z = std::max(vol.lo.z, g_fit.target[2] - m - 300.0);
        vol.hi.z = std::min(vol.hi.z, g_fit.target[2] + m + 1500.0);
    }
    if (!(vol.lo.x < vol.hi.x && vol.lo.y < vol.hi.y && vol.lo.z < vol.hi.z)) return false;
    vol.build(fr);
    if (vol.n < 3) return false;
    // into light space (orthographic: w = 1)
    P2 lp[200], hp[400];
    double l[16];
    for (int i = 0; i < 16; ++i) l[i] = g_fit.light_vp[i];
    for (int i = 0; i < vol.n; ++i) {
        const double v[4] = {vol.pts[i].x, vol.pts[i].y, vol.pts[i].z, 1.0};
        double o[4];
        row_mul(v, l, o);
        lp[i] = {o[0], o[1]};
    }
    const int hn = hull(lp, vol.n, hp);
    if (hn < 3) return false;
    // smallest rectangle: one side along a hull edge; keep the held rotation unless another is clearly smaller
    double best_ang = 0, best_area = 1e300, u0, u1, v0, v1;
    for (int i = 0; i < hn; ++i) {
        const P2& a = hp[i];
        const P2& b = hp[(i + 1) % hn];
        const double ang = atan2(b.y - a.y, b.x - a.x);
        extents(hp, hn, ang, u0, u1, v0, v1);
        const double area = (u1 - u0) * (v1 - v0);
        if (area < best_area) best_area = area, best_ang = ang;
    }
    // a rectangle's sides repeat every 90 degrees: one canonical angle in [0, 90), so the grid never flips over
    best_ang = fmod(best_ang, kPi / 2);
    if (best_ang < 0) best_ang += kPi / 2;
    double ang = fmod(round(best_ang / kAngStep) * kAngStep, kPi / 2);
    if (g_held.level == level && g_held.obj == g_fit.light_obj) {
        extents(hp, hn, g_held.ang, u0, u1, v0, v1);
        if ((u1 - u0) * (v1 - v0) <= best_area * 1.03) ang = g_held.ang;
    } else {
        g_held.tu = g_held.tv = 0;
    }
    extents(hp, hn, ang, u0, u1, v0, v1);
    int n = shadow_map_size();
    if (n < 64) n = 2048;
    const double usable = n - 2;   // the engine's viewport is (1, 1, n - 2, n - 2)
    const double room = usable - 6;   // snapping and filter taps never reach the unwritten 1-texel frame
    if (!(u1 > u0 && v1 > v0)) return false;
    const double tu = hold_texel(g_held.ang == ang && g_held.level == level ? g_held.tu : 0, (u1 - u0) / room);
    const double tv = hold_texel(g_held.ang == ang && g_held.level == level ? g_held.tv : 0, (v1 - v0) / room);
    // the rectangle starts on the texel grid and is centred on the content within it
    const double su = tu * usable, sv = tv * usable;
    const double us = floor(((u0 + u1) / 2 - su / 2) / tu) * tu, vs = floor(((v0 + v1) / 2 - sv / 2) / tv) * tv;
    if (u0 < us || u1 > us + su || v0 < vs || v1 > vs + sv) return false;
    g_held.obj = g_fit.light_obj;
    g_held.level = level;
    g_held.ang = ang;
    g_held.tu = tu;
    g_held.tv = tv;
    // [lx ly lz 1] -> clip: u = lx cos + ly sin, v = -lx sin + ly cos; x = (u - us) * 2 / su - 1, same for y
    const double cs = cos(ang), sn = sin(ang), ku = 2 / su, kv = 2 / sv;
    const double m[16] = {cs * ku,       -sn * kv,      0, 0,   //
                          sn * ku,       cs * kv,       0, 0,   //
                          0,             0,             1, 0,   //
                          -us * ku - 1,  -vs * kv - 1,  0, 1};
    for (int i = 0; i < 16; ++i) out[i] = float(m[i]);
    const double te = tsm_area(g_fit.tsm);
    *gain = te > 0 ? te / (su * sv) : 0;
    return true;
}

void log_fit(int level, bool ok) {
    const double now = now_ms();
    const bool same = level == g_held.logged_level && ok == g_held.logged_ours;
    if (same && (!ok || (g_held.tu == g_held.logged_tu && g_held.tv == g_held.logged_tv))) return;
    if (same && now < g_held.next_log) return;
    g_held.next_log = now + 1000;
    g_held.logged_level = level;
    g_held.logged_ours = ok;
    g_held.logged_tu = g_held.tu;
    g_held.logged_tv = g_held.tv;
    if (!level) {
        RLOG(CH_CORE, L_INFO, "shadow fit: the engine's trapezoid");
        return;
    }
    if (!ok) {
        if (level == 1 && g_fit.gain > 0)
            RLOG(CH_CORE, L_INFO, "shadow fit 1: the engine's trapezoid is denser at this zoom (ours %.2fx), kept",
                 g_fit.gain);
        else
            RLOG(CH_CORE, L_WARN, "shadow fit %d: no volume in view, the engine's trapezoid kept for now", level);
        return;
    }
    // world units per light-space unit: 1 / the length of the light x row's world gradient
    const double gx = sqrt(double(g_fit.light_vp[0]) * g_fit.light_vp[0] + double(g_fit.light_vp[4]) * g_fit.light_vp[4] +
                           double(g_fit.light_vp[8]) * g_fit.light_vp[8]);
    const double w = gx > 0 ? 1 / gx : 0;
    const double dx = g_fit.eye[0] - g_fit.target[0], dy = g_fit.eye[1] - g_fit.target[1],
                 dz = g_fit.eye[2] - g_fit.target[2];
    RLOG(CH_CORE, L_INFO,
         "shadow fit %d at zoom %.0f: texel %.2f x %.2f world units, %.2fx the texels per area of the engine's "
         "trapezoid (rotation %.2f deg)",
         level, sqrt(dx * dx + dy * dy + dz * dz), g_held.tu * w, g_held.tv * w, g_fit.gain,
         g_held.ang * 180 / kPi);
}

}  // namespace

// The view camera's projection, read live (ssao.cpp): near / far plane (+0x274 / +0x278), tan of half the field of view
// (+0x27C, degrees) times the aspect fields (+0x334 / +0x338) across and up.
bool view_projection(float* n, float* f, float* tx, float* ty) {
    float np = 0, fp = 0, fov = 0, ax = 0, ay = 0;
    __try {
        const uintptr_t app = *reinterpret_cast<const uintptr_t*>(0x01464D58);
        const uintptr_t cp = app ? *reinterpret_cast<const uintptr_t*>(app + 8) : 0;
        const uint8_t* cam = reinterpret_cast<const uint8_t*>(cp ? cp : 0x01475420);
        np = *reinterpret_cast<const float*>(cam + 0x274);
        fp = *reinterpret_cast<const float*>(cam + 0x278);
        fov = *reinterpret_cast<const float*>(cam + 0x27C);
        ax = *reinterpret_cast<const float*>(cam + 0x334);
        ay = *reinterpret_cast<const float*>(cam + 0x338);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!(np > 0 && fp > np && fov > 1 && fov < 170 && ax > 0 && ay > 0)) return false;
    const double t = tan(double(fov) * kPi / 360.0);
    *n = np;
    *f = fp;
    *tx = float(t * ax);
    *ty = float(t * ay);
    return true;
}

bool view_eye_target(float eye[3], float target[3]) {
    __try {
        const uintptr_t app = *reinterpret_cast<const uintptr_t*>(0x01464D58);
        const uintptr_t cp = app ? *reinterpret_cast<const uintptr_t*>(app + 8) : 0;
        const uint8_t* cam = reinterpret_cast<const uint8_t*>(cp ? cp : 0x01475420);
        memcpy(eye, cam + 0x2A8, 12);
        memcpy(target, cam + 0x288, 12);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return finite_all(eye, 3) && finite_all(target, 3);
}

bool view_focus(float* focus) {
    __try {
        const uintptr_t app = *reinterpret_cast<const uintptr_t*>(0x01464D58);
        const uintptr_t cp = app ? *reinterpret_cast<const uintptr_t*>(app + 8) : 0;
        const uint8_t* cam = reinterpret_cast<const uint8_t*>(cp ? cp : 0x01475420);
        *focus = *reinterpret_cast<const float*>(cam + 0x26C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return *focus > 0;
}

namespace {

void __cdecl on_light_vars(uint8_t* obj) {
    __try {
        memcpy(g_fit.light_vp, obj + 0x178, 64);
        memcpy(g_fit.tsm, obj + 0x4E0, 64);
        g_fit.light_obj = obj;
        g_fit.light = true;
        uintptr_t app = *reinterpret_cast<const uintptr_t*>(0x01464D58);
        uintptr_t cp = app ? *reinterpret_cast<const uintptr_t*>(app + 8) : 0;
        const uint8_t* cam = reinterpret_cast<const uint8_t*>(cp ? cp : 0x01475420);
        memcpy(g_fit.cam_vp, cam + 0x178, 64);
        memcpy(g_fit.eye, cam + 0x2A8, 12);
        memcpy(g_fit.target, cam + 0x288, 12);
        g_fit.focus = *reinterpret_cast<const float*>(cam + 0x26C);
        g_fit.np = *reinterpret_cast<const float*>(cam + 0x274);
        g_fit.fp = *reinterpret_cast<const float*>(cam + 0x278);
        g_fit.fov = *reinterpret_cast<const float*>(cam + 0x27C);
        g_fit.snp = *reinterpret_cast<const float*>(cam + 0x280);
        g_fit.sfp = *reinterpret_cast<const float*>(cam + 0x284);
        g_fit.ax = *reinterpret_cast<const float*>(cam + 0x334);
        g_fit.ay = *reinterpret_cast<const float*>(cam + 0x338);
        g_fit.cam_obj = cam;
        g_fit.cam = true;
        uintptr_t world = app ? *reinterpret_cast<const uintptr_t*>(app + 4) : 0;
        g_fit.box = false;
        if (world) {
            memcpy(g_fit.box_lo, reinterpret_cast<const void*>(world + 0x54), 12);
            memcpy(g_fit.box_hi, reinterpret_cast<const void*>(world + 0x60), 12);
            g_fit.box = finite_all(g_fit.box_lo, 3) && finite_all(g_fit.box_hi, 3);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_fit.cam = false;
    }
    const int level = g_cfg.shadow_fit;
    g_fit.level = level;
    g_fit.ours = false;
    if (level > 0 && g_fit.cam) {
        float m[16];
        double gain = 0;
        if (compute_fit(level, m, &gain)) {
            g_fit.gain = gain;
            // level 1 covers the map's whole height: close to the ground (zoom ~3000 and in) that is more than the
            // trapezoid, which then stays (switch over at 1.1x / back at 0.95x, no flicker in between)
            if (level == 1) g_held.l1_ours = g_held.l1_ours ? gain >= 0.95 : gain >= 1.1;
            if (level != 1 || g_held.l1_ours) {
                __try {
                    memcpy(obj + 0x4E0, m, 64);
                    memcpy(g_fit.fit, m, 64);
                    g_fit.ours = true;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    g_fit.ours = false;
                }
            }
        }
    }
    log_fit(level, g_fit.ours);
}

__declspec(naked) void h_light_vars() {
    __asm {
        pushad
        push ecx
        call on_light_vars
        add esp, 4
        popad
        jmp dword ptr [g_light_tramp]
    }
}

bool read_shadow_matrix(float m[16]) {
    __try {
        uintptr_t table = *reinterpret_cast<const uintptr_t*>(0x01455970 + 0x18);
        uintptr_t var = table ? *reinterpret_cast<const uintptr_t*>(table + 0x40) : 0;
        if (!var) return false;
        memcpy(m, reinterpret_cast<const void*>(var + 0x34), 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void add_matrix(std::string& s, const char* name, const float* m) {
    char b[320];
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                " | %s %.7g %.7g %.7g %.7g / %.7g %.7g %.7g %.7g / %.7g %.7g %.7g %.7g / %.7g %.7g %.7g %.7g", name, m[0],
                m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    s += b;
}
}  // namespace

void shadow_fit_install() {
    if (memcmp(reinterpret_cast<const void*>(FN_LIGHT_VARS), kLightVarsSig, sizeof(kLightVarsSig)) != 0) {
        RLOG(CH_CORE, L_WARN, "shadow fit hook off: code at 0x%08x differs", unsigned(FN_LIGHT_VARS));
        return;
    }
    g_light_tramp = detour(FN_LIGHT_VARS, kLightVarsSig, sizeof(kLightVarsSig), (void*)h_light_vars);
    RLOG(CH_CORE, g_light_tramp ? L_INFO : L_ERROR, "shadow fit hook %s", g_light_tramp ? "installed" : "FAILED");
}

// Lines for a frame capture: the shader variable, then the last fit seen (light camera, TSM, view camera).
std::string shadow_matrix_describe() {
    std::string s = "shadow: ShadowMapProjection";
    float m[16];
    if (read_shadow_matrix(m)) add_matrix(s, "", m);
    else s += " -";
    s += "\nshadow fit:";
    if (!g_fit.light) return s + " -";
    char b[400];
    _snprintf_s(b, sizeof(b), _TRUNCATE, " light camera %p", g_fit.light_obj);
    s += b;
    add_matrix(s, "light VP", g_fit.light_vp);
    add_matrix(s, "TSM", g_fit.tsm);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "\nshadow fit level %d: %s", g_fit.level,
                g_fit.ours ? "ours" : "the engine's trapezoid");
    s += b;
    if (g_fit.ours) {
        add_matrix(s, "fit", g_fit.fit);
        _snprintf_s(b, sizeof(b), _TRUNCATE, " | %.2fx the texels per area of the trapezoid", g_fit.gain);
        s += b;
    }
    if (g_fit.box) {
        _snprintf_s(b, sizeof(b), _TRUNCATE, " | map box %.0f %.0f %.0f - %.0f %.0f %.0f", g_fit.box_lo[0],
                    g_fit.box_lo[1], g_fit.box_lo[2], g_fit.box_hi[0], g_fit.box_hi[1], g_fit.box_hi[2]);
        s += b;
    }
    if (!g_fit.cam) return s;
    _snprintf_s(b, sizeof(b), _TRUNCATE,
                "\nshadow view camera %p: eye %.2f %.2f %.2f | target %.2f %.2f %.2f | focus %.2f | near %.2f far %.2f "
                "fov %.3f aspect %.4f %.4f | shadow near %.2f far %.2f",
                g_fit.cam_obj, g_fit.eye[0], g_fit.eye[1], g_fit.eye[2], g_fit.target[0], g_fit.target[1],
                g_fit.target[2], g_fit.focus, g_fit.np, g_fit.fp, g_fit.fov, g_fit.ax, g_fit.ay, g_fit.snp, g_fit.sfp);
    s += b;
    add_matrix(s, "VP", g_fit.cam_vp);
    return s;
}

}  // namespace rol
