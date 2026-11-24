// A light CRT look, written for this port:
//  - horizontally: sharp-bilinear sampling, slightly softened (params2.x = softness 0..1);
//  - vertically: each NES line is a beam with a gaussian profile whose width grows with brightness (bright lines
//    bloom and fill the gap, dark lines show it), mixed from the two nearest lines (params.x = scanline strength);
//  - an aperture-grille mask over output pixel columns (params.y = mask strength);
//  - brightness compensation (params.z) and gamma: the math runs in linear light.
//  - params2.y = number of beam rows (0 = the texture's height; 240 when the input was upscaled).
// Needs a bilinear sampler.
#include "common.hlsli"

static const float GAMMA_IN = 2.4;
static const float GAMMA_OUT = 2.2;

float3 row_color(float x_texel, float row, float rows)
{
    float scale = out_size.x * src_size.z;
    float center = floor(x_texel) + 0.5;
    float d = clamp((x_texel - center) * scale * (1.0 - 0.75 * params2.x), -0.5, 0.5);
    float2 uv = float2((center + d) * src_size.z, (row + 0.5) / rows);
    return pow(abs(src_tex.Sample(src_smp, uv).rgb), GAMMA_IN);
}

float3 beam(float3 c, float dist)
{
    float3 width = lerp(0.28, 0.50, sqrt(c)); // wider beam for brighter colors
    float3 w = exp(-(dist * dist) / (2.0 * width * width));
    return lerp(1.0.xxx, w, params.x);
}

float4 main(PSIn i) : SV_Target0
{
    float rows = params2.y > 0.0 ? params2.y : src_size.y;
    float2 texel = i.uv * float2(src_size.x, rows);
    float y = texel.y - 0.5;
    float row0 = floor(y);
    float dy = y - row0;
    float3 c0 = row_color(texel.x, clamp(row0, 0.0, rows - 1.0), rows);
    float3 c1 = row_color(texel.x, clamp(row0 + 1.0, 0.0, rows - 1.0), rows);
    float3 c = c0 * beam(c0, dy) + c1 * beam(c1, 1.0 - dy);
    if (params.x <= 0.0)
        c = lerp(c0, c1, dy);

    // aperture grille: R, G, B stripes over 3 output pixels
    float m = fmod(floor(i.pos.x), 3.0);
    float3 mask = float3(m == 0.0, m == 1.0, m == 2.0);
    c *= lerp(1.0.xxx, lerp(0.55.xxx, 1.25.xxx, mask), params.y);

    c *= params.z;
    return float4(pow(saturate(c), 1.0 / GAMMA_OUT), 1.0);
}
