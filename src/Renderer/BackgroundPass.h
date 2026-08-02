#pragma once

#include "BackgroundRenderDesc.h"
#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/BackgroundPassDX12.h"
#endif

namespace Renderer
{
    // Writes a full-screen pattern via a compute dispatch instead of a
    // draw call. Backend-agnostic front layer - see PlatformMacros.h.
    struct BackgroundPass : public OMD_GFX_CLASS(BackgroundPass)
    {
        static void Init()
        {
            OMD_GFX_CALL(BackgroundPass, Init());
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(BackgroundPass, Shutdown());
        }

        static void Render(const BackgroundRenderDesc& desc)
        {
            OMD_GFX_CALL(BackgroundPass, Render(desc));
        }
    };
}
