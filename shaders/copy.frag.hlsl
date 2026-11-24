// Plain sampling: nearest or bilinear depending on the sampler bound. params.x = opacity (overlay).
#include "common.hlsli"

float4 main(PSIn i) : SV_Target0
{
    float4 c = src_tex.Sample(src_smp, i.uv);
    c.a *= params.x;
    return c;
}
