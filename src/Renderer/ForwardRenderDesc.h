#pragma once

#include <DirectXMath.h>

namespace Renderer
{
    // Its own header (like CompiledShader/PipelineDesc) so both the front
    // layer and the DX12 implementation can see the full definition
    // without including each other.
    struct ForwardRenderDesc
    {
        // Combined view * projection matrix, row-major storage (matches
        // Asset's own DirectXMath matrix convention) - computed wherever the
        // camera currently lives (a temporary fixed/free-fly camera directly
        // in Game/main.cpp for now, Engine's real Camera once it exists; see
        // the camera ownership convention). The pass multiplies this by each
        // draw's world matrix; the world-space triangle vertices this pass
        // currently draws use an implicit identity world matrix.
        DirectX::XMFLOAT4X4 viewProjection;
    };
}
