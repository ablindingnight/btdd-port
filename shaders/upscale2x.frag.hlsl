// 2x pixel-art upscaler, written for this port from the EPX rule (Eric Johnston, 1992; the same rule is known as
// Scale2x / AdvMAME2x): each source pixel E becomes 2x2; a corner takes the color of the two neighbours that meet
// there when they are equal and the pixel is not part of a straight line. Edges get rounded, no new colors appear.
// Renders into a texture twice the source size; uses exact texel loads (no filtering).
#include "common.hlsli"

float3 px(int2 p)
{
    p = clamp(p, int2(0, 0), int2(src_size.xy) - 1);
    return src_tex.Load(int3(p, 0)).rgb;
}

bool same(float3 a, float3 b)
{
    return all(abs(a - b) < 1.0 / 512.0);
}

float4 main(PSIn i) : SV_Target0
{
    float2 t = i.uv * src_size.xy;
    int2 p = int2(floor(t));
    int2 q = int2(floor(frac(t) * 2.0)); // which of the 4 output pixels
    float3 E = px(p), B = px(p + int2(0, -1)), D = px(p + int2(-1, 0)), F = px(p + int2(1, 0)),
           H = px(p + int2(0, 1));
    float3 c = E;
    if (!same(B, H) && !same(D, F)) {
        if (q.x == 0 && q.y == 0 && same(D, B))
            c = D;
        else if (q.x == 1 && q.y == 0 && same(B, F))
            c = F;
        else if (q.x == 0 && q.y == 1 && same(D, H))
            c = D;
        else if (q.x == 1 && q.y == 1 && same(H, F))
            c = F;
    }
    return float4(c, 1.0);
}
