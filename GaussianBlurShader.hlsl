Texture2D inputTexture : register(t0);
SamplerState inputSampler : register(s0);

cbuffer BlurSettings : register(b0)
{
    float2 texelSize;  // (1 / width, 1 / height)
    float blurRadius;
};

// Standard 1D Gaussian function
float Gaussian(float x, float sigma)
{
    return exp(-0.5 * (x / sigma) * (x / sigma)) / (sqrt(2.0 * 3.14159) * sigma);
}

// --------------------------------------
// Pass #1: Horizontal blur
// --------------------------------------
float4 PSHorizontalBlur(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET
{
    float4 color = float4(0, 0, 0, 0);
    float totalWeight = 0.0;
    float sigma = blurRadius * 0.5;

    for (int x = -int(blurRadius); x <= int(blurRadius); x++)
    {
        float2 offset = float2(x * texelSize.x, 0.0);
        float weight = Gaussian(abs(float(x)), sigma);
        color += inputTexture.Sample(inputSampler, uv + offset) * weight;
        totalWeight += weight;
    }

    return color / totalWeight;
}

// --------------------------------------
// Pass #2: Vertical blur
// --------------------------------------
float4 PSVerticalBlur(float4 pos : SV_POSITION, float2 uv : TEXCOORD) : SV_TARGET
{
    float4 color = float4(0, 0, 0, 0);
    float totalWeight = 0.0;
    float sigma = blurRadius * 0.5;

    for (int y = -int(blurRadius); y <= int(blurRadius); y++)
    {
        float2 offset = float2(0.0, y * texelSize.y);
        float weight = Gaussian(abs(float(y)), sigma);
        color += inputTexture.Sample(inputSampler, uv + offset) * weight;
        totalWeight += weight;
    }

    return color / totalWeight;
}
