// Unlit, vertex-colored line-list shader for Renderer::DebugDrawPass - draws whatever colored
// world-space segments the caller supplied (currently the collision module's hitbox/hurtbox/
// trigger wireframes), same vertex shape and math as Triangle.hlsl, kept as its own file since
// this pass's topology (line list) is a distinct PSO from every triangle-drawing pass.

cbuffer CameraConstants : register(b0)
{
    // Explicit row_major - matches every other pass's cbuffer convention (see Triangle.hlsl).
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
    output.position = mul(ViewProjection, float4(input.position, 1.0f));
    output.color = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return float4(input.color, 1.0f);
}
