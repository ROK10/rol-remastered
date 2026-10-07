// UI scale: draws pretransformed (POSITIONT / XYZRHW) vertices given in virtual-screen pixels onto the real
// backbuffer, x_real = (x + 0.5) * s - 0.5 (the engine already puts its -0.5 texel offset into x).
// rolhook swaps the declaration's POSITIONT for POSITION and draws through this shader; the fixed-function pixel
// pipeline stays as the engine set it. Variants (gen_uiscale_vs.py): COL0, COL1 = 0/1, TEX = 0..4 texcoord sets.
//   c250 = (2s/W, -2s/H, s/W - 1, 1 - s/H)

float4 g_xf : register(c250);

struct VSIn {
    float4 pos : POSITION;   // x, y, z, rhw
#if COL0
    float4 c0 : COLOR0;
#endif
#if COL1
    float4 c1 : COLOR1;
#endif
#if TEX >= 1
    float4 t0 : TEXCOORD0;
#endif
#if TEX >= 2
    float4 t1 : TEXCOORD1;
#endif
#if TEX >= 3
    float4 t2 : TEXCOORD2;
#endif
#if TEX >= 4
    float4 t3 : TEXCOORD3;
#endif
};

struct VSOut {
    float4 pos : POSITION;
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
#if TEX >= 1
    float4 t0 : TEXCOORD0;
#endif
#if TEX >= 2
    float4 t1 : TEXCOORD1;
#endif
#if TEX >= 3
    float4 t2 : TEXCOORD2;
#endif
#if TEX >= 4
    float4 t3 : TEXCOORD3;
#endif
    float fog : FOG;
};

VSOut main(VSIn i) {
    VSOut o;
    float w = 1.0 / i.pos.w;   // rhw -> w keeps perspective-correct interpolation for rhw != 1
    o.pos = float4((i.pos.xy * g_xf.xy + g_xf.zw) * w, i.pos.z * w, w);
#if COL0
    o.c0 = i.c0;
#else
    o.c0 = 1;   // fixed function: no diffuse in the vertex = opaque white
#endif
#if COL1
    o.c1 = i.c1;
#else
    o.c1 = 0;
#endif
#if TEX >= 1
    o.t0 = i.t0;
#endif
#if TEX >= 2
    o.t1 = i.t1;
#endif
#if TEX >= 3
    o.t2 = i.t2;
#endif
#if TEX >= 4
    o.t3 = i.t3;
#endif
    o.fog = 1;
    return o;
}
