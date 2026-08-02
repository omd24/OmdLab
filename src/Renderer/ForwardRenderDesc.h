#pragma once

namespace Renderer
{
    // Its own header (like CompiledShader/PipelineDesc) so both the front
    // layer and the DX12 implementation can see the full definition
    // without including each other.
    struct ForwardRenderDesc
    {
        // Per-frame inputs (camera, draw list, lighting) land here once
        // Engine has real scene data to supply - empty for now since this
        // pass currently draws one hardcoded triangle.
    };
}
