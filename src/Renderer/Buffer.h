#pragma once

#include "BufferHandle.h"
#include "PlatformMacros.h"

#include <cstddef>

#if defined(OMD_GFX_DX12)
    #include "dx12/BufferDX12.h"
#endif

namespace Renderer
{
    // Upload-heap-backed GPU buffer, initialized once from CPU data at
    // creation. Backend-agnostic front layer - see PlatformMacros.h.
    // Simplest possible resource path - upload-heap memory is CPU-visible
    // but slower for the GPU to read than a default-heap copy, and this
    // offers no way to update the data after creation. Fine for small,
    // static, create-once data (the current vertex buffer use); revisit
    // with a default-heap + upload-copy path once something needs either
    // large data or per-frame updates.
    struct Buffer : public OMD_GFX_CLASS(Buffer)
    {
        static BufferHandle Create(const void* data, size_t sizeBytes)
        {
            return OMD_GFX_CALL(Buffer, Create(data, sizeBytes));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(Buffer, Shutdown());
        }
    };
}
