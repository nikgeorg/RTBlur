Texture2D inputTexture : register(t0);
SamplerState samplerState : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET{
    return inputTexture.Sample(samplerState, uv);
}