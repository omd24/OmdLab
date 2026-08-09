#pragma once

#include <DirectXMath.h>

namespace Renderer
{
    // Its own header (like ForwardRenderDesc) so both the front layer and the DX12
    // implementation can see the full definition without including each other.
    struct DebugDrawRenderDesc
    {
        // Combined view * projection matrix, row-major storage - same convention/source as
        // every other pass (see ForwardRenderDesc). Lines are drawn with an implicit identity
        // world matrix - DebugDrawLine's start/end are already world-space.
        DirectX::XMFLOAT4X4 viewProjection;
    };
}
