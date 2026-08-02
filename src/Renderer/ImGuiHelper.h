#pragma once

#include "PlatformMacros.h"

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

#if defined(OMD_GFX_DX12)
    #include "dx12/ImGuiHelperDX12.h"
#endif

namespace Renderer
{
    // Dear ImGui plumbing (context, platform/renderer backend init, the
    // per-frame NewFrame/Render bookends). What gets drawn between them is
    // not Renderer's concern - Engine/Game content later; a demo window for
    // now, as the visual check that the integration itself works. Backend-
    // agnostic front layer - see PlatformMacros.h.
    struct ImGuiHelper : public OMD_GFX_CLASS(ImGuiHelper)
    {
        static void Init(HWND window)
        {
            OMD_GFX_CALL(ImGuiHelper, Init(window));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(ImGuiHelper, Shutdown());
        }

        // Call once at the start of a frame, before any ImGui:: calls.
        static void NewFrame()
        {
            OMD_GFX_CALL(ImGuiHelper, NewFrame());
        }

        // Call once at the end of a frame, with a render target already
        // bound - draws on top of whatever else rendered this frame.
        static void Render()
        {
            OMD_GFX_CALL(ImGuiHelper, Render());
        }
    };
}
