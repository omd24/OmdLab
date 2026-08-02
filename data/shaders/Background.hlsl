RWTexture2D<float4> Output : register(u0);

static const uint kCheckerSize = 32;
static const float4 kColorA = float4(0.10f, 0.10f, 0.15f, 1.0f);
static const float4 kColorB = float4(0.25f, 0.25f, 0.35f, 1.0f);

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 tile = dispatchThreadID.xy / kCheckerSize;
    bool isEven = (tile.x + tile.y) % 2 == 0;
    Output[dispatchThreadID.xy] = isEven ? kColorA : kColorB;
}
