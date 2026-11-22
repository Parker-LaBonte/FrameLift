#pragma once
// HLSL compiled at startup with D3DCompile (d3dcompiler_47.dll ships with Windows 10/11).
//
// Pipeline per *unique* source frame (duplicates never reach the GPU):
//   Convert (NV12/YUY2/BGRA bytes -> RGBA) -> luma pyramid 1/2, 1/4, 1/8
//   Motion search, coarse to fine: 1/8 full search -> 1/4 refine -> 1/2 refine -> 1/2 half-pel -> median
// Per *output* frame: one full-screen pixel shader warps both neighbours along the motion
// field to the exact display time and blends, with occlusion and static-HUD fallbacks.

static const char kConvertCS[] = R"(
Texture2D<uint4> Packed : register(t0);   // YUY2 (Y0,U,Y1,V) or BGRA (B,G,R,A)
Texture2D<uint> Plane : register(t1);     // NV12: luma rows, then interleaved UV rows
RWTexture2D<unorm float4> OutColor : register(u0);
cbuffer C : register(b0) { uint W; uint H; uint Fmt; uint Matrix709; uint FullRange; uint Transfer; uint Bt2020; uint pad; };

// ---- HDR (P010) -> SDR ----
float3 PqToLinear(float3 e) {  // SMPTE ST 2084 EOTF, result in units of 10000 nits
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float3 HlgToLinear(float3 e) {  // ARIB STD-B67 inverse OETF (scene light 0..1)
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = saturate(e);
    return (e <= 0.5) ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
}
float SrgbEncode(float c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
// Non-linear R'G'B' (BT.2020) -> SDR sRGB/BT.709. Graphics white (203 nits, BT.2408) maps to SDR
// white; brighter highlights roll off smoothly instead of clipping. Luminance-preserving per pixel
// (scales by max channel) so bright colours keep their hue.
float3 HdrToSdr(float3 e) {
    float3 lin;
    if (Transfer == 2) lin = HlgToLinear(e) / 0.26497;  // HLG 75% = reference white
    else lin = PqToLinear(e) * (10000.0 / 203.0);
    if (Bt2020 != 0)
        lin = float3(dot(lin, float3(1.6605, -0.5876, -0.0728)), dot(lin, float3(-0.1246, 1.1329, -0.0083)),
                     dot(lin, float3(-0.0182, -0.1006, 1.1187)));
    lin = max(lin, 0.0);
    const float k = 0.8;  // linear below this; [k, inf) rolls off into [k, 1)
    float m = max(lin.r, max(lin.g, lin.b));
    if (m > k) lin *= (k + (1.0 - k) * (1.0 - exp(-(m - k) / (1.0 - k)))) / m;
    return float3(SrgbEncode(lin.r), SrgbEncode(lin.g), SrgbEncode(lin.b));
}
float3 Yuv10ToRgb(float y, float u, float v) {  // 10-bit codes
    if (FullRange == 0) { y = (y - 64.0) / 876.0; u = (u - 512.0) / 896.0; v = (v - 512.0) / 896.0; }
    else                { y = y / 1023.0;         u = (u - 512.0) / 1023.0; v = (v - 512.0) / 1023.0; }
    float3 rgb;
    if (Bt2020 != 0)        rgb = float3(y + 1.4746 * v, y - 0.16455 * u - 0.57135 * v, y + 1.8814 * u);
    else if (Matrix709 != 0) rgb = float3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u);
    else                     rgb = float3(y + 1.402 * v, y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);
    rgb = saturate(rgb);
    return Transfer != 0 ? HdrToSdr(rgb) : rgb;
}

float3 YuvToRgb(float y, float u, float v) {
    if (FullRange == 0) { y = (y - 16.0) / 219.0; u = (u - 128.0) / 224.0; v = (v - 128.0) / 224.0; }
    else                { y = y / 255.0;          u = (u - 128.0) / 255.0; v = (v - 128.0) / 255.0; }
    float3 rgb;
    if (Matrix709 != 0) rgb = float3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u);
    else                rgb = float3(y + 1.402 * v,  y - 0.344136 * u - 0.714136 * v, y + 1.772 * u);
    return saturate(rgb);
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float3 rgb;
    if (Fmt == 0) {          // NV12
        uint luma = Plane.Load(int3(id.xy, 0));
        int2 c = int2(id.x & ~1u, H + id.y / 2);
        uint cb = Plane.Load(int3(c, 0));
        uint cr = Plane.Load(int3(c.x + 1, c.y, 0));
        rgb = YuvToRgb(luma, cb, cr);
    } else if (Fmt == 1) {   // YUY2
        uint4 t = Packed.Load(int3(id.x / 2, id.y, 0));
        uint luma = (id.x & 1u) ? t.z : t.x;
        rgb = YuvToRgb(luma, t.y, t.w);
    } else if (Fmt == 3) {   // P010: NV12 layout, 16-bit samples with the 10 bits at the top
        uint luma = Plane.Load(int3(id.xy, 0)) >> 6;
        int2 c = int2(id.x & ~1u, H + id.y / 2);
        uint cb = Plane.Load(int3(c, 0)) >> 6;
        uint cr = Plane.Load(int3(c.x + 1, c.y, 0)) >> 6;
        rgb = Yuv10ToRgb(luma, cb, cr);
    } else {                 // BGRA
        uint4 t = Packed.Load(int3(id.xy, 0));
        rgb = float3(t.z, t.y, t.x) / 255.0;
    }
    OutColor[id.xy] = float4(rgb, 1);
}
)";

static const char kLumaHalfCS[] = R"(
Texture2D<float4> Src : register(t0);
RWTexture2D<float> Out : register(u0);
cbuffer C : register(b0) { uint SW; uint SH; uint DW; uint DH; };
float L(int2 p) { p = min(p, int2(SW - 1, SH - 1)); return dot(Src.Load(int3(p, 0)).rgb, float3(0.299, 0.587, 0.114)); }
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DW || id.y >= DH) return;
    int F = clamp((int)(SW / DW), 1, 8);  // 2 normally; 4 when a 4K capture is analysed at 1080p scale
    int2 p = int2(id.xy) * F;
    float acc = 0;
    for (int y = 0; y < F; y++)
        for (int x = 0; x < F; x++) acc += L(p + int2(x, y));
    Out[id.xy] = acc / (F * F);
}
)";

static const char kDownCS[] = R"(
Texture2D<float> Src : register(t0);
RWTexture2D<float> Out : register(u0);
cbuffer C : register(b0) { uint SW; uint SH; uint DW; uint DH; };
float L(int2 p) { p = min(p, int2(SW - 1, SH - 1)); return Src.Load(int3(p, 0)); }
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= DW || id.y >= DH) return;
    int F = clamp((int)(SW / DW), 1, 8);  // 2 normally; 4 when a 4K capture is analysed at 1080p scale
    int2 p = int2(id.xy) * F;
    float acc = 0;
    for (int y = 0; y < F; y++)
        for (int x = 0; x < F; x++) acc += L(p + int2(x, y));
    Out[id.xy] = acc / (F * F);
}
)";

// Local-pattern signal for matching: luma minus its local mean (+0.5). Flashes, exposure shifts and
// shadows move the mean, not the pattern, so matches stop being thrown off by lighting changes.
static const char kHighPassCS[] = R"(
Texture2D<float> In : register(t0);
RWTexture2D<float> Out : register(u0);
cbuffer C : register(b0) { uint W; uint H; int R; uint pad; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float s = 0;
    int n = 0;
    for (int y = -R; y <= R; y++)
        for (int x = -R; x <= R; x++) {
            int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(W, H) - 1);
            s += In.Load(int3(p, 0));
            n++;
        }
    Out[id.xy] = In.Load(int3(id.xy, 0)) - s / n + 0.5;
}
)";

// One thread group per 8x8 block; the 64 threads split the candidate vectors, then reduce.
// Matching is *bilateral*: a block at the midpoint is compared between frame A at -v/2 and
// frame B at +v/2, so every output pixel gets a vector (no holes, unlike forward warping).
static const char kSearchCS[] = R"(
Texture2D<float> L0 : register(t0);
Texture2D<float> L1 : register(t1);
Texture2D<float4> Pred : register(t2);
Texture2D<float4> Temporal : register(t3);
RWTexture2D<float4> Out : register(u0);
SamplerState Lin : register(s0);
cbuffer C : register(b0) {
    float2 LevelSize; float2 InvLevelSize;
    uint2 Grid; uint2 PredGrid;
    uint2 TempGrid; float Scale; float Step;
    int Radius; uint Mode; uint UseTemporal; float Lambda;
    int Local; float LocalPen; uint Asym; float TempSign;
};
#define WIN 12
groupshared float sCost[64];
groupshared float2 sVec[64];

float Cost(float2 c, float2 v) {
    // Symmetric (Asym=0): match at the midpoint, A at -v/2 and B at +v/2.
    // One-sided (Asym=1): the block sits in L0 and moves by v into L1 (forward/backward fields).
    float2 h0 = Asym != 0 ? float2(0, 0) : v * 0.5;
    float2 h1 = Asym != 0 ? v : v * 0.5;
    float acc = 0;
    [loop] for (int y = 0; y < WIN; y++) {
        [unroll] for (int x = 0; x < WIN; x++) {
            float2 q = c + float2(x - WIN * 0.5 + 0.5, y - WIN * 0.5 + 0.5);
            acc += abs(L0.SampleLevel(Lin, (q - h0) * InvLevelSize, 0) - L1.SampleLevel(Lin, (q + h1) * InvLevelSize, 0));
        }
    }
    return acc * (255.0 / (WIN * WIN));
}

float2 LoadPred(int2 p) {
    p = clamp(p, int2(0, 0), int2(PredGrid) - 1);
    return Pred.Load(int3(p, 0)).xy / Scale;
}

[numthreads(64, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    int2 b = int2(gid.xy);
    float2 center = (float2(b) + 0.5) * 8.0;
    float2 tpred = float2(0, 0);
    if (UseTemporal != 0) {
        int2 tp = int2((float2(b) + 0.5) / float2(Grid) * float2(TempGrid));
        tp = clamp(tp, int2(0, 0), int2(TempGrid) - 1);
        tpred = Temporal.Load(int3(tp, 0)).xy * TempSign / Scale;
    }
    float best = 1e30;
    float2 bestV = float2(0, 0);
    if (Mode == 0) {
        int side = 2 * Radius + 1;
        int full = side * side;
        int total = full + (UseTemporal != 0 ? 9 : 0);
        for (int k = int(gi); k < total; k += 64) {
            float2 v;
            if (k < full) v = float2(k % side - Radius, k / side - Radius) * Step;
            else { int j = k - full; v = round(tpred) + float2(j % 3 - 1, j / 3 - 1); }
            float c = Cost(center, v) + Lambda * 0.25 * (abs(v.x) + abs(v.y));
            if (c < best) { best = c; bestV = v; }
        }
    } else {
        int2 pb = (Mode == 1) ? b / 2 : b;
        float2 preds[7];
        preds[0] = LoadPred(pb);
        preds[1] = LoadPred(pb + int2(-1, 0));
        preds[2] = LoadPred(pb + int2(1, 0));
        preds[3] = LoadPred(pb + int2(0, -1));
        preds[4] = LoadPred(pb + int2(0, 1));
        preds[5] = tpred;
        preds[6] = float2(0, 0);
        // 63 candidates around the predictors, plus (Local > 0) every motion within +-Local of
        // zero. The second set rescues small objects (a camera-followed character's head) whose
        // motion differs from the background that dominates the coarse levels; it's penalised by
        // its own size, not by distance from the (background) neighbours.
        int side = 2 * Local + 1;
        int total = 63 + (Local > 0 ? side * side : 0);
        for (int k = int(gi); k < total; k += 64) {
            float2 v;
            float pen;
            if (k < 63) {
                int pi = k / 9;
                int j = k % 9;
                v = preds[pi] + float2(j % 3 - 1, j / 3 - 1) * Step;
                pen = Lambda * (abs(v.x - preds[0].x) + abs(v.y - preds[0].y));
            } else {
                int q = k - 63;
                v = float2(q % side - Local, q / side - Local) * Step;
                pen = Lambda * LocalPen * (abs(v.x) + abs(v.y));  // penalised by its own size
            }
            float c = Cost(center, v) + pen;
            if (c < best) { best = c; bestV = v; }
        }
    }
    sCost[gi] = best;
    sVec[gi] = bestV;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32; s > 0; s >>= 1) {
        if (gi < s && sCost[gi + s] < sCost[gi]) { sCost[gi] = sCost[gi + s]; sVec[gi] = sVec[gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) Out[b] = float4(sVec[0] * Scale, sCost[0], 0);
}
)";

static const char kMedianCS[] = R"(
Texture2D<float4> In : register(t0);
RWTexture2D<float4> Out : register(u0);
cbuffer C : register(b0) { uint2 Grid; uint2 pad; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Grid.x || id.y >= Grid.y) return;
    float xs[9]; float ys[9];
    [unroll] for (int k = 0; k < 9; k++) {
        int2 p = clamp(int2(id.xy) + int2(k % 3 - 1, k / 3 - 1), int2(0, 0), int2(Grid) - 1);
        float4 v = In.Load(int3(p, 0));
        xs[k] = v.x; ys[k] = v.y;
    }
    [unroll] for (int i = 0; i < 9; i++) {
        [unroll] for (int j = 0; j < 8 - i; j++) {
            float a = xs[j], b = xs[j + 1]; xs[j] = min(a, b); xs[j + 1] = max(a, b);
            float c = ys[j], d = ys[j + 1]; ys[j] = min(c, d); ys[j + 1] = max(c, d);
        }
    }
    Out[id.xy] = float4(xs[4], ys[4], In.Load(int3(id.xy, 0)).z, 0);
}
)";

// Per-pixel vector choice at 1/2 resolution. Block vectors are blended between neighbouring
// blocks, which is wrong right at object edges (car vs. road) and causes halos. Here each pixel
// tries the 4 surrounding block vectors, their blend, and zero, and keeps whichever actually
// matches best over a 5x5 patch. Output: one vector per 2x2 source pixels.
static const char kRefineCS[] = R"(
Texture2D<float> L0 : register(t0);
Texture2D<float> L1 : register(t1);
Texture2D<float4> Blocks : register(t2);
RWTexture2D<float4> Out : register(u0);
SamplerState Lin : register(s0);
cbuffer C : register(b0) { float2 InvLevelSize; uint2 Size; uint2 Grid; float Bias; float ZeroBias; };

float Cost(float2 q, float2 v) {
    float2 h = v * 0.5;
    float acc = 0;
    [unroll] for (int y = -2; y <= 2; y++) {
        [unroll] for (int x = -2; x <= 2; x++) {
            float2 s = q + float2(x, y);
            acc += abs(L0.SampleLevel(Lin, (s - h) * InvLevelSize, 0) - L1.SampleLevel(Lin, (s + h) * InvLevelSize, 0));
        }
    }
    return acc * (255.0 / 25.0);
}

float2 Blk(int2 b) { return Blocks.Load(int3(clamp(b, int2(0, 0), int2(Grid) - 1), 0)).xy * 0.5; }  // full px -> 1/2 px

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Size.x || id.y >= Size.y) return;
    float2 q = float2(id.xy) + 0.5;
    float2 fb = q / 8.0 - 0.5;
    int2 b0 = int2(floor(fb));
    float2 w = fb - float2(b0);
    float2 v00 = Blk(b0), v10 = Blk(b0 + int2(1, 0)), v01 = Blk(b0 + int2(0, 1)), v11 = Blk(b0 + int2(1, 1));
    float2 c[6];
    c[0] = lerp(lerp(v00, v10, w.x), lerp(v01, v11, w.x), w.y);
    c[1] = v00; c[2] = v10; c[3] = v01; c[4] = v11;
    c[5] = float2(0, 0);
    float best = 1e30;
    float2 bv = c[0];
    [unroll] for (int k = 0; k < 6; k++) {
        // Small bias toward the smooth blend so flat areas don't flicker between candidates.
        float cst = Cost(q, c[k]) + (k == 0 ? 0.0 : (k == 5 ? ZeroBias : Bias));
        if (cst < best) { best = cst; bv = c[k]; }
    }
    Out[id.xy] = float4(bv * 2.0, best, 0);
}
)";

// Static guard: a pixel that is (near) identical in both real frames, together with its 3x3
// neighbourhood, is static (menu text, HUD, minimap). It must not be moved by motion picked up
// from an animated background around it.
static const char kStaticMaskCS[] = R"(
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
RWTexture2D<unorm float> Out : register(u0);
cbuffer C : register(b0) { uint W; uint H; float Thr; uint pad; };
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float m = 0;
    [unroll] for (int y = -1; y <= 1; y++)
        [unroll] for (int x = -1; x <= 1; x++) {
            int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(W, H) - 1);
            float3 d = abs(A.Load(int3(p, 0)).rgb - B.Load(int3(p, 0)).rgb);
            m = max(m, max(d.r, max(d.g, d.b)));
        }
    Out[id.xy] = m <= Thr ? 1.0 : 0.0;
}
)";

// --- NVIDIA Optical Flow engine glue ---------------------------------------------------------
// Full-res 8-bit luma for the hardware engine (read by CUDA through D3D11 interop).
static const char kLuma8CS[] = R"(
Texture2D<float4> Src : register(t0);
RWTexture2D<unorm float> Out : register(u0);
cbuffer C : register(b0) { uint W; uint H; uint S; uint pad; };  // output size, downscale factor (1 or 2)
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= W || id.y >= H) return;
    float l = 0;
    for (uint y = 0; y < S; y++)
        for (uint x = 0; x < S; x++) l += dot(Src.Load(int3(id.xy * S + uint2(x, y), 0)).rgb, float3(0.299, 0.587, 0.114));
    Out[id.xy] = l / (S * S);
}
)";

// Hardware flow -> per-pixel midpoint vectors at 1/2 res. The engine gives A->B measured from A
// and B->A measured from B (4x4 grid, S10.5). For a pixel of the in-between frame, follow each to
// the midpoint by fixed-point iteration, then keep whichever candidate lines A and B up best.
static const char kNvToDenseCS[] = R"(
Texture2D<int2> NF : register(t0);
Texture2D<int2> NB : register(t1);
Texture2D<float> L0 : register(t2);
Texture2D<float> L1 : register(t3);
Texture2D<float4> Prior : register(t4);
RWTexture2D<float4> Out : register(u0);
SamplerState Lin : register(s0);
cbuffer C : register(b0) { uint2 Size; uint2 FlowSize; float2 InvL2; uint Hybrid; float Bias; uint Bilin; float FlowScale; uint2 pad; };

// Flow lookup at a full-res position: nearest cell, or bilinear between cell centres. The engine may
// run on a downscaled image (FlowScale 2): cells are then 8x8 full-res px and vectors scale by 2.
float2 Look(Texture2D<int2> T, float2 p) {
    float cell = 4.0 * FlowScale;
    if (Bilin == 0) {
        int2 c = clamp(int2(p / cell), int2(0, 0), int2(FlowSize) - 1);
        return float2(T.Load(int3(c, 0))) / 32.0 * FlowScale;
    }
    float2 g = p / cell - 0.5;
    int2 c0 = int2(floor(g));
    float2 w = g - float2(c0);
    int2 mx = int2(FlowSize) - 1;
    float2 v00 = float2(T.Load(int3(clamp(c0, int2(0, 0), mx), 0)));
    float2 v10 = float2(T.Load(int3(clamp(c0 + int2(1, 0), int2(0, 0), mx), 0)));
    float2 v01 = float2(T.Load(int3(clamp(c0 + int2(0, 1), int2(0, 0), mx), 0)));
    float2 v11 = float2(T.Load(int3(clamp(c0 + int2(1, 1), int2(0, 0), mx), 0)));
    return lerp(lerp(v00, v10, w.x), lerp(v01, v11, w.x), w.y) / 32.0 * FlowScale;
}
float2 Fw(float2 p) { return Look(NF, p); }
float2 Bw(float2 p) { return Look(NB, p); }

float Cost(float2 q, float2 v) {  // q: 1/2-res px; v: full-res px
    float2 h = v * 0.25;
    float acc = 0;
    [unroll] for (int y = -2; y <= 2; y++) {
        [unroll] for (int x = -2; x <= 2; x++) {
            float2 s = q + float2(x, y);
            acc += abs(L0.SampleLevel(Lin, (s - h) * InvL2, 0) - L1.SampleLevel(Lin, (s + h) * InvL2, 0));
        }
    }
    return acc * (255.0 / 25.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Size.x || id.y >= Size.y) return;
    float2 q = float2(id.xy) + 0.5;
    float2 p = q * 2.0;
    float2 vf = Fw(p);
    [unroll] for (int i = 0; i < 3; i++) vf = Fw(p - 0.5 * vf);
    float2 vb = -Bw(p);
    [unroll] for (int j = 0; j < 3; j++) vb = -Bw(p + 0.5 * vb);
    float2 c[5];
    c[0] = vf; c[1] = vb; c[2] = 0.5 * (vf + vb); c[3] = float2(0, 0);
    c[4] = Hybrid != 0 ? Prior.Load(int3(id.xy, 0)).xy : vf;
    float best = 1e30;
    float2 bv = vf;
    [unroll] for (int k = 0; k < 5; k++) {
        float cst = Cost(q, c[k]) + (k == 0 ? 0.0 : Bias);
        if (cst < best) { best = cst; bv = c[k]; }
    }
    Out[id.xy] = float4(bv, best, 0);
}
)";

// Hardware flow -> the coarse (1/16) fields the rest of the pipeline uses: midpoint field (motion
// hints, legacy view), plus forward/backward fields for the occlusion-aware blend.
static const char kNvToGridCS[] = R"(
Texture2D<int2> NF : register(t0);
Texture2D<int2> NB : register(t1);
RWTexture2D<float4> OutField : register(u0);
RWTexture2D<float4> OutFwd : register(u1);
RWTexture2D<float4> OutBwd : register(u2);
cbuffer C : register(b0) { uint2 Grid; uint2 FlowSize; float FlowScale; uint3 pad; };
float2 Fw(float2 p) { int2 c = clamp(int2(p / (4.0 * FlowScale)), int2(0, 0), int2(FlowSize) - 1); return float2(NF.Load(int3(c, 0))) / 32.0 * FlowScale; }
float2 Bw(float2 p) { int2 c = clamp(int2(p / (4.0 * FlowScale)), int2(0, 0), int2(FlowSize) - 1); return float2(NB.Load(int3(c, 0))) / 32.0 * FlowScale; }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Grid.x || id.y >= Grid.y) return;
    float2 p = (float2(id.xy) + 0.5) * 16.0;
    float2 vf = Fw(p);
    [unroll] for (int i = 0; i < 3; i++) vf = Fw(p - 0.5 * vf);
    OutField[id.xy] = float4(vf, 0, 0);
    OutFwd[id.xy] = float4(Fw(p), 0, 0);
    OutBwd[id.xy] = float4(Bw(p), 0, 0);
}
)";

static const char kPresentHLSL[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

Texture2D<float4> C0 : register(t0);
Texture2D<float4> C1 : register(t1);
Texture2D<float4> Field : register(t2);   // block vectors (legacy path, Mode 3)
Texture2D<float4> Dense : register(t3);   // per-pixel vectors at 1/2 res (Mode 1)
Texture2D<float4> Fwd : register(t4);     // A->B motion measured from A (1/2 grid)
Texture2D<float4> Bwd : register(t5);     // B->A motion measured from B (1/2 grid)
Texture2D<float> Still : register(t6);    // static guard mask (full res), 1 = identical in both frames
SamplerState Lin : register(s0);
cbuffer P : register(b0) {
    float2 SrcSize; float2 FieldUVScale; float T; uint Mode; uint ShowMotion; uint Occl;
    float OccTol; float OccSpan; float MotionUnit; float pad;
};

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PSMain(VSOut i) : SV_Target {
    if (Mode == 0) return float4(C1.SampleLevel(Lin, i.uv, 0).rgb, 1);
    if (Mode == 4) return float4(lerp(C0.SampleLevel(Lin, i.uv, 0).rgb, C1.SampleLevel(Lin, i.uv, 0).rgb, T), 1);  // no-motion baseline
    if (Mode == 2) {  // scene cut: never blend two unrelated pictures
        if (T < 0.5) return float4(C0.SampleLevel(Lin, i.uv, 0).rgb, 1);
        return float4(C1.SampleLevel(Lin, i.uv, 0).rgb, 1);
    }
    float4 f;
    if (Mode == 3) {
        f = Field.SampleLevel(Lin, i.uv * FieldUVScale, 0);
    } else {
        uint dw, dh;
        Dense.GetDimensions(dw, dh);
        int2 dp = min(int2(i.uv * float2(dw, dh)), int2(dw, dh) - 1);
        f = Dense.Load(int3(dp, 0));  // nearest: keeps object edges sharp
    }
    float2 v = f.xy * MotionUnit / SrcSize;  // full motion A->B in uv units
    float3 a = C0.SampleLevel(Lin, i.uv - T * v, 0).rgb;
    float3 b = C1.SampleLevel(Lin, i.uv + (1 - T) * v, 0).rgb;
    // Plain motion-compensated blend. Tested against real in-between frames (tools/fgsim.py):
    // the earlier per-pixel switches (static-HUD fallback, snap-to-nearer-frame on mismatch)
    // made neighbouring pixels flip and tore fine detail; removing both scored +2.1 dB overall,
    // +1.0 dB on moving pixels and +2.5 dB on the minimap.
    if (Mode == 5) return float4(a, 1);  // bench oracle: A's warped sample only
    if (Mode == 6) return float4(b, 1);  // bench oracle: B's warped sample only
    float3 result = lerp(a, b, T);
    if (Occl != 0 && Mode == 1) {
        // Occlusion: does the spot we fetch from in A really move the way this pixel does (A's own
        // forward motion), and likewise in B (B's own backward motion)? If one side disagrees, that
        // sample is on another object (the character in front, or background hidden in that frame),
        // so take this pixel from the other frame only.
        float2 pa = i.uv - T * v, pb = i.uv + (1 - T) * v;
        float2 F = Fwd.SampleLevel(Lin, pa * FieldUVScale, 0).xy;
        float2 G = Bwd.SampleLevel(Lin, pb * FieldUVScale, 0).xy;
        float mag = length(f.xy);
        float tol = OccTol + 0.1 * mag, span = OccSpan + 0.1 * mag;
        float ca = saturate(1 - (length(F - f.xy) - tol) / span);
        float cb = saturate(1 - (length(G + f.xy) - tol) / span);
        float wa = (1 - T) * ca + 1e-3, wb = T * cb + 1e-3;
        result = (a * wa + b * wb) / (wa + wb);
    }

    if (Mode == 1 && Still.SampleLevel(Lin, i.uv, 0) > 0.5) {
        result = lerp(C0.SampleLevel(Lin, i.uv, 0).rgb, C1.SampleLevel(Lin, i.uv, 0).rgb, T);  // static: show as-is
    }
    if (ShowMotion != 0) {
        float2 m = f.xy / 48.0;
        result = lerp(result, float3(0.5 + m.x, 0.5 + m.y, 0.5 - 0.5 * (m.x + m.y)), 0.6);
    }
    return float4(result, 1);
}

// Final scale-up of a frame generated at source resolution to the output size.
float4 PSUpscale(VSOut i) : SV_Target { return float4(C0.SampleLevel(Lin, i.uv, 0).rgb, 1); }
)";

// Output scaling (capture size -> screen size). Pass 1: Lanczos-2 resample (kernel widened when
// shrinking, so 4K onto a 1080p screen doesn't alias), clamped to the nearest 2x2 source texels
// when enlarging so edges don't ring. Pass 2: contrast-adaptive sharpening (the RCAS approach
// from AMD FidelityFX FSR 1, MIT): sharpens less where it would clip, so no halos.
static const char kScaleHLSL[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
Texture2D<float4> Src : register(t0);
cbuffer S : register(b0) { float2 SrcSize; float2 DstSize; float2 DstOrigin; float Sharp; float Ratio; };

float L2(float x) {
    x = abs(x);
    if (x >= 2.0) return 0.0;
    if (x < 1e-4) return 1.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}

float4 PSLanczos(VSOut i) : SV_Target {
    float2 p = (i.pos.xy - DstOrigin) * SrcSize / DstSize - 0.5;
    float2 fb = floor(p);
    float2 f = p - fb;
    int2 b = int2(fb);
    float r = max(Ratio, 1.0);
    int R = min((int)ceil(2.0 * r), 8);
    int2 mx = int2(SrcSize) - 1;
    float3 acc = 0;
    float wsum = 0;
    [loop] for (int y = 1 - R; y <= R; y++) {
        float wy = L2((y - f.y) / r);
        [loop] for (int x = 1 - R; x <= R; x++) {
            float w = wy * L2((x - f.x) / r);
            acc += w * Src.Load(int3(clamp(b + int2(x, y), 0, mx), 0)).rgb;
            wsum += w;
        }
    }
    float3 col = acc / max(wsum, 1e-4);
    if (Ratio <= 1.0) {
        int2 c0 = clamp(b, 0, mx), c1 = clamp(b + 1, 0, mx);
        float3 t0 = Src.Load(int3(c0, 0)).rgb, t1 = Src.Load(int3(c1.x, c0.y, 0)).rgb;
        float3 t2 = Src.Load(int3(c0.x, c1.y, 0)).rgb, t3 = Src.Load(int3(c1, 0)).rgb;
        col = clamp(col, min(min(t0, t1), min(t2, t3)), max(max(t0, t1), max(t2, t3)));
    }
    return float4(col, 1);
}

float4 PSRcas(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy - DstOrigin);
    int2 mx = int2(DstSize) - 1;
    float3 e = Src.Load(int3(clamp(p, 0, mx), 0)).rgb;
    if (Sharp <= 0.0) return float4(e, 1);
    float3 b = Src.Load(int3(clamp(p + int2(0, -1), 0, mx), 0)).rgb;
    float3 d = Src.Load(int3(clamp(p + int2(-1, 0), 0, mx), 0)).rgb;
    float3 f = Src.Load(int3(clamp(p + int2(1, 0), 0, mx), 0)).rgb;
    float3 h = Src.Load(int3(clamp(p + int2(0, 1), 0, mx), 0)).rgb;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0 - 1e-5);
    float3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * Sharp;
    float3 col = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
    return float4(saturate(col), 1);
}
)";
