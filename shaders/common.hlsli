// Shared declarations for btdd_play's GPU shaders (our own code, MIT like the rest of the port).
// SDL3 GPU / D3D12 binding rules: fragment textures and samplers in space2, uniform buffers in space3.
Texture2D<float4> src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer Params : register(b0, space3)
{
    float4 src_size; // width, height, 1/width, 1/height of the input texture
    float4 out_size; // width, height, 1/width, 1/height of the output viewport in pixels
    float4 params;   // effect parameters (see each shader)
    float4 params2;
};

struct PSIn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0; // 0..1 over the viewport
};
