// shadergen.cpp — the soft-shadow rewrite and the gamma-correct light (shaderfx.cpp puts them in the game;
// shadertest.exe checks them offline).
//
// The effects' pixel shaders that receive the shadow test the fragment against the shadow map once:
//    texld rS, rC, sN          the map's depth (R32F)
//    add   rA, rS.x, cE.x      + shadowmap_epsilon
//    add   rA, rA, -rF         - the fragment's depth in the light
//    cmp   rL, rA, c1, c0      lit (1) or not (0)
// analyse_shadow finds those four instructions by their data flow (write masks and swizzles followed from rS.x).
//
// soft_shadow_source keeps rC before the tap and, where the fragment's depth is known, makes the same test over the
// (W+1) x (W+1) texels under a box W texels wide centred on rC, the box's edge rows and columns weighted by how far it
// reaches into them: with a = rC * size - W/2 and f = frac(a), texels n + 0 .. n + W (n = a - f) weigh 1 - f, 1 ... 1, f
// along each axis. That is the mean of W x W bilinear lookups a texel apart: the lit share changes smoothly as the
// fragment moves across texels, with no steps, and every tap lands on a texel centre, whatever the map's filter. The
// share goes to the cmp's register in its place; the rest of the shader is left as it was.
//
// A wide box reaches texels whose surface lies nearer or farther from the light than the fragment on a slope, and a lit
// slope would shade itself. With plane_bias each tap is compared against the receiving surface's depth under it: dz
// per texel along u and v from the screen derivatives (dsx/dsy) of (u, v, z) through the inverse of the 2x2 Jacobian,
// clamped to +-4 x shadowmap_epsilon per texel (a 2x2 pixel quad across an edge gives garbage), added up tap by tap.
//
// The terrain gets its shadow from a pass of its own (terrain_shadow_pass_0/2, one shader, known by its code's hash):
// a bilinear quad of 4 taps a texel apart (shrink_shadowmap_size = (N/4, 4/N, 1/N) from 0x0071C8C9), its corner r8 =
// uv - 2/N and its weights frac(r8 N): bilinear at uv - 1.5 texels, compared with no epsilon, its shadow scaled down by
// 4 diagonal taps near the edge. The terrain is drawn into the map too, so a wider box around that centre would reach
// texels toward the light and shade flat ground: the box replaces the quad and the diagonal factor, centred where the
// quad was (so buildings' shadows stay where the game puts them; shadertest checks a 1-texel box against the game's
// quad), compared against the plane under each tap with kTerrainEps.
//
// D3D9 rules kept: one constant register per instruction (X5213), swizzles none or replicate only (ps_2_0's set), and
// ps_2_x out for the instruction count (taps x 5 + ~30); the effects' 2_0 vertex shaders pair with it.
#include "shadergen.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace rol {
namespace sg {
namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> split_args(const std::string& rest) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= rest.size()) {
        const size_t c = rest.find(',', p);
        out.push_back(trim(rest.substr(p, c == std::string::npos ? std::string::npos : c - p)));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    return out;
}

struct Src {
    bool neg = false;
    std::string reg, sw;   // "r3", "w"
};
struct Op {
    std::string base;         // the opcode without _pp / _sat
    std::string dreg, dmask;  // the destination, its write mask ("xyzw" when none)
    std::vector<Src> srcs;
};

void split_reg(std::string a, std::string& reg, std::string& sw) {
    const size_t abs = a.find("_abs");
    if (abs != std::string::npos) a.erase(abs, 4);
    const size_t dot = a.find('.');
    reg = a.substr(0, dot);
    sw = dot == std::string::npos ? std::string() : a.substr(dot + 1);
}

bool parse_op(const std::string& line, Op& op) {
    std::string l = line;
    if (!l.empty() && l[0] == '+') l = trim(l.substr(1));
    const size_t sp = l.find(' ');
    if (sp == std::string::npos) return false;
    const std::string opc = l.substr(0, sp);
    op.base = opc.substr(0, opc.find('_'));
    const std::vector<std::string> args = split_args(l.substr(sp + 1));
    if (args.empty()) return false;
    split_reg(args[0], op.dreg, op.dmask);
    if (op.dmask.empty()) op.dmask = "xyzw";
    op.srcs.clear();
    for (size_t i = 1; i < args.size(); ++i) {
        Src s;
        std::string a = args[i];
        s.neg = !a.empty() && a[0] == '-';
        if (s.neg) a.erase(0, 1);
        split_reg(a, s.reg, s.sw);
        op.srcs.push_back(s);
    }
    return true;
}

char comp(const std::string& sw, int k) {   // the source component read for output component k
    if (sw.empty()) return "xyzw"[k];
    if (sw.size() == 1) return sw[0];
    return k < int(sw.size()) ? sw[k] : sw.back();
}

int index_of(char c) { return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : 3; }

bool is_reg(const std::string& r, char kind) {
    return r.size() > 1 && r[0] == kind && isdigit(static_cast<unsigned char>(r[1]));
}

// The one component of s read for output component c: "c2.x".
std::string at_comp(const Src& s, char c) { return s.reg + "." + comp(s.sw, index_of(c)); }

std::string fmt(const char* f, ...) {
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof(b), f, a);
    va_end(a);
    return b;
}

}  // namespace

bool analyse_shadow(const std::string& text, ShadowPlan& pl, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    // the shadow sampler's register from the constant table: "//   shadow_sampler   s0   1"
    const size_t at = text.find("//   shadow_sampler ");
    if (at == std::string::npos) return fail("no shadow_sampler in the constant table");
    {
        const size_t eol = text.find('\n', at);
        const std::string row = text.substr(at + 20, eol == std::string::npos ? std::string::npos : eol - at - 20);
        const size_t s = row.find('s');
        if (s == std::string::npos || s + 1 >= row.size() || !isdigit(static_cast<unsigned char>(row[s + 1])))
            return fail("the shadow sampler's register not read");
        size_t e = s + 1;
        while (e < row.size() && isdigit(static_cast<unsigned char>(row[e]))) ++e;
        pl.sampler = row.substr(s, e - s);
    }
    std::vector<std::string> lines;
    size_t p = 0;
    while (p < text.size()) {
        const size_t e = text.find('\n', p);
        const std::string l = trim(text.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (!l.empty() && l.compare(0, 2, "//") != 0) lines.push_back(l);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    int start = -1;
    for (int i = 0; i < int(lines.size()); ++i)
        if (lines[i].compare(0, 3, "ps_") == 0) start = i;
    if (start < 0) return fail("no pixel shader version line");
    pl.code.assign(lines.begin() + start, lines.end());
    if (pl.code[0] != "ps_2_0" && pl.code[0] != "ps_2_x") return fail("not ps_2_0 / ps_2_x");
    unsigned long long hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < pl.code.size(); ++i)
        for (const char ch : (i ? "\n" : "") + pl.code[i]) hash = (hash ^ static_cast<unsigned char>(ch)) * 0x100000001b3ull;
    pl.terrain = hash == kTerrainHash && int(pl.code.size()) == kTerrainLines;
    // the registers in use, the def'd constants
    std::unordered_map<std::string, std::vector<float>> defs;
    for (const std::string& l : pl.code) {
        for (size_t i = 0; i + 1 < l.size(); ++i) {
            const char k = l[i];
            if ((k == 'r' || k == 'c') && isdigit(static_cast<unsigned char>(l[i + 1])) &&
                (i == 0 || !isalnum(static_cast<unsigned char>(l[i - 1])))) {
                const int n = atoi(l.c_str() + i + 1);
                if (k == 'r' && n > pl.max_r) pl.max_r = n;
                if (k == 'c' && n < 32) pl.used_c[n] = true;
            }
        }
        if (l.compare(0, 4, "def ") == 0) {
            const std::vector<std::string> a = split_args(l.substr(4));
            if (a.size() == 5) {
                std::vector<float> v;
                for (int i = 1; i < 5; ++i) v.push_back(float(atof(a[i].c_str())));
                defs[a[0]] = v;
            }
        }
    }
    if (pl.terrain) return pl.max_r + kSoftTemps <= 31 || fail("no free temporary registers");
    // the single tap
    int taps = 0;
    for (int i = 0; i < int(pl.code.size()); ++i) {
        Op op;
        if (parse_op(pl.code[i], op) && op.base == "texld" && op.srcs.size() == 2 && op.srcs[1].reg == pl.sampler) {
            ++taps;
            pl.tap = i;
        }
    }
    if (taps != 1) return fail(taps ? "more than one shadow tap" : "no shadow tap");
    Op tap;
    parse_op(pl.code[pl.tap], tap);
    {
        const std::string& l = pl.code[pl.tap];
        const std::vector<std::string> a = split_args(l.substr(l.find(' ') + 1));
        pl.coord = a.size() == 3 ? a[1] : std::string();
        if (pl.coord.empty() || pl.coord[0] == '-' || pl.coord.find('.') != std::string::npos)
            return fail("the tap's coordinates are not a plain register");
    }
    // the data flow from the depth (rS.x)
    std::vector<std::pair<std::string, char>> tainted = {{tap.dreg, 'x'}};
    auto is_t = [&](const std::string& r, char c) {
        for (const auto& t : tainted)
            if (t.first == r && t.second == c) return true;
        return false;
    };
    auto plain_t = [&](const Src& s, char c) { return !s.neg && is_t(s.reg, comp(s.sw, index_of(c))); };
    int stage = 0;
    for (int i = pl.tap + 1; i < int(pl.code.size()) && stage < 3; ++i) {
        Op op;
        if (!parse_op(pl.code[i], op)) continue;
        const std::string& b = op.base;
        const bool scalar = b == "rcp" || b == "rsq" || b == "log" || b == "exp" || b == "pow" || b == "dp2add" ||
                            b == "dp3" || b == "dp4" || b == "nrm" || b == "texld" || b == "texldp" || b == "texldb" ||
                            b == "crs";
        const int width = (b == "dp3" || b == "nrm" || b == "crs") ? 3 : 4;
        std::string out_t;
        for (int k = 0; k < 4; ++k) {
            const char c = "xyzw"[k];
            if (op.dmask.find(c) == std::string::npos) continue;
            bool hit = false;
            for (const Src& s : op.srcs) {
                if (scalar)
                    for (int j = 0; j < width && !hit; ++j) hit = is_t(s.reg, comp(s.sw, j));
                else
                    hit = hit || is_t(s.reg, comp(s.sw, k));
            }
            if (hit) out_t += c;
        }
        if (!out_t.empty()) {
            const char c = out_t[0];
            if (stage == 0 && b == "add" && op.srcs.size() == 2 && plain_t(op.srcs[0], c) &&
                is_reg(op.srcs[1].reg, 'c') && !op.srcs[1].neg) {
                pl.eps = at_comp(op.srcs[1], c);
                stage = 1;
            } else if (stage == 1 && b == "add" && op.srcs.size() == 2 && plain_t(op.srcs[0], c) && op.srcs[1].neg &&
                       is_reg(op.srcs[1].reg, 'r')) {
                pl.frag = at_comp(op.srcs[1], c);
                pl.frag_at = i;
                stage = 2;
            } else if (stage == 2 && b == "cmp" && op.srcs.size() == 3) {
                for (char d : op.dmask)
                    if (!plain_t(op.srcs[0], d)) return fail("the depth reaches a cmp other than as its condition");
                auto value = [&](const Src& s, float& v) {
                    auto d = defs.find(s.reg);
                    if (d == defs.end() || d->second.size() < 4) return false;
                    v = d->second[index_of(comp(s.sw, 0))];
                    if (s.neg) v = -v;
                    return true;
                };
                float lit = -1, dark = -1;
                if (!value(op.srcs[1], lit) || !value(op.srcs[2], dark) || lit != 1.0f || dark != 0.0f)
                    return fail("the cmp does not pick def'd 1 (lit) / 0 (dark)");
                pl.cmp_dst = op.dreg + (op.dmask == "xyzw" ? "" : "." + op.dmask);
                pl.cmp_at = i;
                stage = 3;
            }
        }
        for (int k = 0; k < 4; ++k) {   // the destination's components: tainted now or not
            const char c = "xyzw"[k];
            if (op.dmask.find(c) == std::string::npos) continue;
            const bool now = out_t.find(c) != std::string::npos;
            bool was = false;
            for (auto it = tainted.begin(); it != tainted.end(); ++it)
                if (it->first == op.dreg && it->second == c) {
                    was = true;
                    if (!now) tainted.erase(it);
                    break;
                }
            if (now && !was) tainted.push_back({op.dreg, c});
        }
    }
    if (stage < 3)
        return fail(stage == 0   ? "no + epsilon after the tap"
                    : stage == 1 ? "no - fragment depth after the epsilon"
                                 : "no lit / dark cmp after the depth");
    if (pl.max_r + kSoftTemps > 31) return fail("no free temporary registers");
    return true;
}

namespace {

// The terrain's box sits where the engine's quad sat: 1.5 texels off the fragment, toward -u -v.
constexpr double kTerrainShift = -1.5;

// The box's registers above the shader's own and constants in free registers from c31 down.
struct Box {
    char cS[8], cK[8], cX[8], cY[8], cH[8];
    char RU[8], RF[8], RP[8], RC[8], RT[8], RA[8], RE[8];
    int w = 0, size = 0;
    bool plane = false;
    double shift = 0;   // the box's centre - the fragment, texels (the plane is laid through the fragment)
};

bool box_alloc(Box& b, const ShadowPlan& pl, int w, int size, bool plane) {
    if (w < 1 || size < 1 || pl.code.empty() || pl.max_r + kSoftTemps > 31) return false;
    int cf[5], n = 0;
    for (int c = 31; c >= 0 && n < 5; --c)
        if (!pl.used_c[c]) cf[n++] = c;
    if (n < 5) return false;
    snprintf(b.cS, sizeof(b.cS), "c%d", cf[0]);   // (size, -W/2, 1/size, 0.5/size)
    snprintf(b.cK, sizeof(b.cK), "c%d", cf[1]);   // (1, 0, 1/(W W), 0)
    snprintf(b.cX, sizeof(b.cX), "c%d", cf[2]);   // (1/size, 0, 0, 0): the next column
    snprintf(b.cY, sizeof(b.cY), "c%d", cf[3]);   // (0, 1/size, 0, 0): the next row
    snprintf(b.cH, sizeof(b.cH), "c%d", cf[4]);   // ((W-1)/2 - shift, 4, kTerrainEps, 0): plane bias, the terrain's eps
    snprintf(b.RU, sizeof(b.RU), "r%d", pl.max_r + 1);   // .xy the box's centre (the tap's coordinates)
    snprintf(b.RF, sizeof(b.RF), "r%d", pl.max_r + 2);   // .xy f: the edge columns' and rows' weights
    snprintf(b.RP, sizeof(b.RP), "r%d", pl.max_r + 3);   // .xy the row's first texel
    snprintf(b.RC, sizeof(b.RC), "r%d", pl.max_r + 4);   // .xy the texel
    snprintf(b.RT, sizeof(b.RT), "r%d", pl.max_r + 5);   // the tap: depth, then lit 1 / dark 0
    snprintf(b.RA, sizeof(b.RA), "r%d", pl.max_r + 6);   // .x row sum .y its first tap .z total .w the first row's sum
    snprintf(b.RE, sizeof(b.RE), "r%d", pl.max_r + 7);   // .x eps - z for the tap .y for the row's first; .zw dz/texel
    b.w = w;
    b.size = size;
    b.plane = plane;
    return true;
}

std::string box_defs(const Box& b) {
    const double texel = 1.0 / b.size;
    return fmt("def %s, %.9g, %.9g, %.9g, %.9g\n", b.cS, double(b.size), -b.w / 2.0, texel, 0.5 * texel) +
           fmt("def %s, 1, 0, %.9g, 0\n", b.cK, 1.0 / (b.w * b.w)) + fmt("def %s, %.9g, 0, 0, 0\n", b.cX, texel) +
           fmt("def %s, 0, %.9g, 0, 0\n", b.cY, texel) + fmt("def %s, %.9g, 4, %.9g, 0\n", b.cH, (b.w - 1) / 2.0 - b.shift, kTerrainEps);
}

// The box around RU.xy, each tap compared like the one tap (depth + eps - z >= 0): the lit share in RA.z.
std::string box_code(const Box& b, const std::string& sampler, const char* eps, const char* frag) {
    const char *cS = b.cS, *cK = b.cK, *cX = b.cX, *cY = b.cY, *cH = b.cH;
    const char *RU = b.RU, *RF = b.RF, *RP = b.RP, *RC = b.RC, *RT = b.RT, *RA = b.RA, *RE = b.RE;
    const int w = b.w;
    std::string out;
    out += fmt("mad %s.xy, %s, %s.x, %s.y\n", RP, RU, cS, cS);   // a = uv size - W/2
    out += fmt("frc %s.xy, %s\n", RF, RP);
    out += fmt("add %s.xy, %s, -%s\n", RP, RP, RF);             // n
    out += fmt("mad %s.xy, %s, %s.z, %s.w\n", RP, RP, cS, cS);   // texel n's centre
    out += fmt("mov %s.x, %s\n", RE, eps);
    out += fmt("add %s.x, %s.x, -%s\n", RE, RE, frag);          // eps - z
    if (b.plane) {
        out += fmt("mul %s.xy, %s, %s.x\n", RT, RU, cS);   // (u, v) in texels, z
        out += fmt("mov %s.z, %s\n", RT, frag);
        out += fmt("dsx %s.xyz, %s\n", RC, RT);
        out += fmt("dsy %s.xyz, %s\n", RT, RT);
        out += fmt("mul %s.x, %s.x, %s.y\n", RA, RC, RT);
        out += fmt("mad %s.x, %s.x, -%s.y, %s.x\n", RA, RT, RC, RA);   // det = ux vy - uy vx
        out += fmt("mul %s.z, %s.z, %s.y\n", RA, RC, RT);
        out += fmt("mad %s.z, %s.z, -%s.y, %s.z\n", RA, RT, RC, RA);   // det dz/du = zx vy - zy vx
        out += fmt("mul %s.w, %s.z, %s.x\n", RA, RT, RC);
        out += fmt("mad %s.w, %s.z, -%s.x, %s.w\n", RA, RC, RT, RA);   // det dz/dv = zy ux - zx uy
        out += fmt("rcp %s.y, %s.x\n", RA, RA);
        out += fmt("mul %s.zw, %s, %s.y\n", RE, RA, RA);   // dz per texel (NaN / inf end at the clamp)
        out += fmt("mov %s.x, %s\n", RA, eps);
        out += fmt("mul %s.x, %s.x, %s.y\n", RA, RA, cH);
        out += fmt("min %s.zw, %s, %s.x\n", RE, RE, RA);
        out += fmt("max %s.zw, %s, -%s.x\n", RE, RE, RA);
        out += fmt("add %s.xy, %s, %s.x\n", RT, RF, cH);   // the fragment's distance from texel n: f + (W-1)/2 - shift
        out += fmt("mad %s.x, %s.z, %s.x, %s.x\n", RE, RE, RT, RE);
        out += fmt("mad %s.x, %s.w, %s.y, %s.x\n", RE, RE, RT, RE);   // eps - the surface's z at texel n
        out += fmt("mov %s.y, %s.x\n", RE, RE);
    }
    for (int y = 0; y <= w; ++y) {
        if (y) {
            out += fmt("add %s.xy, %s, %s\n", RP, RP, cY);
            if (b.plane) out += fmt("add %s.y, %s.y, -%s.w\nmov %s.x, %s.y\n", RE, RE, RE, RE, RE);
        }
        out += fmt("mov %s.xy, %s\n", RC, RP);
        for (int x = 0; x <= w; ++x) {
            if (x) {
                out += fmt("add %s.xy, %s, %s\n", RC, RC, cX);
                if (b.plane) out += fmt("add %s.x, %s.x, -%s.z\n", RE, RE, RE);
            }
            out += fmt("texld %s, %s, %s\n", RT, RC, sampler.c_str());
            out += fmt("add %s.x, %s.x, %s.x\n", RT, RT, RE);
            out += fmt("cmp %s.x, %s.x, %s.x, %s.y\n", RT, RT, cK, cK);   // lit 1, dark 0
            if (x == 0)
                out += fmt("mov %s.xy, %s.x\n", RA, RT);
            else if (x < w)
                out += fmt("add %s.x, %s.x, %s.x\n", RA, RA, RT);
            else   // + f (last - first): the edge columns weighed 1 - f and f
                out += fmt("add %s.x, %s.x, -%s.y\nmad %s.x, %s.x, %s.x, %s.x\n", RT, RT, RA, RA, RT, RF, RA);
        }
        if (y == 0)
            out += fmt("mov %s.zw, %s.x\n", RA, RA);
        else if (y < w)
            out += fmt("add %s.z, %s.z, %s.x\n", RA, RA, RA);
        else
            out += fmt("add %s.x, %s.x, -%s.w\nmad %s.z, %s.x, %s.y, %s.z\n", RA, RA, RA, RA, RA, RF, RA);
    }
    return out;
}

// The terrain's pass, kept: version and defs (0-2), declarations (3-9), the map coordinates (10) and the engine's
// quad's corner r8.yz = uv - 2/N (11), the fog of war's tap (29), the fragment's depth r5.w (30), and from line 72 the
// shadow's share in r1.w scaled by shadow_percent, masked by the face's N.L (t2.x), coloured by shadow_color and
// the fog. Dropped: the 7 taps' coordinates and taps (12-28), the quad and the diagonal factor (31-71).
std::string terrain_source(const ShadowPlan& pl, const Box& b) {
    std::string out = "ps_2_x\n";
    for (int i = 1; i <= 2; ++i) out += pl.code[i] + "\n";
    out += box_defs(b);
    for (int i = 3; i <= 11; ++i) out += pl.code[i] + "\n";
    out += fmt("add %s.xy, r8.yzxw, %s.w\n", b.RU, b.cS);   // the quad's centre, uv - 1.5 texels: the box sits there
    out += pl.code[29] + "\n" + pl.code[30] + "\n";
    out += box_code(b, pl.sampler, fmt("%s.z", b.cH).c_str(), "r5.w");
    out += fmt("mad r1.w, -%s.z, %s.z, %s.x\n", b.RA, b.cK, b.cK);   // the shadow's share: 1 - the lit one
    for (int i = 72; i < int(pl.code.size()); ++i) out += pl.code[i] + "\n";
    return out;
}

}  // namespace

std::string soft_shadow_source(const ShadowPlan& pl, int w, int size, bool plane) {
    Box b;
    if (!box_alloc(b, pl, w, size, plane)) return {};
    if (pl.terrain) {
        b.shift = kTerrainShift;
        return terrain_source(pl, b);
    }
    std::string out = "ps_2_x\n";
    bool defs_done = false;
    for (int i = 1; i < int(pl.code.size()); ++i) {
        const std::string& l = pl.code[i];
        if (!defs_done && l.compare(0, 4, "def ") != 0) {
            out += box_defs(b);
            defs_done = true;
        }
        if (i == pl.tap) out += fmt("mov %s.xy, %s\n", b.RU, pl.coord.c_str());   // the tap may write over them
        if (i == pl.frag_at) out += box_code(b, pl.sampler, pl.eps.c_str(), pl.frag.c_str());
        if (i == pl.cmp_at) {   // the lit share in the cmp's place
            out += fmt("mul %s, %s.z, %s.z\n", pl.cmp_dst.c_str(), b.RA, b.cK);
            continue;
        }
        out += l + "\n";
    }
    return out;
}

// ---- Gamma-correct light on the buildings and units.
//
// Their pixel shaders (dot3shadow_2, metal_and_dot3shadow_2, unitdot3teamcolor_2, dot3teamcolorshadow_2, clockwork_2,
// flamingunit_2, timonium_2, dot3reflect_2, dot3metalglass_2...) light in tangent space: N.L from the normal map and t1
// (the vector to the sun), times the sun's colour light_diffuse, plus a hemispheric ambient between the ground and the
// sky colour (the preshader hands sky - ground: mov rX, cK ... mad ..., rX, c<ground>) - summed as stored, in gamma
// space, then multiplied by the texture. Summed in linear space and encoded for the screen, a surface turned away from
// the sun keeps more of its light. With the ambient a and the sun L (their luminances), out = albedo x (a^g + t B)^(1/g)
// - the texture cancels exactly, (albedo^g X)^(1/g) = albedo X^(1/g) - where B = (a + L)^g - a^g keeps full sun and
// full shade as the game has them (its a + L is over 1: straight linear sums would come out much darker). So every
// saturated N.L (t) is remapped to t' = ((a^g + t B)^(1/g) - a) / L, g = 2.2: equal at 0 and 1, brighter between - the
// terminator's falloff, which is what gamma-correct lighting changes; a and L come from the shader's own constants per
// pixel (the ambient's midpoint between ground and sky, light_diffuse), so each map's light is kept.
namespace {

std::vector<std::string> disasm_code(const std::string& text) {
    std::vector<std::string> lines;
    size_t p = 0;
    while (p < text.size()) {
        const size_t e = text.find('\n', p);
        const std::string l = trim(text.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (!l.empty() && l.compare(0, 2, "//") != 0) lines.push_back(l);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    int start = -1;
    for (int i = 0; i < int(lines.size()); ++i)
        if (lines[i].compare(0, 3, "ps_") == 0) start = i;
    if (start < 0) return {};
    return std::vector<std::string>(lines.begin() + start, lines.end());
}

void scan_regs(const std::vector<std::string>& code, int& max_r, bool used_c[32]) {
    max_r = 0;
    for (int i = 0; i < 32; ++i) used_c[i] = false;
    for (const std::string& l : code)
        for (size_t i = 0; i + 1 < l.size(); ++i) {
            const char k = l[i];
            if ((k == 'r' || k == 'c') && isdigit(static_cast<unsigned char>(l[i + 1])) &&
                (i == 0 || !isalnum(static_cast<unsigned char>(l[i - 1])))) {
                const int n = atoi(l.c_str() + i + 1);
                if (k == 'r' && n > max_r) max_r = n;
                if (k == 'c' && n < 32) used_c[n] = true;
            }
        }
}

// The register a name has in the last constant table of the text (the main shader's; the preshader's comes first).
std::string ctab_reg(const std::string& text, const char* name) {
    const std::string key = std::string("//   ") + name + " ";
    std::string reg;
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
        const size_t eol = text.find('\n', at);
        const std::string row = text.substr(at + key.size(), eol == std::string::npos ? std::string::npos : eol - at - key.size());
        size_t s = row.find_first_not_of(' ');
        if (s == std::string::npos || (row[s] != 'c' && row[s] != 's')) continue;
        size_t e = s + 1;
        while (e < row.size() && isdigit(static_cast<unsigned char>(row[e]))) ++e;
        if (e > s + 1) reg = row.substr(s, e - s);
    }
    return reg;
}

bool declares_vector(const std::vector<std::string>& code, const char* t) {
    for (const std::string& l : code) {
        if (l.compare(0, 3, "dcl") != 0) continue;
        const size_t sp = l.find(' ');
        if (sp == std::string::npos) continue;
        std::string reg, mask;
        split_reg(trim(l.substr(sp + 1)), reg, mask);
        if (reg == t && (mask.empty() || mask.find("xyz") == 0)) return true;
    }
    return false;
}

}  // namespace

std::string code_text(const std::vector<std::string>& code) {
    std::string s;
    for (const std::string& l : code) s += l + "\n";
    return s;
}

bool gamma_effect(const std::string& effect) {
    for (size_t p = 0; p <= effect.size();) {
        const size_t e = effect.find('/', p);
        std::string n = effect.substr(p, e == std::string::npos ? std::string::npos : e - p);
        for (char& c : n) c = char(tolower(static_cast<unsigned char>(c)));
        if (n.compare(0, 7, "terrain") == 0 || n.compare(0, 6, "splats") == 0) return false;
        if (e == std::string::npos) break;
        p = e + 1;
    }
    return true;
}

bool analyse_light(const std::string& text, LightPlan& lp, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    lp = LightPlan();
    const std::string ground = ctab_reg(text, "hemispheric_ground_color");
    const std::string light = ctab_reg(text, "light_diffuse");
    if (ground.empty() || light.empty()) return fail("no hemispheric_ground_color / light_diffuse");
    lp.code = disasm_code(text);
    if (lp.code.empty() || (lp.code[0] != "ps_2_0" && lp.code[0] != "ps_2_x")) return fail("not ps_2_0 / ps_2_x");
    if (!declares_vector(lp.code, "t1")) return fail("no light vector (t1)");
    lp.ground = ground;
    lp.light = light;
    std::vector<Op> ops(lp.code.size());
    std::vector<bool> ok(lp.code.size());
    for (size_t i = 0; i < lp.code.size(); ++i) ok[i] = parse_op(lp.code[i], ops[i]);
    // the preshader's sky - ground: mov rX, cK, then within 3 lines mad ..., rX, c<ground>
    for (size_t i = 0; i < ops.size() && lp.sky_diff.empty(); ++i) {
        if (!ok[i] || ops[i].base != "mov" || ops[i].srcs.size() != 1 || !is_reg(ops[i].srcs[0].reg, 'c')) continue;
        for (size_t j = i + 1; j < ops.size() && j <= i + 3; ++j)
            if (ok[j] && ops[j].base == "mad" && ops[j].srcs.size() == 3 && ops[j].srcs[1].reg == ops[i].dreg &&
                ops[j].srcs[2].reg == ground)
                lp.sky_diff = ops[i].srcs[0].reg;
    }
    if (lp.sky_diff.empty()) return fail("no hemispheric ambient (sky - ground)");
    // N.L: a one-component dp3 with the light vector (t1, or a register's xyz nrm made of it and not written over
    // since - a write to .w leaves it), saturated there or by the next mov_sat of it
    std::unordered_map<std::string, std::string> lv;   // register -> its components that hold the light vector
    auto in_lv = [&](const std::string& r) {
        if (r == "t1") return true;
        auto it = lv.find(r);
        return it != lv.end() && it->second.find('x') != std::string::npos && it->second.find('y') != std::string::npos &&
               it->second.find('z') != std::string::npos;
    };
    for (size_t i = 0; i < ops.size(); ++i) {
        if (!ok[i]) continue;
        const Op& op = ops[i];
        const bool from_l = op.base == "nrm" && op.srcs.size() == 1 && in_lv(op.srcs[0].reg);
        if (op.base == "dp3" && op.dmask.size() == 1 && op.srcs.size() == 2 &&
            (in_lv(op.srcs[0].reg) || in_lv(op.srcs[1].reg))) {
            const std::string opc = lp.code[i].substr(0, lp.code[i].find(' '));
            if (opc.find("_sat") != std::string::npos) {
                lp.anchors.push_back(lp.code[i]);
                lp.targets.push_back(op.dreg + "." + op.dmask);
            } else {
                for (size_t j = i + 1; j < ops.size(); ++j) {
                    if (!ok[j]) continue;
                    const std::string oj = lp.code[j].substr(0, lp.code[j].find(' '));
                    if (ops[j].base == "mov" && oj.find("_sat") != std::string::npos && ops[j].srcs.size() == 1 &&
                        ops[j].srcs[0].reg == op.dreg && comp(ops[j].srcs[0].sw, 0) == op.dmask[0] &&
                        ops[j].dmask.size() == 1 && !ops[j].srcs[0].neg) {
                        lp.anchors.push_back(lp.code[j]);
                        lp.targets.push_back(ops[j].dreg + "." + ops[j].dmask);
                        break;
                    }
                    // the dot product written over before any mov_sat: no saturated N.L from it
                    if (ops[j].dreg == op.dreg && ops[j].dmask.find(op.dmask[0]) != std::string::npos) break;
                }
            }
        }
        // the light vector's registers: nrm of it puts it in the components written, anything else takes them out
        if (from_l) {
            lv[op.dreg] = op.dmask;
        } else {
            auto it = lv.find(op.dreg);
            if (it != lv.end())
                for (char c : op.dmask) {
                    const size_t at = it->second.find(c);
                    if (at != std::string::npos) it->second.erase(at, 1);
                }
        }
    }
    if (lp.anchors.empty()) return fail("no saturated N.L");
    lp.gamma = true;
    return true;
}

std::string light_source(const std::string& src, const LightPlan& lp, const LightOptions& o) {
    if (!o.gamma || !lp.gamma) return src;
    std::vector<std::string> lines;
    for (size_t p = 0; p < src.size();) {
        const size_t e = src.find('\n', p);
        const std::string l = trim(src.substr(p, e == std::string::npos ? std::string::npos : e - p));
        if (!l.empty()) lines.push_back(l);
        if (e == std::string::npos) break;
        p = e + 1;
    }
    if (lines.empty() || lines[0].compare(0, 3, "ps_") != 0) return {};
    int max_r = 0;
    bool used_c[32];
    scan_regs(lines, max_r, used_c);
    if (max_r + kLightTemps > 31) return {};
    int cf[2], n = 0;
    for (int c = 31; c >= 0 && n < 2; --c)
        if (!used_c[c]) cf[n++] = c;
    if (n < 2) return {};
    char cW[8], cV[8], RK[8], RT[8];
    snprintf(cW, sizeof(cW), "c%d", cf[0]);   // (luminance weights, 0.5)
    snprintf(cV, sizeof(cV), "c%d", cf[1]);   // (0.01, g, 1/g, 0)
    snprintf(RK, sizeof(RK), "r%d", max_r + 1);   // (a^g, B, a, 1/L)
    snprintf(RT, sizeof(RT), "r%d", max_r + 2);   // scratch
    int last_def = -1, last_dcl = -1;
    std::vector<int> at(lp.anchors.size(), -1);
    for (int i = 0; i < int(lines.size()); ++i) {
        if (lines[i].compare(0, 4, "def ") == 0) last_def = i;
        if (lines[i].compare(0, 3, "dcl") == 0) last_dcl = i;
        for (size_t k = 0; k < lp.anchors.size(); ++k)
            if (lines[i] == lp.anchors[k]) {
                if (at[k] >= 0) return {};   // ambiguous
                at[k] = i;
            }
    }
    if (last_dcl < 0) return {};
    for (int a : at)
        if (a < 0) return {};
    const std::string defs = fmt("def %s, 0.299, 0.587, 0.114, 0.5\n", cW) +
                             fmt("def %s, 0.01, %.9g, %.9g, 0\n", cV, kGamma, 1.0 / kGamma);
    std::string out = "ps_2_x\n";
    if (last_def < 0) out += defs;
    for (int i = 1; i < int(lines.size()); ++i) {
        out += lines[i] + "\n";
        if (i == last_def) out += defs;
        if (i == last_dcl) {   // a, L and B from the shader's constants
            out += fmt("mov %s.xyz, %s\n", RT, lp.sky_diff.c_str());
            out += fmt("mul %s.xyz, %s, %s.w\n", RT, RT, cW);
            out += fmt("add %s.xyz, %s, %s\n", RT, RT, lp.ground.c_str());   // the ambient's midpoint
            out += fmt("dp3 %s.z, %s, %s\n", RK, RT, cW);                     // a
            out += fmt("mov %s.xyz, %s\n", RT, lp.light.c_str());
            out += fmt("dp3 %s.w, %s, %s\n", RT, RT, cW);                     // L
            out += fmt("max %s.w, %s.w, %s.x\n", RT, RT, cV);
            out += fmt("rcp %s.w, %s.w\n", RK, RT);
            out += fmt("pow %s.x, %s.z, %s.y\n", RK, RK, cV);                 // a^g
            out += fmt("add %s.z, %s.z, %s.w\n", RT, RK, RT);
            out += fmt("pow %s.z, %s.z, %s.y\n", RT, RT, cV);                 // (a + L)^g
            out += fmt("add %s.y, %s.z, -%s.x\n", RK, RT, RK);                // B
        }
        for (size_t k = 0; k < at.size(); ++k)
            if (i == at[k]) {   // t' = ((a^g + t B)^(1/g) - a) / L
                const char* t = lp.targets[k].c_str();
                out += fmt("mad %s.x, %s, %s.y, %s.x\n", RT, t, RK, RK);
                out += fmt("pow %s.x, %s.x, %s.z\n", RT, RT, cV);
                out += fmt("add %s.x, %s.x, -%s.z\n", RT, RT, RK);
                out += fmt("mul_sat %s, %s.x, %s.w\n", t, RT, RK);
            }
    }
    return out;
}

}  // namespace sg
}  // namespace rol
