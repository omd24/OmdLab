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

        // Begins a frame: waits for the GPU to be ready for this frame's
        // resources, resets command recording, and clears the back buffer.
        static void BeginFrame()
        {
            OMD_GFX_CALL(Device, BeginFrame());
        }

        // Ends a frame: closes and submits the command list, presents, and
        // waits for the GPU to finish (fully synchronous for now - no
        // multi-frame pipelining yet).
        static void EndFrame()
        {
            OMD_GFX_CALL(Device, EndFrame());
        }
    };
}
