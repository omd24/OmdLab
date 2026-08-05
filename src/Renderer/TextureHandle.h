#pragma once

namespace Renderer
{
    // Opaque handle into Texture's internal registry. Its own header so both
    // the front layer and the DX12 implementation can see the full
    // definition without including each other.
    struct TextureHandle
    {
        int index = -1;
    };
}
