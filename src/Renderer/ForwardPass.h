#pragma once

#include "ForwardRenderDesc.h"
#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/ForwardPassDX12.h"
#endif

namespace Renderer
{
    // Draws opaque (and, later, skinned) geometry with lighting computed
    // directly in the pixel shader - forward, not deferred: no G-buffer or
    // separate lighting pass needed at this scale. Backend-agnostic front
    // layer - see PlatformMacros.h.
    struct ForwardPass : public OMD_GFX_CLASS(ForwardPass)
    {
        static void Init()
        {
            OMD_GFX_CALL(ForwardPass, Init());
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(ForwardPass, Shutdown());
        }

        static void Render(const ForwardRenderDesc& desc)
        {
            OMD_GFX_CALL(ForwardPass, Render(desc));
        }
    };
}
