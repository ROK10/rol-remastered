// shadertest.exe — the shader rewrites (shadergen.cpp) checked without the game. Every dumped pixel shader with a
// shadow sampler ([debug] dump_shaders=1 writes them to <session>\shaders) is analysed and rewritten for each level's
// box width (kSoftWidth), with and without the plane bias; every one with light_diffuse gets the gamma-correct light,
// over each soft-shadow level. The copies are assembled by d3dx9_27.dll (the game's D3DX, so its validator) and created
// on a D3D9 device (the runtime's own validation against the card's caps).
//   shadertest <shaders dir> [map size, 8192] [dir to write the failed copies' assembly to]
// First the self-tests: the soft-shadow rewrite of a minimal shader emulated on a synthetic map against the filter
// computed directly, the gamma's remap emulated against its formula, the AO renderer on a synthetic scene, the
// sharpening and vibrance pass on a synthetic image against the same formulas on the CPU, the interface scale's auto
// choice for the usual screen heights.
// Exit code 0 when the self-tests pass and every copy assembles and is created.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ao.h"
#include "post.h"
#include "shadergen.h"
#include "uiauto.h"

typedef HRESULT(WINAPI* Assemble_t)(LPCSTR, UINT, const void*, void*, DWORD, IUnknown**, IUnknown**);
typedef HRESULT(WINAPI* Disasm_t)(const DWORD*, BOOL, LPCSTR, IUnknown**);

static std::string buffer_text(IUnknown* buf) {
    void** vt = *reinterpret_cast<void***>(buf);
    auto get_ptr = reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>(vt[3]);
    auto get_size = reinterpret_cast<DWORD(STDMETHODCALLTYPE*)(IUnknown*)>(vt[4]);
    const char* p = static_cast<const char*>(get_ptr(buf));
    return std::string(p ? p : "", p ? strnlen(p, get_size(buf)) : 0);
}

static const DWORD* buffer_code(IUnknown* buf) {
    return static_cast<const DWORD*>(
        reinterpret_cast<void*(STDMETHODCALLTYPE*)(IUnknown*)>((*reinterpret_cast<void***>(buf))[3])(buf));
}

static void release(IUnknown* u) {
    if (u) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(IUnknown*)>((*reinterpret_cast<void***>(u))[2])(u);
}

static std::string read_all(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return s;
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

// ---- The self-test: the rewrite of a minimal one-tap shader run by an emulator of the instructions it uses, a 2x2
// pixel quad at a time (dsx / dsy as the hardware takes them), on a synthetic shadow map, against the filter computed
// directly. Numbers only: the lit share at a shadow's edge, its smoothness, a slope that must stay lit.
namespace emu {

struct V4 {
    float v[4] = {0, 0, 0, 0};
};

struct Arg {
    bool neg = false;
    std::string reg;
    int sw[4] = {0, 1, 2, 3};
};

static std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static int comp_index(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : 3; }

static Arg parse_arg(std::string a) {
    Arg r;
    if (!a.empty() && a[0] == '-') {
        r.neg = true;
        a.erase(0, 1);
    }
    const size_t dot = a.find('.');
    r.reg = a.substr(0, dot);
    if (dot != std::string::npos) {
        const std::string s = a.substr(dot + 1);
        for (int k = 0; k < 4; ++k) r.sw[k] = comp_index(k < int(s.size()) ? s[k] : s.back());
    }
    return r;
}

struct Machine {
    std::map<std::string, V4> reg[4];   // a register file per pixel of the quad: 0 1 / 2 3
    const std::vector<float>* depth = nullptr;   // n x n texels, rows along v
    int n = 0;
    std::map<std::string, V4> flat;              // samplers that read one texel everywhere (a normal map)
    std::string error;

    V4 read(int lane, const Arg& a) {
        auto it = reg[lane].find(a.reg);
        V4 o;
        if (it == reg[lane].end()) {
            if (error.empty()) error = "reads unset " + a.reg;
            return o;
        }
        for (int k = 0; k < 4; ++k) o.v[k] = a.neg ? -it->second.v[a.sw[k]] : it->second.v[a.sw[k]];
        return o;
    }

    bool run(const std::vector<std::string>& code) {
        for (const std::string& raw : code) {
            const std::string l = trim(raw);
            if (l.empty()) continue;
            const size_t sp = l.find(' ');
            const std::string opc = l.substr(0, sp);
            const std::string op = opc.substr(0, opc.find('_'));
            if (op == "ps" || op == "dcl") continue;
            std::vector<std::string> args;
            for (size_t p = sp + 1; p <= l.size();) {
                const size_t c = l.find(',', p);
                args.push_back(trim(l.substr(p, c == std::string::npos ? std::string::npos : c - p)));
                if (c == std::string::npos) break;
                p = c + 1;
            }
            const size_t dot = args[0].find('.');
            const std::string dreg = args[0].substr(0, dot);
            int mask = 15;
            if (dot != std::string::npos) {
                mask = 0;
                for (char c : args[0].substr(dot + 1)) mask |= 1 << comp_index(c);
            }
            if (op == "def") {
                V4 c;
                for (int k = 0; k < 4; ++k) c.v[k] = float(atof(args[1 + k].c_str()));
                for (int lane = 0; lane < 4; ++lane) reg[lane][dreg] = c;
                continue;
            }
            std::vector<Arg> a;
            for (size_t i = 1; i < args.size(); ++i) a.push_back(parse_arg(args[i]));
            V4 s[4][3];
            for (int lane = 0; lane < 4; ++lane)
                for (size_t i = 0; i < a.size() && i < 3; ++i)
                    if (!(op == "texld" && i == 1)) s[lane][i] = read(lane, a[i]);
            for (int lane = 0; lane < 4; ++lane) {
                V4 o;
                const V4 &x = s[lane][0], &y = s[lane][1], &z = s[lane][2];
                for (int k = 0; k < 4; ++k) {
                    float& r = o.v[k];
                    if (op == "mov") r = x.v[k];
                    else if (op == "add") r = x.v[k] + y.v[k];
                    else if (op == "mul") r = x.v[k] * y.v[k];
                    else if (op == "mad") r = x.v[k] * y.v[k] + z.v[k];
                    else if (op == "frc") r = x.v[k] - floorf(x.v[k]);
                    else if (op == "cmp") r = x.v[k] >= 0 ? y.v[k] : z.v[k];
                    else if (op == "rcp") r = 1.0f / x.v[0];
                    else if (op == "min") r = x.v[k] < y.v[k] ? x.v[k] : y.v[k];
                    else if (op == "max") r = x.v[k] >= y.v[k] ? x.v[k] : y.v[k];
                    else if (op == "dsx") r = s[lane | 1][0].v[k] - s[lane & 2][0].v[k];
                    else if (op == "dsy") r = s[lane | 2][0].v[k] - s[lane & 1][0].v[k];
                    else if (op == "dp3") r = x.v[0] * y.v[0] + x.v[1] * y.v[1] + x.v[2] * y.v[2];
                    else if (op == "nrm") r = x.v[k] / sqrtf(x.v[0] * x.v[0] + x.v[1] * x.v[1] + x.v[2] * x.v[2]);
                    else if (op == "pow") r = powf(fabsf(x.v[0]), y.v[0]);
                    else if (op == "rsq") r = 1.0f / sqrtf(fabsf(x.v[0]));
                    else if (op == "texld" && flat.count(args[2]))
                        r = flat[args[2]].v[k];
                    else if (op == "texld" && args[2] != "s0")
                        r = 0;   // other textures (the fog of war): clear
                    else if (op == "texld") {
                        const int ix = int(floorf(x.v[0] * n)), iy = int(floorf(x.v[1] * n));
                        const bool in = ix >= 0 && iy >= 0 && ix < n && iy < n;
                        r = k ? 1.0f : (in ? (*depth)[size_t(iy) * n + ix] : 1e30f);   // R32F; BORDER reads lit
                    } else {
                        error = "no emulation for " + op;
                        return false;
                    }
                }
                if (opc.find("_sat") != std::string::npos)
                    for (float& r : o.v) r = r < 0 ? 0 : (r > 1 ? 1 : r);
                V4& d = reg[lane][dreg];
                for (int k = 0; k < 4; ++k)
                    if (mask & (1 << k)) d.v[k] = o.v[k];
            }
            if (!error.empty()) return false;
        }
        return true;
    }
};

// The filter computed directly: the box of W texels centred 'shift' texels off the fragment at (u, v), edge weights
// 1 - f / f, each texel compared against the fragment's depth or (plane) the receiving plane's under it, laid through
// the fragment, its slope per texel clamped as the shader does.
static double reference(const std::vector<float>& depth, int n, double u, double v, double z, double eps, int w,
                        bool plane, double gu, double gv, double shift) {
    const double ax = (u * n + shift) - w / 2.0, ay = (v * n + shift) - w / 2.0;
    const double fx = ax - floor(ax), fy = ay - floor(ay);
    const int nx = int(floor(ax)), ny = int(floor(ay));
    const double m = 4 * eps;
    const double cu = plane ? (gu < -m ? -m : gu > m ? m : gu) : 0, cv = plane ? (gv < -m ? -m : gv > m ? m : gv) : 0;
    const double tx = fx + (w - 1) / 2.0 - shift, ty = fy + (w - 1) / 2.0 - shift;   // the fragment from texel n
    double sum = 0;
    for (int j = 0; j <= w; ++j)
        for (int i = 0; i <= w; ++i) {
            const double wx = i == 0 ? 1 - fx : i == w ? fx : 1, wy = j == 0 ? 1 - fy : j == w ? fy : 1;
            const int ix = nx + i, iy = ny + j;
            const bool in = ix >= 0 && iy >= 0 && ix < n && iy < n;
            const double d = in ? depth[size_t(iy) * n + ix] : 1e30;
            const double zref = z + cu * (i - tx) + cv * (j - ty);
            sum += (d + eps - zref >= 0 ? 1 : 0) * wx * wy;
        }
    return sum / (w * w);
}

}  // namespace emu

// The minimal shader with the one-tap test, as D3DXDisassembleShader writes it (the constant table's sampler row).
static const char kMinimal[] =
    "//   shadow_sampler           s0       1\n"
    "    ps_2_0\n"
    "    def c1, 1, 0, 0, 0\n"
    "    dcl t0.xy\n"
    "    dcl t1.x\n"
    "    dcl_2d s0\n"
    "    mov r1.xy, t0\n"
    "    texld r0, r1, s0\n"
    "    add r2.w, r0.x, c0.x\n"
    "    mov r3.w, t1.x\n"
    "    add r2.w, r2.w, -r3.w\n"
    "    cmp r4.w, r2.w, c1.x, c1.y\n"
    "    mov r5, r4.w\n"
    "    mov oC0, r5\n";

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    for (size_t p = 0; p < s.size();) {
        const size_t e = s.find('\n', p);
        lines.push_back(s.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return lines;
}

// A shader the self-test runs: the inputs of one pixel at map coordinates (u, v) with depth z in the light, and the lit
// share read back from its registers.
struct Subject {
    std::string name;
    rol::sg::ShadowPlan plan;
    double eps = 0;     // what the rewrite compares with
    double shift = 0;   // the box's centre - the fragment, texels
    std::function<void(std::map<std::string, emu::V4>&, double u, double v, double z)> inputs;
    std::function<double(std::map<std::string, emu::V4>&)> lit;
};

static const int kN = 64;   // the synthetic map's size
// the receiving surface: z = 0.5 + gu x + gv y over texels x = u n, y = v n; the quad's pixels 0.37 / 0.58 texel apart
// and turned a little, as the fitted map lies on the screen
static const double kDux = 0.37 / kN, kDvx = 0.11 / kN, kDuy = -0.06 / kN, kDvy = 0.58 / kN;

// Runs 'lines' for the quad whose first pixel is at (u, v) on a surface of slope (gu, gv) per texel; that pixel's lit
// share.
static double emulate(const Subject& s, const std::vector<std::string>& lines, const std::vector<float>& depth, double u,
                      double v, double gu, double gv, std::string& error) {
    emu::Machine m;
    m.depth = &depth;
    m.n = kN;
    for (int lane = 0; lane < 4; ++lane) {
        const double pu = u + ((lane & 1) ? kDux : 0) + ((lane & 2) ? kDuy : 0);
        const double pv = v + ((lane & 1) ? kDvx : 0) + ((lane & 2) ? kDvy : 0);
        s.inputs(m.reg[lane], pu, pv, 0.5 + gu * pu * kN + gv * pv * kN);
    }
    if (!m.run(lines) && error.empty()) error = m.error;
    return s.lit(m.reg[0]);
}

// The rewrite at each level, with and without the plane bias, on three synthetic maps: a shadow's edge on a flat
// surface (its lit share 0.5 where the box's centre is on the edge, rising monotonic and smooth across it), a lit slope
// (must stay lit with the plane bias), the slope with an edge across it; every value against reference().
static int run_subject(const Subject& s, Assemble_t assemble, IDirect3DDevice9* dev) {
    int bad = 0;
    for (int level = 1; level < 4; ++level)
        for (int plane = 0; plane < 2; ++plane) {
            const int w = rol::sg::kSoftWidth[level];
            const std::string src = rol::sg::soft_shadow_source(s.plan, w, kN, plane != 0);
            IUnknown *code = nullptr, *errs = nullptr;
            bool made = !src.empty() && SUCCEEDED(assemble(src.c_str(), UINT(src.size()), nullptr, nullptr, 0, &code,
                                                           &errs)) && code;
            if (!made && errs) printf("    %.300s\n", buffer_text(errs).c_str());
            if (made && dev) {
                IDirect3DPixelShader9* ps = nullptr;
                made = SUCCEEDED(dev->CreatePixelShader(buffer_code(code), &ps));
                if (ps) ps->Release();
            }
            release(code);
            release(errs);
            const std::vector<std::string> lines = split_lines(src);
            std::vector<float> depth(size_t(kN) * kN);
            double max_diff = 0, mid = -1, max_step = 0, slope_min = 1, slope_sum = 0;
            bool monotonic = true;
            std::string error;
            auto shade = [&](double cu, double cv, double gu, double gv) {   // the box's centre at (cu, cv) texels
                const double u = (cu - s.shift) / kN, v = (cv - s.shift) / kN;
                const double lit = emulate(s, lines, depth, u, v, gu, gv, error);
                const double direct = emu::reference(depth, kN, u, v, 0.5 + gu * u * kN + gv * v * kN, s.eps, w,
                                                     plane != 0, gu, gv, s.shift);
                max_diff = std::max(max_diff, fabs(lit - direct));
                return lit;
            };
            // 1. a shadow's edge on a flat surface: texels x < 32 hold an occluder; across it, a 16th of a texel a step
            for (int y = 0; y < kN; ++y)
                for (int x = 0; x < kN; ++x) depth[size_t(y) * kN + x] = x < 32 ? 0.0f : 0.5f;
            double prev = -1;
            for (int t = -16 * (w + 1); t <= 16 * (w + 1); ++t) {
                const double lit = shade(32 + t / 16.0, 32.3, 0, 0);
                if (t == 0) mid = lit;
                if (prev >= 0) {
                    if (lit < prev - 1e-6) monotonic = false;
                    max_step = std::max(max_step, lit - prev);
                }
                prev = lit;
            }
            // 2. a slope with nothing over it (2 eps per texel along u, -1 along v), drawn into the map: it must stay lit
            const double e = 0.0003;
            for (int y = 0; y < kN; ++y)
                for (int x = 0; x < kN; ++x) depth[size_t(y) * kN + x] = float(0.5 + 2 * e * (x + 0.5) - e * (y + 0.5));
            unsigned seed = 12345;
            auto rnd = [&]() {
                seed = seed * 1664525u + 1013904223u;
                return (seed >> 8) / double(1 << 24);
            };
            for (int i = 0; i < 400; ++i) {
                const double lit = shade(16 + 32 * rnd(), 16 + 32 * rnd(), 2 * e, -e);
                slope_min = std::min(slope_min, lit);
                slope_sum += lit;
            }
            // 3. the same slope with an occluder's edge across it (x < 32 nearer the light)
            for (int y = 0; y < kN; ++y)
                for (int x = 0; x < 32; ++x) depth[size_t(y) * kN + x] -= 0.2f;
            for (int i = 0; i < 400; ++i) shade(32 + (w + 2) * (2 * rnd() - 1), 16 + 32 * rnd(), 2 * e, -e);
            const bool ok = made && error.empty() && max_diff < 1e-4 && fabs(mid - 0.5) < 1e-4 && monotonic &&
                            max_step <= 1.0 / (16 * w) + 1e-4 && (!plane || slope_min > 0.9999);
            if (!ok) ++bad;
            printf("self-test %s W%d%s: %s - edge %.4f at its line, %s, steepest step %.4f (1/16W %.4f); slope lit min "
                   "%.3f mean %.3f; emulated vs direct %.2g%s%s\n",
                   s.name.c_str(), w, plane ? " plane bias" : "           ", ok ? "ok  " : "FAIL", mid,
                   monotonic ? "monotonic" : "NOT monotonic", max_step, 1.0 / (16 * w), slope_min, slope_sum / 400, max_diff,
                   made ? "" : ", NOT ASSEMBLED", error.empty() ? "" : (", " + error).c_str());
        }
    return bad;
}

// The terrain pass's inputs as the engine gives them: shadowmap_size N (c4), shrink_shadowmap_size (N/4, 4/N, 1/N)
// (c5), the preshader's 2/N (c0) and shadow_max_depth_influence - 1 (c1), shadow_percent 0.5 (c2), shadow_color black
// (c3); t0 the light's clip position, t1.z its depth (z * 2 - 1), t2.x the face's N.L (lit side), v0 white. The shadow's
// share comes out in oC0.w, scaled by shadow_percent.
static void terrain_inputs(std::map<std::string, emu::V4>& r, double u, double v, double z) {
    auto set = [&](const char* name, double a, double b = 0, double c = 0, double d = 0) {
        emu::V4 x;
        x.v[0] = float(a);
        x.v[1] = float(b);
        x.v[2] = float(c);
        x.v[3] = float(d);
        r[name] = x;
    };
    set("t0", 2 * u - 1, 1 - 2 * v);
    set("t1", 0, 0, 2 * z - 1);
    set("t2", 1);
    set("t3", 0.5, 0.5);
    set("v0", 1, 1, 1, 1);
    set("c0", 2.0 / kN);
    set("c1", 0.2 - 1);
    set("c2", 0.5);
    set("c3", 0, 0, 0, 1);
    set("c4", kN);
    set("c5", kN / 4.0, 4.0 / kN, 1.0 / kN);
}

// The game's own terrain pass up to its bilinear quad (line 50: r1.w = 1 - the quad's lit share) against a 1-texel box
// centred 1.5 texels off, compared with no epsilon: the same thing if the rewrite's box sits where the quad sat.
static int terrain_vanilla(const Subject& s) {
    std::vector<std::string> lines(s.plan.code.begin(), s.plan.code.begin() + 51);
    Subject q = s;
    q.lit = [](std::map<std::string, emu::V4>& r) { return 1.0 - r["r1"].v[3]; };
    std::vector<float> depth(size_t(kN) * kN);
    for (int y = 0; y < kN; ++y)
        for (int x = 0; x < kN; ++x) depth[size_t(y) * kN + x] = float(0.5 + (x < 32 ? -0.2 : 0) + 0.0006 * (x + 0.5));
    unsigned seed = 777;
    double max_diff = 0;
    std::string error;
    for (int i = 0; i < 2000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double cu = 26 + 12 * ((seed >> 8) / double(1 << 24));
        seed = seed * 1664525u + 1013904223u;
        const double cv = 16 + 32 * ((seed >> 8) / double(1 << 24));
        const double u = (cu + 1.5) / kN, v = (cv + 1.5) / kN;
        const double lit = emulate(q, lines, depth, u, v, 0.0006, 0, error);
        const double direct = emu::reference(depth, kN, u, v, 0.5 + 0.0006 * u * kN, 0, 1, false, 0, 0, -1.5);
        max_diff = std::max(max_diff, fabs(lit - direct));
    }
    const bool ok = error.empty() && max_diff < 1e-4;
    printf("self-test terrain: the game's quad vs a 1-texel box 1.5 texels off: %s - max difference %.2g%s\n",
           ok ? "ok  " : "FAIL", max_diff, error.empty() ? "" : (", " + error).c_str());
    return ok ? 0 : 1;
}

static int selftest(Assemble_t assemble, IDirect3DDevice9* dev, const std::string& terrain_text) {
    int bad = 0;
    Subject one;
    one.name = "one tap";
    std::string why;
    if (!rol::sg::analyse_shadow(kMinimal, one.plan, &why)) {
        printf("self-test: the minimal shader not recognised (%s)\n", why.c_str());
        return 1;
    }
    one.eps = 0.0003;
    one.inputs = [](std::map<std::string, emu::V4>& r, double u, double v, double z) {
        emu::V4 t0, t1, c0;
        t0.v[0] = float(u);
        t0.v[1] = float(v);
        t1.v[0] = float(z);
        c0.v[0] = 0.0003f;
        r["t0"] = t0;
        r["t1"] = t1;
        r["c0"] = c0;
    };
    one.lit = [](std::map<std::string, emu::V4>& r) { return double(r["oC0"].v[0]); };
    bad += run_subject(one, assemble, dev);
    if (terrain_text.empty()) {
        printf("self-test terrain: no terrain_shadow_pass dump in the folder - skipped\n");
        return bad;
    }
    Subject t;
    t.name = "terrain";
    if (!rol::sg::analyse_shadow(terrain_text, t.plan, &why) || !t.plan.terrain) {
        printf("self-test terrain: the dumped pass is not the known one (%s)\n", why.c_str());
        return bad + 1;
    }
    t.eps = rol::sg::kTerrainEps;
    t.shift = -1.5;
    t.inputs = terrain_inputs;
    t.lit = [](std::map<std::string, emu::V4>& r) { return 1.0 - r["oC0"].v[3] / 0.5; };
    bad += terrain_vanilla(t);
    bad += run_subject(t, assemble, dev);
    return bad;
}

// ---- The AO renderer (ao.cpp) on a synthetic scene at the game's scale: the game's projection (near 100, far 18116,
// half angles 0.742 x 0.311 at 3440 x 1440), the camera 7500 away looking down at 45 degrees, flat ground and a cube
// 300 on a side standing on it, drawn into the renderer's own INTZ. Numbers read back from the GPU: open ground and the
// cube's top stay unoccluded, the ground at the cube's foot darkens, no halo on the ground seen just past the cube's
// silhouette; the composite multiplies a white target (and an 8x multisampled one, resolved) by exactly the AO.
namespace aot {

struct M4 {
    double m[4][4];
};

M4 mul(const M4& a, const M4& b) {
    M4 r = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// D3D conventions: row vectors (v' = v M), left-handed, the camera looking down +z.
M4 look_at(const double eye[3], const double at[3]) {
    double z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    double l = sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    for (double& c : z) c /= l;
    const double up[3] = {0, 1, 0};
    double x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    l = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (double& c : x) c /= l;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    M4 v = {};
    for (int i = 0; i < 3; ++i) {
        v.m[i][0] = x[i];
        v.m[i][1] = y[i];
        v.m[i][2] = z[i];
    }
    v.m[3][0] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    v.m[3][1] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    v.m[3][2] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    v.m[3][3] = 1;
    return v;
}

M4 perspective(double tx, double ty, double n, double f) {
    M4 p = {};
    p.m[0][0] = 1 / tx;
    p.m[1][1] = 1 / ty;
    p.m[2][2] = f / (f - n);
    p.m[2][3] = 1;
    p.m[3][2] = -n * f / (f - n);
    return p;
}

// A world point's pixel (x right, y down) under view-projection vp on a w x h target.
bool project(const M4& vp, double x, double y, double z, int w, int h, double& px, double& py) {
    const double in[4] = {x, y, z, 1};
    double o[4] = {};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) o[j] += in[k] * vp.m[k][j];
    if (o[3] <= 0) return false;
    px = (o[0] / o[3] * 0.5 + 0.5) * w;
    py = (0.5 - o[1] / o[3] * 0.5) * h;
    return true;
}

float half_to_float(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    uint32_t b;
    if (e == 0) {
        if (!m) b = s << 31;
        else {   // denormal
            float v = std::ldexp(float(m), -24);
            return s ? -v : v;
        }
    } else if (e == 31) {
        b = (s << 31) | 0x7F800000 | (m << 13);
    } else {
        b = (s << 31) | ((e + 112) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &b, 4);
    return f;
}

HRESULT STDMETHODCALLTYPE set_rt(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { return d->SetRenderTarget(i, s); }
HRESULT STDMETHODCALLTYPE set_tex(IDirect3DDevice9* d, DWORD i, IDirect3DBaseTexture9* t) { return d->SetTexture(i, t); }
HRESULT STDMETHODCALLTYPE set_samp(IDirect3DDevice9* d, DWORD i, D3DSAMPLERSTATETYPE t, DWORD v) {
    return d->SetSamplerState(i, t, v);
}
HRESULT STDMETHODCALLTYPE set_vp(IDirect3DDevice9* d, const D3DVIEWPORT9* v) { return d->SetViewport(v); }
HRESULT STDMETHODCALLTYPE set_vs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return d->SetVertexShader(s); }
HRESULT STDMETHODCALLTYPE set_ps(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { return d->SetPixelShader(s); }

const char kSceneHlsl[] =
    "row_major float4x4 g_wvp : register(c0);\n"
    "float4 vs_main(float4 p : POSITION) : POSITION { return mul(p, g_wvp); }\n"
    "float4 ps_main() : COLOR { return 1; }\n";

}  // namespace aot

static int ao_test(IDirect3DDevice9* dev, rol::ao::Compile_t compile) {
    using namespace aot;
    if (!dev || !compile) {
        printf("AO test: no device or no compiler - skipped\n");
        return 0;
    }
    const UINT W = 3440, H = 1440;
    rol::ao::Renderer r;
    rol::ao::Calls calls = {set_rt, set_tex, set_samp, set_vp, set_vs, set_ps};
    std::string why;
    if (!r.init(dev, compile, calls, W, H, &why)) {
        printf("AO test: FAIL - the renderer was not made: %.400s\n", why.c_str());
        return 1;
    }
    // the scene's shaders
    IUnknown *vsc = nullptr, *psc = nullptr, *err = nullptr;
    compile(aot::kSceneHlsl, UINT(sizeof(aot::kSceneHlsl) - 1), nullptr, nullptr, "vs_main", "vs_3_0", 0, &vsc, &err, nullptr);
    compile(aot::kSceneHlsl, UINT(sizeof(aot::kSceneHlsl) - 1), nullptr, nullptr, "ps_main", "ps_3_0", 0, &psc, &err, nullptr);
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    if (!vsc || !psc || FAILED(dev->CreateVertexShader(buffer_code(vsc), &vs)) ||
        FAILED(dev->CreatePixelShader(buffer_code(psc), &ps))) {
        printf("AO test: FAIL - the scene's shaders were not made\n");
        return 1;
    }
    release(vsc);
    release(psc);
    IDirect3DVertexDeclaration9* decl = nullptr;
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    D3DDECL_END()};
    dev->CreateVertexDeclaration(el, &decl);
    // the camera and the scene
    const double n = 100, f = 18116, tx = 0.742, ty = 0.311;
    const double az = 30 * 3.14159265358979 / 180;   // the camera 30 degrees round: the front and right faces seen
    const double eye[3] = {7500 * 0.70710678 * sin(az), 7500 * 0.70710678, -7500 * 0.70710678 * cos(az)}, at[3] = {0, 0, 0};
    const M4 vp = mul(look_at(eye, at), perspective(tx, ty, n, f));
    float c[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) c[i * 4 + j] = float(vp.m[i][j]);
    std::vector<float> tri;
    auto quad = [&](const double a[3], const double b[3], const double cc[3], const double d[3]) {
        for (const double* p : {a, b, cc, a, cc, d}) tri.insert(tri.end(), {float(p[0]), float(p[1]), float(p[2]), 1.0f});
    };
    {
        const double g0[3] = {-9000, 0, -9000}, g1[3] = {-9000, 0, 9000}, g2[3] = {9000, 0, 9000}, g3[3] = {9000, 0, -9000};
        quad(g0, g1, g2, g3);
        const double s = 150, h = 300;   // the cube
        const double p[8][3] = {{-s, 0, -s}, {s, 0, -s}, {s, 0, s}, {-s, 0, s}, {-s, h, -s}, {s, h, -s}, {s, h, s}, {-s, h, s}};
        const int faces[6][4] = {{0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}, {4, 5, 6, 7}, {3, 2, 1, 0}};
        for (const auto& fc : faces) quad(p[fc[0]], p[fc[1]], p[fc[2]], p[fc[3]]);
    }
    dev->BeginScene();
    dev->SetRenderTarget(0, r.null_target());
    dev->SetDepthStencilSurface(r.depth());
    dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetVertexDeclaration(decl);
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    dev->SetVertexShaderConstantF(0, c, 4);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(tri.size() / 12), tri.data(), 16);
    const rol::ao::View view = {float(n), float(f), float(tx), float(ty)};
    rol::ao::Params prm = {90, 3.0f, 0.1f, 1.0f};   // rolhook's level 2
    // the passes' GPU time: timestamps around compute (AO + 2 blurs) and the composite onto the 8x target
    IDirect3DQuery9 *qd = nullptr, *qf = nullptr, *qt[4] = {};
    bool timed = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &qd)) &&
                 SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &qf));
    for (auto*& q : qt) timed = timed && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q));
    if (timed) {
        qd->Issue(D3DISSUE_BEGIN);
        qt[0]->Issue(D3DISSUE_END);
    }
    r.compute(dev, view, prm);
    if (timed) qt[1]->Issue(D3DISSUE_END);
    // the composite onto a white target and onto a white 8x multisampled one (resolved for reading)
    IDirect3DSurface9 *white = nullptr, *msaa = nullptr, *resolved = nullptr;
    dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &white, nullptr);
    dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_8_SAMPLES, 0, FALSE, &msaa, nullptr);
    dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr);
    dev->SetRenderTarget(0, r.mask());   // nothing over the scene
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    for (IDirect3DSurface9* t : {white, msaa}) {
        if (!t) continue;
        dev->SetRenderTarget(0, t);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
        if (timed && t == msaa) qt[2]->Issue(D3DISSUE_END);
        r.composite(dev, t, prm);
        if (timed && t == msaa) qt[3]->Issue(D3DISSUE_END);
    }
    if (timed) {
        qf->Issue(D3DISSUE_END);
        qd->Issue(D3DISSUE_END);
    }
    if (msaa && resolved) dev->StretchRect(msaa, nullptr, resolved, nullptr, D3DTEXF_NONE);
    // the mask: its left half covered (alpha 1) - the composite must leave a white target white there
    IDirect3DSurface9* masked = nullptr;
    dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &masked, nullptr);
    if (masked) {
        const D3DRECT left = {0, 0, LONG(W / 2), LONG(H)};
        dev->SetRenderTarget(0, r.mask());
        dev->Clear(1, &left, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        dev->SetRenderTarget(0, masked);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);
        r.composite(dev, masked, prm);
    }
    dev->EndScene();
    // read back
    auto read_g16 = [&](std::vector<float>& ao, std::vector<float>& z) {
        IDirect3DSurface9 *src = nullptr, *sys = nullptr;
        r.result()->GetSurfaceLevel(0, &src);
        dev->CreateOffscreenPlainSurface(W, H, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        bool ok = src && sys && SUCCEEDED(dev->GetRenderTargetData(src, sys));
        D3DLOCKED_RECT lr;
        if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            ao.resize(size_t(W) * H);
            z.resize(size_t(W) * H);
            for (UINT y = 0; y < H; ++y) {
                const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (UINT x = 0; x < W; ++x) {
                    ao[size_t(y) * W + x] = half_to_float(row[x * 2]);
                    z[size_t(y) * W + x] = half_to_float(row[x * 2 + 1]);
                }
            }
            sys->UnlockRect();
        } else {
            ok = false;
        }
        if (src) src->Release();
        if (sys) sys->Release();
        return ok;
    };
    auto read_x8 = [&](IDirect3DSurface9* t, std::vector<float>& v) {
        IDirect3DSurface9* sys = nullptr;
        dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        bool ok = t && sys && SUCCEEDED(dev->GetRenderTargetData(t, sys));
        D3DLOCKED_RECT lr;
        if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            v.resize(size_t(W) * H);
            for (UINT y = 0; y < H; ++y) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (UINT x = 0; x < W; ++x) v[size_t(y) * W + x] = ((row[x] >> 8) & 0xFF) / 255.0f;   // green
            }
            sys->UnlockRect();
        } else {
            ok = false;
        }
        if (sys) sys->Release();
        return ok;
    };
    std::vector<float> ao, zz, comp, comp8;
    int bad = 0;
    if (!read_g16(ao, zz)) {
        printf("AO test: FAIL - the AO could not be read back\n");
        return 1;
    }
    size_t nans = 0;
    for (float v : ao)
        if (!(v == v)) ++nans;
    // a world point's AO: the mean over the 5x5 pixels around its projection
    auto ao_at = [&](double x, double y, double z, double dy_px = 0) {
        double px, py;
        if (!project(vp, x, y, z, W, H, px, py)) return -1.0;
        py += dy_px;
        double s = 0;
        int k = 0;
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) {
                const int X = int(px) + i, Y = int(py) + j;
                if (X < 0 || Y < 0 || X >= int(W) || Y >= int(H)) continue;
                s += ao[size_t(Y) * W + X];
                ++k;
            }
        return k ? s / k : -1.0;
    };
    const double open = ao_at(1500, 0, 600), top = ao_at(0, 300, 0), face = ao_at(0, 200, -150);
    const double foot_front = ao_at(0, 0, -160), foot_side = ao_at(160, 0, 0);   // 10 units from the faces
    // the ground seen just past the cube's far top edge (y = 300, z = +150): 6 px above it on screen lies ground
    // ~1000 units farther away - its AO must not see the cube
    const double past = ao_at(0, 300, 150, -6);
    // depth in G: the cube's top at its centre, against the expected view depth
    double tpx, tpy;
    project(vp, 0, 300, 0, W, H, tpx, tpy);
    const double z_read = zz[size_t(int(tpy)) * W + int(tpx)] * f;
    const double dx0 = 0 - eye[0], dy0 = 300 - eye[1], dz0 = 0 - eye[2];
    const double zdir[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const double zl = sqrt(zdir[0] * zdir[0] + zdir[1] * zdir[1] + zdir[2] * zdir[2]);
    const double z_true = (dx0 * zdir[0] + dy0 * zdir[1] + dz0 * zdir[2]) / zl;
    const bool depth_ok = fabs(z_read - z_true) < 0.002 * z_true;
    printf("AO test profile (distance from the cube's face: front / right):");
    for (double dist : {2.0, 5.0, 10.0, 20.0, 40.0, 60.0, 80.0, 100.0})
        printf("  %g: %.3f / %.3f", dist, ao_at(0, 0, -150 - dist), ao_at(150 + dist, 0, 0));
    printf("\n");
    const bool pass1 = !nans && open > 0.97 && top > 0.97 && face > 0.95 && foot_front < 0.85 && foot_side < 0.85 &&
                       past > 0.95 && depth_ok;
    if (!pass1) ++bad;
    if (timed) {
        BOOL disjoint = TRUE;
        UINT64 fq = 0, t[4] = {};
        bool got = true;
        for (int spin = 0; spin < 2000 && qd->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) != S_OK; ++spin)
            Sleep(1);
        got = qf->GetData(&fq, sizeof(fq), D3DGETDATA_FLUSH) == S_OK && fq && !disjoint;
        for (int k = 0; k < 4 && got; ++k) got = qt[k]->GetData(&t[k], sizeof(t[k]), D3DGETDATA_FLUSH) == S_OK;
        if (got)
            printf("AO test GPU time at 3440 x 1440: AO and blurs %.3f ms, composite onto the 8x target %.3f ms\n",
                   double(t[1] - t[0]) * 1000.0 / fq, double(t[3] - t[2]) * 1000.0 / fq);
    }
    for (IDirect3DQuery9* q : {qd, qf, qt[0], qt[1], qt[2], qt[3]})
        if (q) q->Release();
    printf("AO test (W %u x %u, radius 90): %s - open ground %.3f, cube top %.3f, front face %.3f, at its foot front "
           "%.3f side %.3f, ground past the silhouette %.3f; view depth of the top %.1f (true %.1f); NaN %zu\n",
           W, H, pass1 ? "ok  " : "FAIL", open, top, face, foot_front, foot_side, past, z_read, z_true, nans);
    // the composite
    double worst = 0, worst8 = 0;
    if (read_x8(white, comp))
        for (size_t i = 0; i < comp.size(); ++i) worst = std::max(worst, fabs(double(comp[i]) - ao[i]));
    else
        worst = 9;
    size_t at8 = 0;
    if (read_x8(resolved, comp8)) {
        for (size_t i = 0; i < comp8.size(); ++i)
            if (fabs(double(comp8[i]) - ao[i]) > worst8) worst8 = fabs(double(comp8[i]) - ao[i]), at8 = i;
        size_t over = 0;
        for (size_t i = 0; i < comp8.size(); ++i)
            if (fabs(double(comp8[i]) - ao[i]) > 1.5 / 255) ++over;
        printf("AO test composite, 8x: worst at pixel %zu,%zu (target %.4f, AO %.4f), %zu pixels off by more than 1.5/255\n",
               at8 % W, at8 / W, comp8[at8], ao[at8], over);
    } else {
        worst8 = 9;
    }
    const bool pass2 = worst <= 1.5 / 255 && worst8 <= 1.5 / 255;
    if (!pass2) ++bad;
    printf("AO test composite: %s - white x AO differs from the AO by %.4f at most, on an 8x multisampled target %.4f\n",
           pass2 ? "ok  " : "FAIL", worst, worst8);
    std::vector<float> cm;
    double left_off = 9, right_off = 9;
    if (masked && read_x8(masked, cm)) {
        left_off = right_off = 0;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                if (x < W / 2) left_off = std::max(left_off, fabs(double(cm[i]) - 1.0));
                else right_off = std::max(right_off, fabs(double(cm[i]) - ao[i]));
            }
    }
    const bool pass3 = left_off <= 1.5 / 255 && right_off <= 1.5 / 255;
    if (!pass3) ++bad;
    printf("AO test mask: %s - under the mask the target stays as it was (off by %.4f), beside it white x AO (off by "
           "%.4f)\n",
           pass3 ? "ok  " : "FAIL", left_off, right_off);
    if (masked) masked->Release();
    if (white) white->Release();
    if (msaa) msaa->Release();
    if (resolved) resolved->Release();
    vs->Release();
    ps->Release();
    if (decl) decl->Release();
    r.release();
    return bad;
}

// ---- The sharpening and vibrance pass (post.cpp) on a synthetic image at the game's size and sampling (3440 x 1440,
// MSAA 8x): an image made on the CPU (dull gradient, a flat patch, a step, a disc with an antialiased rim, a 2-pixel
// checker, black against white, colour patches, noise) is copied into the multisampled target, the pass runs, and
// every pixel is compared with the same formulas computed on the CPU from the resolved input (post::cas_vibrance).
// Numbers for the record: what the levels do to a step, to fine detail, to noise and to colours; the GPU time.
namespace postt {

const char kCopyHlsl[] =
    "float4 g_size : register(c0);\n"
    "sampler2D s_img : register(s0);\n"
    "float4 vs_main(float4 p : POSITION) : POSITION { return p; }\n"
    "float4 ps_main(float2 vpos : VPOS) : COLOR { return tex2Dlod(s_img, float4((vpos + 0.5) * g_size.xy, 0, 0)); }\n";

struct Img {
    UINT w = 0, h = 0;
    std::vector<uint8_t> rgb;   // 3 bytes a pixel
    float at(UINT x, UINT y, int k) const { return rgb[(size_t(y) * w + x) * 3 + k] / 255.0f; }
};

uint8_t q8(double v) { return uint8_t(v <= 0 ? 0 : (v >= 1 ? 255 : int(v * 255 + 0.5))); }

HRESULT STDMETHODCALLTYPE stretch(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t,
                                  const RECT* tr, D3DTEXTUREFILTERTYPE f) {
    return d->StretchRect(s, sr, t, tr, f);
}

Img make_image(UINT W, UINT H) {
    Img im;
    im.w = W;
    im.h = H;
    im.rgb.resize(size_t(W) * H * 3);
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / double(1 << 24);
    };
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            const double t = double(x) / W;
            double c[3] = {0.30 + 0.25 * t, 0.34 + 0.16 * t, 0.40 + 0.02 * t};   // dull gradient
            auto in = [&](UINT x0, UINT y0, UINT x1, UINT y1) { return x >= x0 && x < x1 && y >= y0 && y < y1; };
            auto set = [&](double r, double g, double b) { c[0] = r, c[1] = g, c[2] = b; };
            if (in(100, 100, 700, 500)) set(0.45, 0.45, 0.45);                               // flat
            if (in(800, 100, 1400, 500)) x < 1100 ? set(0.3, 0.3, 0.3) : set(0.6, 0.6, 0.6);   // step
            if (in(1500, 100, 2100, 500)) {                                                  // disc, 1 px rim
                const double dx = x + 0.5 - 1800, dy = y + 0.5 - 300, r = sqrt(dx * dx + dy * dy);
                const double cov = r < 149.5 ? 1 : (r > 150.5 ? 0 : 150.5 - r);
                for (int k = 0; k < 3; ++k) c[k] = (k == 0 ? 0.8 : k == 1 ? 0.15 : 0.1) * cov +
                                                   (k == 0 ? 0.25 : k == 1 ? 0.3 : 0.4) * (1 - cov);
            }
            if (in(2200, 100, 2800, 500)) ((x / 2 + y / 2) & 1) ? set(0.55, 0.55, 0.55) : set(0.45, 0.45, 0.45);
            if (in(2900, 100, 3300, 500)) x < 3100 ? set(0, 0, 0) : set(1, 1, 1);           // black | white
            static const double patch[7][3] = {{0.5, 0.5, 0.5},    {0.55, 0.45, 0.35}, {0.4, 0.5, 0.4},  {0.85, 0.12, 0.1},
                                               {0.1, 0.2, 0.9},    {0.8, 0.6, 0.5},    {0.15, 0.12, 0.1}};
            if (y >= 600 && y < 1000 && x >= 100 && x < 100 + 7 * 450 && (x - 100) % 450 < 400) {
                const double* pc = patch[(x - 100) / 450];
                set(pc[0], pc[1], pc[2]);
            }
            if (in(100, 1100, 1100, 1400)) {   // noise around grey
                const double n = (rnd() - 0.5) * 0.08;
                set(0.5 + n, 0.5 + n, 0.5 + n);
            }
            for (int k = 0; k < 3; ++k) im.rgb[(size_t(y) * W + x) * 3 + k] = q8(c[k]);
        }
    return im;
}

bool read_rgb(IDirect3DDevice9* dev, IDirect3DSurface9* rt, UINT W, UINT H, Img& out) {
    IDirect3DSurface9* sys = nullptr;
    dev->CreateOffscreenPlainSurface(W, H, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    bool ok = rt && sys && SUCCEEDED(dev->GetRenderTargetData(rt, sys));
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        out.w = W;
        out.h = H;
        out.rgb.resize(size_t(W) * H * 3);
        for (UINT y = 0; y < H; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < W; ++x) {
                uint8_t* o = &out.rgb[(size_t(y) * W + x) * 3];
                o[0] = uint8_t(row[x] >> 16), o[1] = uint8_t(row[x] >> 8), o[2] = uint8_t(row[x]);
            }
        }
        sys->UnlockRect();
    } else {
        ok = false;
    }
    if (sys) sys->Release();
    return ok;
}

// The pass on the CPU from 'in', quantised as the target stores it.
void reference(const Img& in, const rol::post::Params& p, Img& out) {
    out = in;
    const UINT W = in.w, H = in.h;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            float n[9][3];
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const UINT X = UINT(std::min(std::max(int(x) + i, 0), int(W) - 1));
                    const UINT Y = UINT(std::min(std::max(int(y) + j, 0), int(H) - 1));
                    for (int k = 0; k < 3; ++k) n[(j + 1) * 3 + i + 1][k] = in.at(X, Y, k);
                }
            float o[3];
            rol::post::cas_vibrance(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], p, o);
            for (int k = 0; k < 3; ++k) out.rgb[(size_t(y) * W + x) * 3 + k] = q8(o[k]);
        }
}

double sat_of(const Img& im, UINT x, UINT y) {
    const float r = im.at(x, y, 0), g = im.at(x, y, 1), b = im.at(x, y, 2);
    return std::max({r, g, b}) - std::min({r, g, b});
}

}  // namespace postt

static int post_test(IDirect3DDevice9* dev, rol::ao::Compile_t compile) {
    using namespace postt;
    if (!dev || !compile) {
        printf("post test: no device or no compiler - skipped\n");
        return 0;
    }
    const UINT W = 3440, H = 1440;
    rol::ao::Calls calls = {aot::set_rt, aot::set_tex, aot::set_samp, aot::set_vp, aot::set_vs, aot::set_ps, stretch};
    rol::post::Renderer r;
    std::string why;
    if (!r.init(dev, compile, calls, W, H, D3DFMT_X8R8G8B8, &why)) {
        printf("post test: FAIL - the renderer was not made: %.400s\n", why.c_str());
        return 1;
    }
    // the image, its texture, the copy shader, the 8x target and a plain one to resolve the result into
    const Img img = make_image(W, H);
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9 *msaa = nullptr, *resolved = nullptr;
    IUnknown *vsc = nullptr, *psc = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    compile(kCopyHlsl, UINT(sizeof(kCopyHlsl) - 1), nullptr, nullptr, "vs_main", "vs_3_0", 0, &vsc, nullptr, nullptr);
    compile(kCopyHlsl, UINT(sizeof(kCopyHlsl) - 1), nullptr, nullptr, "ps_main", "ps_3_0", 0, &psc, nullptr, nullptr);
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                    D3DDECL_END()};
    bool ok = vsc && psc && SUCCEEDED(dev->CreateVertexShader(buffer_code(vsc), &vs)) &&
              SUCCEEDED(dev->CreatePixelShader(buffer_code(psc), &ps)) && SUCCEEDED(dev->CreateVertexDeclaration(el, &decl)) &&
              SUCCEEDED(dev->CreateTexture(W, H, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) &&
              SUCCEEDED(dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_8_SAMPLES, 0, FALSE, &msaa, nullptr)) &&
              SUCCEEDED(dev->CreateRenderTarget(W, H, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr));
    release(vsc);
    release(psc);
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
        for (UINT y = 0; y < H; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
            for (UINT x = 0; x < W; ++x) {
                const uint8_t* c = &img.rgb[(size_t(y) * W + x) * 3];
                row[x] = 0xFF000000u | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
            }
        }
        tex->UnlockRect(0);
    } else {
        ok = false;
    }
    if (!ok) {
        printf("post test: FAIL - the test image, its target or the copy shader were not made\n");
        return 1;
    }
    auto draw_image = [&]() {
        static const float tri[12] = {-2, -2, 0, 1, 6, -2, 0, 1, -2, 6, 0, 1};
        const float size[4] = {1.0f / W, 1.0f / H, float(W), float(H)};
        dev->SetRenderTarget(0, msaa);
        dev->SetDepthStencilSurface(nullptr);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        for (D3DSAMPLERSTATETYPE t : {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER}) dev->SetSamplerState(0, t, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->SetPixelShaderConstantF(0, size, 1);
        dev->SetTexture(0, tex);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 16);
        dev->SetTexture(0, nullptr);
    };
    // the runs: each level of each alone, then both at level 2 (timed)
    struct Run {
        const char* name;
        rol::post::Params p;
    };
    const Run runs[] = {{"sharpen 1", {0.4f, 0}},     {"sharpen 2", {0.8f, 0}},     {"sharpen 3", {1.6f, 0}},
                        {"vibrance 1", {0, 0.12f}},   {"vibrance 2", {0, 0.25f}},   {"vibrance 3", {0, 0.4f}},
                        {"both 2", {0.8f, 0.25f}}};
    IDirect3DQuery9 *qd = nullptr, *qf = nullptr, *qt[2] = {};
    bool timed = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &qd)) &&
                 SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &qf));
    for (auto*& q : qt) timed = timed && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &q));
    int bad = 0;
    std::string step_line, detail_line, noise_line;
    for (const Run& run : runs) {
        const bool time_it = timed && !strcmp(run.name, "both 2");
        dev->BeginScene();
        draw_image();
        if (time_it) {   // a pass run first, then the image drawn again and finished: the timestamps hold the pass alone
            r.apply(dev, msaa, run.p);
            draw_image();
            IDirect3DQuery9* ev = nullptr;
            if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &ev))) {
                ev->Issue(D3DISSUE_END);
                for (int spin = 0; spin < 2000 && ev->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE; ++spin) Sleep(1);
                ev->Release();
            }
            qd->Issue(D3DISSUE_BEGIN);
            qt[0]->Issue(D3DISSUE_END);
            r.apply(dev, msaa, run.p);
            qt[1]->Issue(D3DISSUE_END);
            qf->Issue(D3DISSUE_END);
            qd->Issue(D3DISSUE_END);
        } else {
            r.apply(dev, msaa, run.p);
        }
        dev->EndScene();
        dev->StretchRect(msaa, nullptr, resolved, nullptr, D3DTEXF_NONE);
        Img in, out, ref;
        IDirect3DSurface9* src = nullptr;
        r.source()->GetSurfaceLevel(0, &src);
        const bool got = read_rgb(dev, src, W, H, in) && read_rgb(dev, resolved, W, H, out);
        if (src) src->Release();
        if (!got) {
            printf("post test %s: FAIL - not read back\n", run.name);
            ++bad;
            continue;
        }
        // the resolve is exact (every sample of a pixel holds the same colour); the pass matches the formulas
        size_t resolve_off = 0, off1 = 0, off2 = 0;
        for (size_t i = 0; i < img.rgb.size(); ++i) resolve_off += in.rgb[i] != img.rgb[i];
        reference(in, run.p, ref);
        int worst = 0;
        for (size_t i = 0; i < ref.rgb.size(); ++i) {
            const int dlt = abs(int(out.rgb[i]) - int(ref.rgb[i]));
            worst = std::max(worst, dlt);
            off1 += dlt == 1;
            off2 += dlt > 1;
        }
        // what it does: the flat patch, the step's bright and dark sides, the checker's contrast, the noise, colours
        double flat_off = 0;
        for (UINT y = 150; y < 450; y += 7)
            for (UINT x = 150; x < 650; x += 7)
                for (int k = 0; k < 3; ++k) flat_off = std::max(flat_off, fabs(double(out.at(x, y, k)) - in.at(x, y, k)));
        double over = 0, under = 0;
        for (UINT x = 1090; x < 1110; ++x) {
            over = std::max(over, double(out.at(x, 300, 1)) - 0.6);
            under = std::max(under, 0.3 - double(out.at(x, 300, 1)));
        }
        auto spread = [&](const Img& im, UINT x0, UINT y0, UINT x1, UINT y1) {
            double sum = 0, sq = 0;
            size_t n = 0;
            for (UINT y = y0; y < y1; ++y)
                for (UINT x = x0; x < x1; ++x) {
                    const double v = im.at(x, y, 1);
                    sum += v, sq += v * v, ++n;
                }
            const double m = sum / n;
            return sqrt(std::max(0.0, sq / n - m * m));
        };
        const double detail = spread(out, 2300, 200, 2700, 400) / spread(in, 2300, 200, 2700, 400);
        const double noise = spread(out, 200, 1150, 1000, 1350) / spread(in, 200, 1150, 1000, 1350);
        double black = 0, white = 1;
        for (UINT y = 150; y < 450; y += 5) {
            black = std::max(black, double(out.at(3000, y, 1)));
            white = std::min(white, double(out.at(3200, y, 1)));
        }
        std::string colours;
        static const char* const pname[7] = {"grey", "dull orange", "dull green", "red", "blue", "skin", "dark"};
        for (int pi = 0; pi < 7; ++pi) {
            const UINT x = 100 + pi * 450 + 200, y = 800;
            char b[64];
            snprintf(b, sizeof(b), "%s%s %.3f->%.3f", pi ? ", " : "", pname[pi], sat_of(in, x, y), sat_of(out, x, y));
            colours += b;
        }
        const bool pass = !resolve_off && worst <= 1 && flat_off <= 1.0 / 255 && black == 0 && white == 1 &&
                          sat_of(out, 100 + 200, 800) <= 1.0 / 255;
        if (!pass) ++bad;
        printf("post test %-10s: %s - vs the CPU: worst %d/255 (%zu pixels off by 1, %zu by more), resolve exact %s; flat "
               "patch moved %.4f; step 0.3|0.6: overshoot %+.3f, undershoot %+.3f; checker contrast x%.2f, noise x%.2f; "
               "black %.3f, white %.3f\n",
               run.name, pass ? "ok  " : "FAIL", worst, off1, off2, resolve_off ? "NO" : "yes", flat_off, over, under,
               detail, noise, black, white);
        if (run.p.vibrance > 0) printf("    saturation (max - min): %s\n", colours.c_str());
    }
    if (timed) {
        BOOL disjoint = TRUE;
        UINT64 fq = 0, t[2] = {};
        for (int spin = 0; spin < 2000 && qd->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) != S_OK; ++spin)
            Sleep(1);
        bool got = qf->GetData(&fq, sizeof(fq), D3DGETDATA_FLUSH) == S_OK && fq && !disjoint;
        for (int k = 0; k < 2 && got; ++k) got = qt[k]->GetData(&t[k], sizeof(t[k]), D3DGETDATA_FLUSH) == S_OK;
        if (got)
            printf("post test GPU time at %u x %u, 8x: resolve + pass %.3f ms\n", W, H, double(t[1] - t[0]) * 1000.0 / fq);
    }
    for (IDirect3DQuery9* q : {qd, qf, qt[0], qt[1]})
        if (q) q->Release();
    tex->Release();
    msaa->Release();
    resolved->Release();
    vs->Release();
    ps->Release();
    decl->Release();
    r.release();
    return bad;
}

// ---- The gamma-correct light on a minimal shader lit as the buildings' and units' are (a normal map, the hemispheric
// ambient from the preshader's sky - ground and hemispheric_ground_color, N.L times light_diffuse, times the albedo),
// emulated against the result computed directly for a run of N.L values. Two N.L to remap: dp3_sat with the light
// vector normalised into r1, and dp3 with the raw t1 saturated by a mov_sat later; r1 then takes another vector (the
// up axis), and the dot product with it - the ambient's factor - must be left alone.
static const char kGammaShader[] =
    "//   hemispheric_ground_color c2       1\n"
    "//   light_diffuse            c3       1\n"
    "    ps_2_0\n"
    "    def c4, 2, -1, 1, 0\n"
    "    def c6, 0.15, 0.15, 0.2, 0\n"
    "    dcl t0.xy\n"
    "    dcl t1.xyz\n"
    "    dcl t3.xyz\n"
    "    dcl_2d s0\n"
    "    dcl_2d s1\n"
    "    texld r0, t0, s1\n"
    "    mad r0.xyz, c4.x, r0, c4.y\n"
    "    nrm r2.xyz, r0\n"
    "    nrm r1.xyz, t1\n"
    "    dp3_sat r3.x, r2, r1\n"
    "    nrm r1.xyz, t3\n"
    "    dp3 r3.y, r2, r1\n"
    "    mov_sat r3.z, r3.y\n"
    "    dp3 r7.x, r2, t1\n"
    "    mov r4.xyz, c6\n"
    "    mad r5.xyz, r3.z, r4, c2\n"
    "    mad r5.xyz, r3.x, c3, r5\n"
    "    mov_sat r7.y, r7.x\n"
    "    texld r6, t0, s0\n"
    "    mul r5.xyz, r5, r6\n"
    "    mov r5.w, r7.y\n"
    "    mov oC0, r5\n";

static int gamma_selftest(Assemble_t assemble, IDirect3DDevice9* dev) {
    rol::sg::LightPlan lp;
    std::string why;
    if (!rol::sg::analyse_light(kGammaShader, lp, &why)) {
        printf("gamma self-test: FAIL - the minimal shader's N.L not found (%s)\n", why.c_str());
        return 1;
    }
    std::string found;
    for (const std::string& t : lp.targets) found += " " + t;
    const bool plan_ok = lp.targets.size() == 2 && lp.targets[0] == "r3.x" && lp.targets[1] == "r7.y" &&
                         lp.sky_diff == "c6" && lp.ground == "c2" && lp.light == "c3";
    printf("gamma self-test: N.L in%s (want r3.x r7.y), sky - ground %s, ground %s, light %s - %s\n", found.c_str(),
           lp.sky_diff.c_str(), lp.ground.c_str(), lp.light.c_str(), plan_ok ? "ok" : "FAIL");
    int bad = plan_ok ? 0 : 1;
    auto norm = [](double v[3]) {
        const double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int k = 0; k < 3; ++k) v[k] /= l;
    };
    auto dot = [](const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    auto sat = [](double x) { return x < 0 ? 0 : (x > 1 ? 1 : x); };
    auto lum = [](const double c[3]) { return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; };
    const double ground[3] = {0.2, 0.25, 0.3}, sky_diff[3] = {0.15, 0.15, 0.2}, light[3] = {0.9, 0.85, 0.7};
    const double albedo[3] = {0.6, 0.5, 0.4}, lraw = 0.7;
    const double mid[3] = {ground[0] + 0.5 * sky_diff[0], ground[1] + 0.5 * sky_diff[1], ground[2] + 0.5 * sky_diff[2]};
    const double g = rol::sg::kGamma, A = lum(mid), Ll = std::max(lum(light), 0.01);
    const double B = pow(A + Ll, g) - pow(A, g);
    auto remap = [&](double t) { return sat((pow(pow(A, g) + t * B, 1 / g) - A) / Ll); };
    // the normal map's texel; the up axis; the light at N.L = t (in N's plane with P)
    double n0[3] = {0.2, -0.1, 0.97}, up[3] = {0.1, 0.3, 0.9};
    norm(n0);
    norm(up);
    const float texel[4] = {float(n0[0] * 0.5 + 0.5), float(n0[1] * 0.5 + 0.5), float(n0[2] * 0.5 + 0.5), 1};
    double N[3] = {2.0 * texel[0] - 1, 2.0 * texel[1] - 1, 2.0 * texel[2] - 1};
    norm(N);
    double P[3] = {N[1], -N[0], 0};
    norm(P);
    const double ts[] = {-0.3, 0, 0.05, 0.2, 0.4, 0.6, 0.8, 0.95, 1};
    std::string line = "gamma self-test (off / on at N.L";
    for (double t : ts) {
        char b[16];
        snprintf(b, sizeof(b), " %g", t);
        line += b;
    }
    line += "):";
    for (int on = 0; on < 2; ++on) {
        const std::string src = rol::sg::light_source(rol::sg::code_text(lp.code), lp, {on != 0});
        IUnknown *code = nullptr, *errs = nullptr;
        bool made = !src.empty() && SUCCEEDED(assemble(src.c_str(), UINT(src.size()), nullptr, nullptr, 0, &code,
                                                       &errs)) && code;
        if (!made && errs) printf("    gamma %d: %.300s\n", on, buffer_text(errs).c_str());
        if (made && dev) {
            IDirect3DPixelShader9* ps = nullptr;
            made = SUCCEEDED(dev->CreatePixelShader(buffer_code(code), &ps));
            if (ps) ps->Release();
        }
        release(code);
        release(errs);
        double worst = 0;
        bool ran = true;
        std::string err;
        for (double t : ts) {
            const double s2 = sqrt(std::max(0.0, 1 - t * t));
            double L[3] = {t * N[0] + s2 * P[0], t * N[1] + s2 * P[1], t * N[2] + s2 * P[2]};
            emu::Machine m;
            m.flat["s1"] = emu::V4{{texel[0], texel[1], texel[2], texel[3]}};
            m.flat["s0"] = emu::V4{{float(albedo[0]), float(albedo[1]), float(albedo[2]), 1}};
            for (int lane = 0; lane < 4; ++lane) {
                m.reg[lane]["t0"] = emu::V4{{0.3f, 0.6f, 0, 0}};
                m.reg[lane]["t1"] = emu::V4{{float(L[0] * lraw), float(L[1] * lraw), float(L[2] * lraw), 0}};
                m.reg[lane]["t3"] = emu::V4{{float(up[0]), float(up[1]), float(up[2]), 0}};
                m.reg[lane]["c2"] = emu::V4{{float(ground[0]), float(ground[1]), float(ground[2]), 1}};
                m.reg[lane]["c3"] = emu::V4{{float(light[0]), float(light[1]), float(light[2]), 1}};
            }
            if (!m.run(split_lines(src))) {
                ran = false;
                err = m.error;
                break;
            }
            // the same directly
            const double nl = sat(dot(N, L)), nl_raw = sat(dot(N, L) * lraw), hemi = sat(dot(N, up));
            const double t1 = on ? remap(nl) : nl, t2 = on ? remap(nl_raw) : nl_raw;
            for (int k = 0; k < 3; ++k) {
                const double want = albedo[k] * (ground[k] + hemi * sky_diff[k] + t1 * light[k]);
                worst = std::max(worst, fabs(double(m.reg[0]["oC0"].v[k]) - want));
            }
            worst = std::max(worst, fabs(double(m.reg[0]["oC0"].v[3]) - t2));
        }
        const bool ok = made && ran && worst < 1e-4;
        if (!ok) ++bad;
        char cell[96];
        snprintf(cell, sizeof(cell), "  %s %s (worst %.1g)", on ? "on" : "off", ok ? "ok" : "FAIL", worst);
        line += cell;
        if (!ok)
            printf("    gamma %d: %s%s, emulated vs direct %.2g\n", on, made ? "" : "NOT ASSEMBLED ", ran ? "" : err.c_str(),
                   worst);
    }
    printf("%s\n", line.c_str());
    // the curve itself: full shade and full sun kept, brighter between, rising
    bool shape = fabs(remap(0)) < 1e-9 && fabs(remap(1) - 1) < 1e-9;
    double prev = -1;
    std::string curve;
    for (int i = 0; i <= 10; ++i) {
        const double t = i / 10.0, r = remap(t);
        shape &= r > prev && (i == 0 || i == 10 || r > t);
        prev = r;
        if (i % 2 == 0) {
            char b[24];
            snprintf(b, sizeof(b), " %.1f->%.3f", t, r);
            curve += b;
        }
    }
    printf("gamma self-test: ambient %.3f, sun %.3f (a + L = %.3f), N.L remapped%s - %s\n", A, Ll, A + Ll, curve.c_str(),
           shape ? "ok" : "FAIL");
    return bad + (shape ? 0 : 1);
}

// The interface scale's auto choice ([ui] layout_scale=0, uiauto.h) for the usual screen heights.
static int ui_auto_test() {
    const int want[][2] = {{720, 100}, {768, 100}, {900, 100}, {1050, 100}, {1080, 100}, {1200, 125}, {1440, 150},
                           {1600, 175}, {1800, 175}, {2160, 200}, {2880, 200}, {4320, 200}};
    int bad = 0;
    std::string line;
    for (const auto& w : want) {
        const int got = rol::ui_auto_scale(w[0]);
        char b[32];
        snprintf(b, sizeof(b), " %d->%d%s", w[0], got, got == w[1] ? "" : "!");
        line += b;
        bad += got != w[1];
    }
    printf("interface scale auto:%s - %s\n", line.c_str(), bad ? "FAIL" : "ok");
    return bad;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("shadertest <shaders dir> [map size, 8192] [dir for the failed copies' assembly]\n");
        return 2;
    }
    std::wstring dir = argv[1];
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    const int size = argc > 2 ? _wtoi(argv[2]) : 8192;
    std::wstring keep = argc > 3 ? argv[3] : L"";
    if (!keep.empty() && keep.back() != L'\\' && keep.back() != L'/') keep += L'\\';
    if (!keep.empty()) CreateDirectoryW(keep.c_str(), nullptr);

    HMODULE dx = LoadLibraryW(L"d3dx9_27.dll");
    auto assemble = dx ? reinterpret_cast<Assemble_t>(GetProcAddress(dx, "D3DXAssembleShader")) : nullptr;
    auto disasm = dx ? reinterpret_cast<Disasm_t>(GetProcAddress(dx, "D3DXDisassembleShader")) : nullptr;
    if (!assemble || !disasm) {
        printf("d3dx9_27.dll (x86) not found\n");
        return 2;
    }
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9* dev = nullptr;
    if (d3d) {
        D3DCAPS9 caps = {};
        d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
        printf("device: ps %lu.%lu, ps_2_x caps 0x%lx, %d temps, %d instruction slots, flow control %d/%d\n",
               (caps.PixelShaderVersion >> 8) & 0xFF, caps.PixelShaderVersion & 0xFF, caps.PS20Caps.Caps,
               caps.PS20Caps.NumTemps, caps.PS20Caps.NumInstructionSlots, caps.PS20Caps.StaticFlowControlDepth,
               caps.PS20Caps.DynamicFlowControlDepth);
        HWND w = CreateWindowExW(0, L"STATIC", L"shadertest", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
        D3DPRESENT_PARAMETERS pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_UNKNOWN;
        pp.BackBufferWidth = 64;
        pp.BackBufferHeight = 64;
        pp.hDeviceWindow = w;
        if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)))
            dev = nullptr;
    }
    if (!dev) printf("no D3D9 device: assembly checked, creation not\n");

    WIN32_FIND_DATAW fd;
    std::string terrain_text;   // the terrain's pass, for the self-test
    HANDLE h = FindFirstFileW((dir + L"*.ps.*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("no *.ps.*.txt in the folder\n");
        return 2;
    }
    do {
        const std::string text = read_all(dir + fd.cFileName);
        rol::sg::ShadowPlan pl;
        if (terrain_text.empty() && text.find("//   shadow_sampler ") != std::string::npos &&
            rol::sg::analyse_shadow(text, pl) && pl.terrain)
            terrain_text = text;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    const auto compile = reinterpret_cast<rol::ao::Compile_t>(GetProcAddress(dx, "D3DXCompileShader"));
    const int self_bad = selftest(assemble, dev, terrain_text) + gamma_selftest(assemble, dev) + ao_test(dev, compile) +
                         post_test(dev, compile) + ui_auto_test();
    int shaders = 0, skipped = 0, copies = 0, failed = self_bad, lit_shaders = 0, lit_copies = 0;
    // a copy assembled and created: its instruction slots, or why not
    auto try_copy = [&](const std::string& src, int& slots, std::string& err) {
        slots = 0;
        err.clear();
        IUnknown *code = nullptr, *errs = nullptr;
        if (src.empty()) {
            err = "no room";
        } else if (FAILED(assemble(src.c_str(), UINT(src.size()), nullptr, nullptr, 0, &code, &errs)) || !code) {
            err = errs ? buffer_text(errs) : std::string("assembly failed");
        } else {
            IUnknown* back = nullptr;
            if (SUCCEEDED(disasm(buffer_code(code), FALSE, nullptr, &back)) && back) {
                const std::string t = buffer_text(back);
                const size_t at = t.find("// approximately ");
                if (at != std::string::npos) slots = atoi(t.c_str() + at + 17);
                release(back);
            }
            IDirect3DPixelShader9* ps = nullptr;
            const HRESULT hr = dev ? dev->CreatePixelShader(buffer_code(code), &ps) : S_OK;
            if (FAILED(hr)) {
                char b[64];
                snprintf(b, sizeof(b), "the device refused it (0x%08lx)", (unsigned long)hr);
                err = b;
            }
            if (ps) ps->Release();
        }
        release(code);
        release(errs);
        return err.empty();
    };
    auto keep_src = [&](const std::string& src, const wchar_t* tag) {
        if (keep.empty() || src.empty()) return;
        wchar_t file[400];
        _snwprintf_s(file, _countof(file), _TRUNCATE, L"%s%s.%s.asm", keep.c_str(), fd.cFileName, tag);
        FILE* f = nullptr;
        if (!_wfopen_s(&f, file, L"wb") && f) {
            fwrite(src.data(), 1, src.size(), f);
            fclose(f);
        }
    };
    h = FindFirstFileW((dir + L"*.ps.*.txt").c_str(), &fd);
    do {
        const std::string text = read_all(dir + fd.cFileName);
        const bool shadowy = text.find("//   shadow_sampler ") != std::string::npos;
        // as shaderfx.cpp; the dump's file name starts with the effect's ("a+b" for rolhook's "a/b")
        const std::wstring file = fd.cFileName;
        std::string effect;
        for (wchar_t c : file.substr(0, file.find(L".ps."))) effect += c == L'+' ? '/' : char(c);
        const bool lit = text.find("//   light_diffuse ") != std::string::npos && rol::sg::gamma_effect(effect);
        if (!shadowy && !lit) continue;
        char name[260];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), nullptr, nullptr);
        rol::sg::ShadowPlan pl;
        std::string why;
        const bool has_shadow = shadowy && rol::sg::analyse_shadow(text, pl, &why);
        if (shadowy && !has_shadow) {
            printf("%s: the shadow test left as it is (%s)\n", name, why.c_str());
            ++skipped;
        }
        if (has_shadow) {
            ++shaders;
            if (pl.terrain)
                printf("%s: the terrain's shadow pass (known by its code's hash), r0-r%d\n", name, pl.max_r);
            else
                printf("%s: %s, tap at %d, eps %s, depth %s at %d, lit share to %s at %d, r0-r%d\n", name,
                       pl.code[0].c_str(), pl.tap, pl.eps.c_str(), pl.frag.c_str(), pl.frag_at, pl.cmp_dst.c_str(),
                       pl.cmp_at, pl.max_r);
            for (int plane = 0; plane < 2; ++plane) {
                std::string line = plane ? "  plane bias:" : "  plain:     ";
                for (int level = 1; level < 4; ++level) {
                    const int width = rol::sg::kSoftWidth[level];
                    ++copies;
                    char cell[64];
                    const std::string src = rol::sg::soft_shadow_source(pl, width, size, plane != 0);
                    int slots;
                    std::string err;
                    if (try_copy(src, slots, err)) {
                        snprintf(cell, sizeof(cell), "  W%d %d", width, slots);
                    } else {
                        ++failed;
                        snprintf(cell, sizeof(cell), "  W%d FAIL", width);
                        printf("    W%d%s: %.400s\n", width, plane ? " plane bias" : "", err.c_str());
                        keep_src(src, plane ? L"plane" : L"plain");
                    }
                    line += cell;
                }
                printf("%s\n", line.c_str());
            }
        }
        if (!lit) continue;
        rol::sg::LightPlan lp;
        std::string lwhy;
        if (!rol::sg::analyse_light(text, lp, &lwhy)) {
            printf("%s: the light left as it is (%s)\n", name, lwhy.c_str());
            continue;
        }
        ++lit_shaders;
        std::string targets;
        for (size_t k = 0; k < lp.targets.size(); ++k) targets += (k ? ", " : "") + lp.targets[k];
        printf("%s: gamma - N.L in %s (sky - ground %s, ground %s, light %s)\n", name, targets.c_str(),
               lp.sky_diff.c_str(), lp.ground.c_str(), lp.light.c_str());
        // the gamma over each soft-shadow level (0-3 when the shader has the test), the plane bias on as in rolhook
        int ok_n = 0, max_slots = 0, bad_n = 0;
        for (int soft = 0; soft < (has_shadow ? 4 : 1); ++soft) {
            ++lit_copies;
            const std::string base = soft ? rol::sg::soft_shadow_source(pl, rol::sg::kSoftWidth[soft], size, true)
                                          : rol::sg::code_text(lp.code);
            const std::string src = base.empty() ? base : rol::sg::light_source(base, lp, {true});
            int slots;
            std::string err;
            if (try_copy(src, slots, err)) {
                ++ok_n;
                max_slots = std::max(max_slots, slots);
            } else {
                ++bad_n;
                ++failed;
                printf("    soft %d gamma: %.400s\n", soft, err.c_str());
                wchar_t tag[32];
                _snwprintf_s(tag, _countof(tag), _TRUNCATE, L"s%dg1", soft);
                keep_src(src, tag);
            }
        }
        printf("  gamma copies: %d made, %d failed, at most %d instruction slots\n", ok_n, bad_n, max_slots);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    printf("%d shaders with the shadow test rewritten (%d left as they were): %d copies; %d with N.L for the gamma: %d copies; "
           "%d failed (map %d)\n",
           shaders, skipped, copies, lit_shaders, lit_copies, failed, size);
    if (dev) dev->Release();
    if (d3d) d3d->Release();
    return failed ? 1 : 0;
}
