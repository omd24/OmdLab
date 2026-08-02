#pragma once

namespace Renderer
{
    // Its own header (like CompiledShader/PipelineDesc) so both the front
    // layer and the DX12 implementation can see the full definition
    // without including each other.
    struct BackgroundRenderDesc
    {
        // Per-frame inputs land here once this pass takes any (e.g. a
        // pattern/color selection) - empty for now.
    };
}
