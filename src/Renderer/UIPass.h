#pragma once

#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/UIPassDX12.h"
#endif

namespace Renderer
{
    // Screen-space UI (health bars, future HUD/menu content) - currently an orchestration-only
    // stub: reserves this pass's place in the per-frame sequence (RenderTasks::DoFrame, after
    // SkinnedMeshPass, before debug draw/ImGui) without rendering anything yet. A real
    // screen-space renderer (orthographic projection, pixel-vs-NDC coordinate convention, text
    // vs quads) is deliberately out of scope here - a separate future design question, not
    // implied by this stub's existence. Backend-agnostic front layer - see PlatformMacros.h.
    struct UIPass : public OMD_GFX_CLASS(UIPass)
    {
        static void Init()
        {
            OMD_GFX_CALL(UIPass, Init());
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(UIPass, Shutdown());
        }

        static void Render()
        {
            OMD_GFX_CALL(UIPass, Render());
        }
    };
}
