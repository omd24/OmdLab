#pragma once

#include "BackgroundPass.h"
#include "Buffer.h"
#include "Device.h"
#include "ForwardPass.h"
#include "ImGuiHelper.h"
#include "Pipeline.h"
#include "PlatformMacros.h"

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace Renderer
{
    // Owns which passes exist, their init/shutdown, and the fixed order
    // they run in each frame - the one seam between "Renderer has passes"
    // and "something calls them." Game (or any other non-Renderer system)
    // only ever calls RenderTasks, never individual passes or Device
    // directly - keeps per-frame pass sequencing from creeping outside
    // Renderer as more passes are added.
    //
    // Pure orchestration over already backend-routed calls (Device,
    // BackgroundPass, ForwardPass) - no D3D12-specific code of its own, so
    // unlike those types this doesn't need an OMD_GFX_CLASS/OMD_GFX_CALL
    // split; there's nothing backend-specific here to route.
    //
    // The frame order below (compute background, then graphics draw on
    // top) is a hardcoded sequence, not a data-driven pass list.
    //
    // TODO(OM): revisit once Engine has real scene data and more passes
    // exist to decide an order between.
    struct RenderTasks
    {
        static void Init(HWND window, unsigned int width, unsigned int height)
        {
            Device::Init(window, width, height);
            BackgroundPass::Init();
            ForwardPass::Init();
            ImGuiHelper::Init(window);
        }

        static void Shutdown()
        {
            ImGuiHelper::Shutdown();
            ForwardPass::Shutdown();
            BackgroundPass::Shutdown();
            Pipeline::Shutdown();
            Buffer::Shutdown();
            Device::Shutdown();
        }

        static void DoFrame()
        {
            ImGuiHelper::NewFrame();

            Device::BeginFrame();
            BackgroundPass::Render({});
            Device::CompositeComputeTarget();
            ForwardPass::Render({});
            ImGuiHelper::Render();
            Device::EndFrame();
        }
    };
}
