#pragma once

#include "BufferHandle.h"
#include "PlatformMacros.h"

#include <cstddef>

#if defined(OMD_GFX_DX12)
    #include "dx12/BufferDX12.h"
#endif

namespace Renderer
{
    // Upload-heap-backed GPU buffer. Backend-agnostic front layer - see
    // PlatformMacros.h. Simplest possible resource path - upload-heap memory
    // is CPU-visible but slower for the GPU to read than a default-heap
    // copy. Fine for small data, static or per-frame-updated alike (e.g. a
    // per-frame camera constant buffer via Update()).
    //
    // TODO(OM): add a default-heap + upload-copy path once something needs
    // large, rarely-updated data (a big static vertex/index buffer, say) -
    // Update() doesn't replace that need, it only covers small data that's
    // fine staying CPU-visible.
    struct Buffer : public OMD_GFX_CLASS(Buffer)
    {
        static BufferHandle Create(const void* data, size_t sizeBytes)
        {
            return OMD_GFX_CALL(Buffer, Create(data, sizeBytes));
        }

        static void Update(BufferHandle handle, const void* data, size_t sizeBytes)
        {
            OMD_GFX_CALL(Buffer, Update(handle, data, sizeBytes));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(Buffer, Shutdown());
        }
    };
}
