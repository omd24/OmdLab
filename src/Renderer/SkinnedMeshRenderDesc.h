#pragma once

#include <DirectXMath.h>

namespace Renderer
{
    // Its own header (like StaticMeshRenderDesc) so both the front layer and the DX12
    // implementation can see the full definition without including each other.
    struct SkinnedMeshRenderDesc
    {
        DirectX::XMFLOAT4X4 viewProjection;
    };
}
