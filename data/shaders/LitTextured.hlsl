cbuffer CameraConstants : register(b0)
{
    // Explicit row_major - see Triangle.hlsl for why (removes any dependence on HLSL's
    // default cbuffer packing, which is easy to get backwards from memory).
    row_major float4x4 ViewProjection;
};

cbuffer ObjectConstants : register(b1)
{
    row_major float4x4 World;
};

Texture2D BaseColorTexture : register(t0);
SamplerState BaseColorSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float2 uv0 : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPosition = mul(World, float4(input.position, 1.0f));
    output.position = mul(ViewProjection, worldPosition);
    // Correct for rotation + uniform scale; would need the inverse-transpose of World for
    // non-uniform scale, which nothing built with this shader so far actually uses.
    output.worldNormal = mul((float3x3)World, input.normal);
    output.uv0 = input.uv0;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColor = BaseColorTexture.Sample(BaseColorSampler, input.uv0);

    // Not full PBR - one hardcoded directional light plus a flat ambient term, read directly
    // here rather than through a general lighting system (see the render pass convention:
    // this game has one flat stage and realistically 1-3 lights, so a real lighting buffer
    // isn't earning its cost yet).
    float3 lightDirection = normalize(float3(0.4f, -1.0f, 0.3f));
    float3 normal = normalize(input.worldNormal);
    float diffuse = max(dot(normal, -lightDirection), 0.0f);
    float lighting = saturate(0.25f + diffuse * 0.85f);

    return float4(baseColor.rgb * lighting, baseColor.a);
}
