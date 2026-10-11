#pragma once

#include "BackgroundPass.h"
#include "Buffer.h"
#include "DebugDrawPass.h"
#include "Device.h"
#include "Foundation/Log.h"
#include "ForwardPass.h"
#include "ImGuiHelper.h"
#include "Pipeline.h"
#include "PlatformMacros.h"
#include "SkinnedMeshPass.h"
#include "StaticMeshPass.h"
#include "Texture.h"
#include "UIPass.h"

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
            UIPass::Init();
#ifdef OMD_DEV_TOOLS
            // DebugDrawPass and ImGui are dev-only tooling (see build/main.sharpmake.cs's
            // OMD_DEV_TOOLS comment) - gated together since DebugDrawPass is only ever toggled
            // through the ImGui window below, and ImGui itself has nothing to draw without it.
            DebugDrawPass::Init();
#endif
#if defined(OMD_DEV_TOOLS) || defined(OMD_PROTOTYPE)
            ImGuiHelper::Init(window);
#endif
        }

        static void Shutdown()
        {
#if defined(OMD_DEV_TOOLS) || defined(OMD_PROTOTYPE)
            ImGuiHelper::Shutdown();
#endif
#ifdef OMD_DEV_TOOLS
            DebugDrawPass::Shutdown();
#endif
            UIPass::Shutdown();
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
        // extraWindowsUI: optional caller-supplied ImGui content for any further windows beyond
        // "Renderer Debug" itself - unlike the two above, this one owns its own Begin()/End()
        // calls entirely (RenderTasks never names or sizes these windows). Exists so a caller
        // with enough debug UI to make one window unwieldy to scroll (Game's own fighter/combo
        // readouts and animation-editing tools, say) can split it into several without
        // RenderTasks needing to know how many windows that is or what any of them are called -
        // same "caller supplies content, this file supplies none of its meaning" contract as the
        // two above, just not nested inside "Renderer Debug". Invoked only where the two above
        // are (OMD_DEV_TOOLS, showDebugUI) so all of a caller's debug windows show/hide together.
        //
        // All three let a non-Renderer caller add debug UI for decisions only it can make -
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
        // primaryContentUI/debugSectionUI are only ever invoked under OMD_DEV_TOOLS (see below) -
        // still accepted unconditionally so Game/main.cpp doesn't need its own #ifdef around
        // this call, just around constructing the lambdas it passes in.
        // showDebugUI (dev-tools builds only): when false, the "Renderer Debug" window and the
        // caller's primary/debug UI lambdas are skipped for this frame - ImGui still runs (an
        // empty frame), so anything drawn straight onto its foreground draw list (e.g. the
        // annotation below) still shows. Lets a screenshot be taken without the panel in it.
        //
        // annotationText (dev-tools builds only): a short line drawn top-left on the ImGui
        // foreground draw list, over everything and regardless of showDebugUI - so a captured
        // frame carries its own sim tick / state. Null or empty draws nothing.
        //
        // hudUI: optional caller-supplied drawing for the always-visible in-game overlay. Invoked
        // every frame outside the showDebugUI gate (so hiding the debug windows never hides it)
        // and outside OMD_DEV_TOOLS (it only needs the ImGui frame, which is also enabled by
        // OMD_PROTOTYPE). Expected to draw on ImGui's background draw list rather than open
        // windows, so it never takes input and debug windows draw over it.
        static bool DoFrame(
            const DirectX::XMFLOAT4X4& viewProjection, const std::function<void()>& primaryContentUI = {},
            const std::function<void()>& debugSectionUI = {}, const std::function<void()>& extraWindowsUI = {}, bool showDebugUI = true,
            const char* annotationText = nullptr, const std::function<void()>& hudUI = {})
        {
#if defined(OMD_DEV_TOOLS) || defined(OMD_PROTOTYPE)
            ImGuiHelper::NewFrame();
#endif
#ifdef OMD_DEV_TOOLS

            if (annotationText != nullptr && annotationText[0] != '\0')
            {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const ImVec2 pad(6.0f, 4.0f);
                const ImVec2 textPos(8.0f, 8.0f);
                const ImVec2 textSize = ImGui::CalcTextSize(annotationText);
                fg->AddRectFilled(
                    ImVec2(textPos.x - pad.x, textPos.y - pad.y), ImVec2(textPos.x + textSize.x + pad.x, textPos.y + textSize.y + pad.y),
                    IM_COL32(0, 0, 0, 170));
                fg->AddText(textPos, IM_COL32(255, 255, 255, 255), annotationText);
            }

            // Off by default - this is bring-up-only debug UI (a window rendering alone
            // doesn't prove mouse/keyboard input is actually reaching ImGui, toggling these
            // does), not part of the default view now that real content (see
            // primaryContentUI) exists to look at instead.
            static bool enableBackgroundPass = false;
            static bool enableForwardPass = false;
            // Also bring-up-only/off-by-default, same reasoning as the two above - unlike
            // StaticMeshPass/SkinnedMeshPass (always-on, data-driven), this pass exists purely
            // to visualize the collision module's volumes on request, not as default content.
            static bool enableDebugDrawPass = false;
            bool resetRequested = false;

            if (showDebugUI)
            {
                ImGui::Begin("Renderer Debug");
                if (primaryContentUI)
                {
                    primaryContentUI();
                }
                ImGui::SeparatorText("Debug");
                if (ImGui::Checkbox("Background compute pass", &enableBackgroundPass))
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Info, "Renderer", "Background compute pass %s",
                        enableBackgroundPass ? "enabled" : "disabled");
                }
                if (ImGui::Checkbox("Forward triangle pass", &enableForwardPass))
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Info, "Renderer", "Forward triangle pass %s", enableForwardPass ? "enabled" : "disabled");
                }
                if (ImGui::Checkbox("Collision debug draw", &enableDebugDrawPass))
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Info, "Renderer", "Collision debug draw %s", enableDebugDrawPass ? "enabled" : "disabled");
                }
                if (debugSectionUI)
                {
                    debugSectionUI();
                }
                if (ImGui::Button("Reset to defaults"))
                {
                    enableBackgroundPass = false;
                    enableForwardPass = false;
                    enableDebugDrawPass = false;
                    resetRequested = true;
                    Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Reset to defaults");
                }
                ImGui::End();

                if (extraWindowsUI)
                {
                    extraWindowsUI();
                }
            }
#endif

#if defined(OMD_DEV_TOOLS) || defined(OMD_PROTOTYPE)
            if (hudUI)
            {
                hudUI();
            }
#endif

            Device::BeginFrame();
#ifdef OMD_DEV_TOOLS
            if (enableBackgroundPass)
            {
                BackgroundPass::Render({});
                Device::CompositeComputeTarget();
            }
            else
#endif
            {
                // Skipping the compute dispatch but still calling
                // CompositeComputeTarget() would copy in whatever the
                // compute target was last left holding, frozen rather than
                // actually absent - clear the back buffer directly instead.
                Device::ClearAndBindRenderTarget();
            }
#ifdef OMD_DEV_TOOLS
            if (enableForwardPass)
            {
                ForwardPass::Render({ viewProjection });
            }
#endif
            // No master on/off switch for this one - it's data-driven (see
            // StaticMeshPass::SetDrawItems), and an empty draw item list already draws
            // nothing on its own. A separate switch on top of that would just be a second,
            // easy-to-desync way to hide the same content the caller's own draw-item
            // selection (see extraDebugUI above) already controls.
            //
            // Opaque-then-transparent is deliberately a GLOBAL ordering across both passes,
            // not something each pass does independently - every opaque draw (either mesh
            // type) must happen before any transparent draw (either mesh type), or a
            // transparent static-mesh item (e.g. a fighter's shadow) could render before a
            // still-opaque skinned character it should be blending against. See
            // StaticMeshPass::RenderOpaque/RenderTransparent's own comment.
            StaticMeshPass::RenderOpaque({ viewProjection });
            SkinnedMeshPass::RenderOpaque({ viewProjection });
            // Shadows darken what the opaque passes drew, so they go between the opaque and
            // transparent content.
            SkinnedMeshPass::RenderShadows({ viewProjection });
            StaticMeshPass::RenderTransparent({ viewProjection });
            SkinnedMeshPass::RenderTransparent({ viewProjection });
            // Unconditional, not OMD_DEV_TOOLS-gated - real UI is shipped content once it
            // exists, same "always-on, data-driven" precedent StaticMeshPass/SkinnedMeshPass
            // already follow (today this is an empty stub, so it draws nothing regardless -
            // see UIPass.h's own comment).
            UIPass::Render();
#ifdef OMD_DEV_TOOLS
            if (enableDebugDrawPass)
            {
                DebugDrawPass::Render({ viewProjection });
            }
#endif
#if defined(OMD_DEV_TOOLS) || defined(OMD_PROTOTYPE)
            ImGuiHelper::Render();
#endif
            Device::EndFrame();

#ifdef OMD_DEV_TOOLS
            return resetRequested;
#else
            return false;
#endif
        }
    };
}
