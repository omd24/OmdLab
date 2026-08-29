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
    // Draws indexed, lit-textured, non-animated ("static" - rigid mesh data with no bone
    // weights, the industry-standard sense used by Unreal's Static Mesh/Skeletal Mesh split
    // or Unity's equivalent, NOT "never moves" - a StaticMeshDrawItem's own world matrix can
    // and does change every frame, e.g. Game::FighterShadow's discs) geometry via
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
        // content isn't available). Internally partitions by StaticMeshDrawItem::transparent
        // into two lists, one per RenderOpaque/RenderTransparent below.
        static void SetDrawItems(const std::vector<StaticMeshDrawItem>& items)
        {
            OMD_GFX_CALL(StaticMeshPass, SetDrawItems(items));
        }

        // Split into two entry points (rather than one Render() doing both in sequence) so
        // RenderTasks can interleave this pass's two halves with every other pass's - the
        // real invariant this project wants is "every opaque draw across the whole frame
        // happens before any transparent draw," not "each pass is internally ordered," which
        // a single combined Render() can't express on its own. See RenderTasks::DoFrame.
        static void RenderOpaque(const StaticMeshRenderDesc& desc)
        {
            OMD_GFX_CALL(StaticMeshPass, RenderOpaque(desc));
        }

        static void RenderTransparent(const StaticMeshRenderDesc& desc)
        {
            OMD_GFX_CALL(StaticMeshPass, RenderTransparent(desc));
        }
    };
}
