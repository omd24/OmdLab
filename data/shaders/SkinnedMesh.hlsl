// LitTextured.hlsl's lighting/texture logic, with the vertex shader blending a bone palette
// before the existing World/ViewProjection chain (see SkinnedMeshDrawItem.h for kMaxSkinJoints,
// kept in sync with BonePalette's array size below).

cbuffer CameraConstants : register(b0)
{
    // Explicit row_major - see Triangle.hlsl for why (removes any dependence on HLSL's default
    // cbuffer packing, which is easy to get backwards from memory).
    row_major float4x4 ViewProjection;
};

cbuffer ObjectConstants : register(b1)
{
    row_major float4x4 World;
};

cbuffer BonePaletteConstants : register(b2)
{
    row_major float4x4 BonePalette[32];
};

// Per-item tint/alpha multiplier - see LitTextured.hlsl's own TintConstants comment (the same
// mechanism, at b3 here since b2 is already the bone palette in this shader).
cbuffer TintConstants : register(b3)
{
    float4 TintAndAlpha;
};

Texture2D BaseColorTexture : register(t0);
SamplerState BaseColorSampler : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0;
    // Stored as floats, not an integer format - see SkinnedMeshVertex's own comment
    // (SkinnedMeshPassDX12.cpp) for why. Exact integers up to 32 round-trip losslessly.
    float4 jointIndices : JOINTS0;
    float4 jointWeights : WEIGHTS0;
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

    // Weighted sum of the per-joint skinning matrices - see
    // Engine::ComputeSkinningMatrices's own comment for the CPU-side derivation this blends
    // the GPU half of. Each BonePalette entry is already transposed for row_major storage
    // individually (safe here specifically because this is a weighted sum, and transpose
    // distributes over addition).
    float4x4 skin = BonePalette[(uint)round(input.jointIndices.x)] * input.jointWeights.x
                  + BonePalette[(uint)round(input.jointIndices.y)] * input.jointWeights.y
                  + BonePalette[(uint)round(input.jointIndices.z)] * input.jointWeights.z
                  + BonePalette[(uint)round(input.jointIndices.w)] * input.jointWeights.w;

    float4 skinnedPosition = mul(skin, float4(input.position, 1.0f));
    float4 worldPosition = mul(World, skinnedPosition);
    output.position = mul(ViewProjection, worldPosition);

    // Correct for rotation + uniform scale, same shortcut LitTextured.hlsl already takes for
    // World - no new gap introduced.
    float3 skinnedNormal = mul((float3x3)skin, input.normal);
    output.worldNormal = mul((float3x3)World, skinnedNormal);
    output.uv0 = input.uv0;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 baseColor = BaseColorTexture.Sample(BaseColorSampler, input.uv0);

    // Same hardcoded single-light approximation as LitTextured.hlsl - see its own comment for
    // why a general lighting system isn't earning its cost yet.
    float3 lightDirection = normalize(float3(0.4f, -1.0f, 0.3f));
    float3 normal = normalize(input.worldNormal);
    float diffuse = max(dot(normal, -lightDirection), 0.0f);
    float lighting = saturate(0.25f + diffuse * 0.85f);

    return float4(baseColor.rgb * lighting * TintAndAlpha.rgb, baseColor.a * TintAndAlpha.a);
}
