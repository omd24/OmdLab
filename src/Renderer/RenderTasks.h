#pragma once

#include "BackgroundPass.h"
#include "Buffer.h"
#include "Device.h"
#include "Foundation/Log.h"
#include "ForwardPass.h"
#include "ImGuiHelper.h"
#include "Pipeline.h"
#include "PlatformMacros.h"
#include "SkinnedMeshPass.h"
#include "StaticMeshPass.h"
#include "Texture.h"

#include <DirectXMath.h>
#include <functional>

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
            StaticMeshPass::Init();
            SkinnedMeshPass::Init();
            ImGuiHelper::Init(window);
        }

        static void Shutdown()
        {
            ImGuiHelper::Shutdown();
            SkinnedMeshPass::Shutdown();
            StaticMeshPass::Shutdown();
            ForwardPass::Shutdown();
            BackgroundPass::Shutdown();
            Pipeline::Shutdown();
            Buffer::Shutdown();
            Texture::Shutdown();
            Device::Shutdown();
        }

        // viewProjection: computed wherever the camera currently lives (see
        // the camera ownership convention) - a temporary fixed/free-fly
        // camera directly in Game/main.cpp for now, Engine's real Camera
        // once it exists. RenderTasks just threads it to whichever passes
        // need it.
        //
        // primaryContentUI: optional caller-supplied ImGui content for the window's main
        // (non-debug) section - e.g. Game's own "which loaded content is actually on screen"
        // checkbox for its real, shipped content. Invoked first, above the "Debug" section
        // below.
        //
        // debugSectionUI: optional caller-supplied ImGui content invoked inside the "Debug"
        // section, alongside this pass's own bring-up toggles (e.g. Game's toggle for an
        // optional dev-only test scene - the same section as "does the background compute
        // pass run" conceptually, not real shipped content).
        //
        // Both let a non-Renderer caller add debug UI for decisions only it can make -
        // e.g. which of several loaded content categories to include in a pass's draw item
        // list - without RenderTasks (or any other Renderer file) needing to know what that
        // content is. No-op by default.
        //
        // Returns true the one frame "Reset to defaults" is clicked - this pass's own
        // toggles are already reset by the time it returns; the caller (Game/main.cpp) uses
        // the return value to also reset whatever it owns (currently the camera and its own
        // content-category toggles). RenderTasks deliberately doesn't know what any of that
        // caller-owned state is - a bool signal is enough to keep that decoupled, rather than
        // a settings/config type shared across the Renderer/Game boundary.
        static bool DoFrame(
            const DirectX::XMFLOAT4X4& viewProjection, const std::function<void()>& primaryContentUI = {},
            const std::function<void()>& debugSectionUI = {})
        {
            ImGuiHelper::NewFrame();

            // Off by default - this is bring-up-only debug UI (a window rendering alone
            // doesn't prove mouse/keyboard input is actually reaching ImGui, toggling these
            // does), not part of the default view now that real content (see
            // primaryContentUI) exists to look at instead.
            static bool enableBackgroundPass = false;
            static bool enableForwardPass = false;
            bool resetRequested = false;

            ImGui::Begin("Renderer Debug");
            if (primaryContentUI)
            {
                primaryContentUI();
            }
            ImGui::SeparatorText("Debug");
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
            if (debugSectionUI)
            {
                debugSectionUI();
            }
            if (ImGui::Button("Reset to defaults"))
            {
                enableBackgroundPass = false;
                enableForwardPass = false;
                resetRequested = true;
                Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Reset to defaults");
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
                ForwardPass::Render({ viewProjection });
            }
            // No master on/off switch for this one - it's data-driven (see
            // StaticMeshPass::SetDrawItems), and an empty draw item list already draws
            // nothing on its own. A separate switch on top of that would just be a second,
            // easy-to-desync way to hide the same content the caller's own draw-item
            // selection (see extraDebugUI above) already controls.
            StaticMeshPass::Render({ viewProjection });
            SkinnedMeshPass::Render({ viewProjection });
            ImGuiHelper::Render();
            Device::EndFrame();

            return resetRequested;
        }
    };
}
