// FullScreenPass.hlsl
cbuffer BlurParams : register(b0)
{
    float2 texelSize;
    float blurRadius;
};

// Vertex shader input
struct VSInput
{
    float2 pos : POSITION;   // clip-space or NDC coordinates
    float2 uv : TEXCOORD0;  // texture coords
};

struct PSInput
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    // We assume pos is already in [-1..1] range for a fullscreen triangle
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

// We'll sample a single texture + sampler
Texture2D    g_inputTexture : register(t0);
SamplerState g_inputSampler : register(s0);

// Simple pass-through pixel shader
float4 PSMain(PSInput input) : SV_TARGET
{
    // For now, no blur loop. Just sample the input for demonstration.
    float4 color = g_inputTexture.Sample(g_inputSampler, input.uv);
    return color;
}
