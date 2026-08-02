#pragma once

#include "Renderer/BufferHandle.h"

#include <cstddef>

struct ID3D12Resource;

namespace Renderer
{
    struct BufferDX12
    {
        static BufferHandle Create(const void* data, size_t sizeBytes);
        static void Shutdown();

        // For other dx12/ backend files (e.g. render passes) that need to
        // build a view over the buffer. Only valid until Shutdown().
        static ID3D12Resource* GetResource(BufferHandle handle);
    };
}
