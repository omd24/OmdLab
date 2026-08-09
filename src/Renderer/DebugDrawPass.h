#pragma once

#include "DebugDrawLine.h"
#include "DebugDrawRenderDesc.h"
#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/DebugDrawPassDX12.h"
#endif

#include <vector>

namespace Renderer
{
    // Draws a caller-supplied list of colored world-space line segments as unlit wireframe.
    // Currently used for the collision module's hitbox/hurtbox/trigger volume visualization
    // (see Engine::BuildCollisionDebugLines), but this pass itself knows nothing about
    // collision, boxes, or any other shape - only lines. Backend-agnostic front layer - see
    // PlatformMacros.h.
    struct DebugDrawPass : public OMD_GFX_CLASS(DebugDrawPass)
    {
        static void Init()
        {
            OMD_GFX_CALL(DebugDrawPass, Init());
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(DebugDrawPass, Shutdown());
        }

        // Replaces the current line list. Called every frame by whoever has fresh lines to
        // show - unlike StaticMeshPass::SetDrawItems (built once at load), this pass's content
        // changes every tick (collision volumes move, overlap color changes). Silently
        // truncated past a fixed capacity if exceeded (see the .cpp) - acceptable for a
        // debug-only visualization.
        static void SetLines(const std::vector<DebugDrawLine>& lines)
        {
            OMD_GFX_CALL(DebugDrawPass, SetLines(lines));
        }

        static void Render(const DebugDrawRenderDesc& desc)
        {
            OMD_GFX_CALL(DebugDrawPass, Render(desc));
        }
    };
}
