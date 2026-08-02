#pragma once

namespace Renderer
{
    // Opaque handle into Buffer's internal registry. Its own header so both
    // the front layer and the DX12 implementation can see the full
    // definition without including each other.
    struct BufferHandle
    {
        int index = -1;
    };
}
