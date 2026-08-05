#pragma once

#include "PlatformMacros.h"
#include "StaticMeshDrawItem.h"
#include "StaticMeshRenderDesc.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/StaticMeshPassDX12.h"
#endif

#include <vector>

namespace Renderer
{
    // Draws indexed, lit-textured, non-animated geometry (StaticMeshDrawItem) via
    // LitTextured.hlsl - generic, not specific to any one asset (validated against a
    // multi-mesh/multi-material local test scene, but nothing here names it). Backend-agnostic
    // front layer - see PlatformMacros.h.
    struct StaticMeshPass : public OMD_GFX_CLASS(StaticMeshPass)
    {
        static void Init()
        {
            OMD_GFX_CALL(StaticMeshPass, Init());
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(StaticMeshPass, Shutdown());
        }

        // Replaces the current draw item list. Called once by whichever caller has already
        // created GPU resources for what it wants drawn (see StaticMeshDrawItem) - not
        // per-frame, since nothing currently rebuilds this list at runtime. An empty list is
        // valid and simply draws nothing (the graceful-degradation path when the source
        // content isn't available).
        static void SetDrawItems(const std::vector<StaticMeshDrawItem>& items)
        {
            OMD_GFX_CALL(StaticMeshPass, SetDrawItems(items));
        }

        static void Render(const StaticMeshRenderDesc& desc)
        {
            OMD_GFX_CALL(StaticMeshPass, Render(desc));
        }
    };
}
