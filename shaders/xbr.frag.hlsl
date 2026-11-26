// xBR-style edge smoothing at the output resolution (any size: 2x, 3x, 4x, fullscreen, widescreen).
// Own implementation of the xBR level-1 rule published by Hyllian (2011; his shaders are MIT, credited in
// third_party/README.md): for the corner of source pixel E that the output pixel lies in, compare the colour differences
// along the two diagonals of the 4x4 neighbourhood around that corner (with xBR's interpolation restriction); when
// an edge runs across the corner, the corner triangle takes the colour of the closer neighbour, with one output
// pixel of antialiasing on the edge.
#include "common.hlsli"

float3 src(int2 p)
{
    p = clamp(p, int2(0, 0), int2(src_size.xy) - 1);
    return src_tex.Load(int3(p, 0)).rgb;
}

float d(float3 a, float3 b)
{
    float3 k = abs(a - b);
    return dot(k, float3(0.299, 0.587, 0.114)) + 0.25 * (k.r + k.b); // luma weighted, some chroma
}

float4 main(PSIn i) : SV_Target0
{
    float2 t = i.uv * src_size.xy;
    int2 p = int2(floor(t));
    float2 f = frac(t) - 0.5;
    int2 dx = int2(f.x < 0 ? -1 : 1, 0), dy = int2(0, f.y < 0 ? -1 : 1); // towards the corner we are in
    float3 E = src(p), F = src(p + dx), H = src(p + dy), I = src(p + dx + dy);
    float3 B = src(p - dy), D = src(p - dx), C = src(p + dx - dy), G = src(p - dx + dy);
    float3 F4 = src(p + 2 * dx), I4 = src(p + 2 * dx + dy), H5 = src(p + 2 * dy), I5 = src(p + dx + 2 * dy);

    float wd1 = d(E, C) + d(E, G) + d(I, F4) + d(I, H5) + 4.0 * d(H, F); // edge across the corner (F-H line)
    float wd2 = d(H, D) + d(H, I5) + d(F, I4) + d(F, B) + 4.0 * d(E, I); // edge through it (E-I line)
    float3 c = E;
    // xBR's interpolation restriction: keep one-pixel lines and outlines unless the diagonal clearly continues
    bool guard = (d(F, B) > 0.0 && d(F, C) > 0.0) || (d(H, D) > 0.0 && d(H, G) > 0.0) ||
                 (d(E, I) == 0.0 && ((d(F, F4) > 0.0 && d(F, I4) > 0.0) || (d(H, H5) > 0.0 && d(H, I5) > 0.0))) ||
                 d(E, G) == 0.0 || d(E, C) == 0.0;
    if (wd1 < wd2 && d(E, F) > 0.0 && d(E, H) > 0.0 && guard) {
        float3 n = d(E, F) <= d(E, H) ? F : H;
        float2 u = abs(f);                                  // 0 at the pixel centre, 0.5 at the corner
        float w = 0.5 * max(src_size.x * out_size.z, src_size.y * out_size.w); // half an output pixel, in source px
        float a = smoothstep(0.5 - w, 0.5 + w, u.x + u.y);
        c = lerp(E, n, a);
    }
    return float4(c, 1.0);
}
