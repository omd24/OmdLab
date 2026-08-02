#pragma once

#include "PlatformMacros.h"

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

#if defined(OMD_GFX_DX12)
    #include "dx12/DeviceDX12.h"
#endif

namespace Renderer
{
    // Owns the graphics device, swap chain, and per-frame submission state.
    // Backend-agnostic front layer - see PlatformMacros.h.
    struct Device : public OMD_GFX_CLASS(Device)
    {
        static void Init(HWND window, unsigned int width, unsigned int height)
        {
            OMD_GFX_CALL(Device, Init(window, width, height));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(Device, Shutdown());
        }

        // One frame's fixed recipe - see RenderTasks for where these get
        // called in order, and DeviceDX12.h for what each stage does.
        static void BeginFrame()
        {
            OMD_GFX_CALL(Device, BeginFrame());
        }

        static void CompositeComputeTarget()
        {
            OMD_GFX_CALL(Device, CompositeComputeTarget());
        }

        // Ends a frame: closes and submits the command list, presents, and
        // waits for the GPU to finish (fully synchronous - no multi-frame
        // pipelining).
        //
        // TODO(OM): pipeline multiple frames in flight once performance
        // actually demands it.
        static void EndFrame()
        {
            OMD_GFX_CALL(Device, EndFrame());
        }
    };
}
