// Sharp bilinear: every source pixel becomes a solid block; only the one output pixel that straddles a
// block edge is blended, so non-integer sizes (and 4:3) stay crisp without uneven pixel widths.
// Needs a bilinear sampler.
#include "common.hlsli"

float4 main(PSIn i) : SV_Target0
{
    float2 scale = out_size.xy * src_size.zw;      // output pixels per source pixel
    float2 texel = i.uv * src_size.xy;
    float2 center = floor(texel) + 0.5;
    float2 d = clamp((texel - center) * scale, -0.5, 0.5);
    return src_tex.Sample(src_smp, (center + d) * src_size.zw);
}
