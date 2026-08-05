cbuffer CameraConstants : register(b0)
{
    // Explicit row_major - do not rely on HLSL's default cbuffer packing, which is easy to
    // get backwards from memory. This matches the CPU side's row-major DirectXMath storage
    // byte-for-byte, with no implicit re-interpretation - see ComputeViewProjection in
    // Game/main.cpp for the paired convention (CPU-side transpose once, then this).
    row_major float4x4 ViewProjection;
};

struct VSInput
{
    float3 position : POSITION;
    float3 color : COLOR;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 color : COLOR;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    // Matrix-first mul(), matching the CPU side's row-major-storage-plus-
    // transpose-before-upload convention (see ComputeViewProjection in
    // Game/main.cpp) - HLSL's mul(matrix, vector) treats the vector as a
    // column vector, which is what that combination expects.
    output.position = mul(ViewProjection, float4(input.position, 1.0f));
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(input.color, 1.0f);
}
