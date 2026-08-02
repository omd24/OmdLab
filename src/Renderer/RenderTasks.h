#pragma once

#include "BackgroundPass.h"
#include "Buffer.h"
#include "Device.h"
#include "Foundation/Log.h"
#include "ForwardPass.h"
#include "ImGuiHelper.h"
#include "Pipeline.h"
#include "PlatformMacros.h"

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

#include <imgui.h>

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

            // Bring-up-only debug UI: a window rendering alone doesn't
            // prove mouse/keyboard input is actually reaching ImGui -
            // toggling these does.
            static bool enableBackgroundPass = true;
            static bool enableForwardPass = true;
            ImGui::Begin("Renderer Debug");
            if (ImGui::Checkbox("Background compute pass", &enableBackgroundPass))
            {
                Foundation::Log::Write(
                    Foundation::Log::Severity::Info, "Renderer", "Background compute pass %s", enableBackgroundPass ? "enabled" : "disabled");
            }
            if (ImGui::Checkbox("Forward triangle pass", &enableForwardPass))
            {
                Foundation::Log::Write(
                    Foundation::Log::Severity::Info, "Renderer", "Forward triangle pass %s", enableForwardPass ? "enabled" : "disabled");
            }
            ImGui::End();

            Device::BeginFrame();
            if (enableBackgroundPass)
            {
                BackgroundPass::Render({});
                Device::CompositeComputeTarget();
            }
            else
            {
                // Skipping the compute dispatch but still calling
                // CompositeComputeTarget() would copy in whatever the
                // compute target was last left holding, frozen rather than
                // actually absent - clear the back buffer directly instead.
                Device::ClearAndBindRenderTarget();
            }
            if (enableForwardPass)
            {
                ForwardPass::Render({});
            }
            ImGuiHelper::Render();
            Device::EndFrame();
        }
    };
}
