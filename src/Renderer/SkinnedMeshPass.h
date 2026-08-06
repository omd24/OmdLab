#pragma once

#include "PlatformMacros.h"
#include "SkinnedMeshDrawItem.h"
#include "SkinnedMeshRenderDesc.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/SkinnedMeshPassDX12.h"
#endif

#include <vector>

namespace Renderer
{
    // Draws indexed, lit-textured, GPU-skinned geometry (SkinnedMeshDrawItem) via
    // SkinnedMesh.hlsl. Backend-agnostic front layer - see PlatformMacros.h.
    struct SkinnedMeshPass : public OMD_GFX_CLASS(SkinnedMeshPass)
    {
        static void Init()
        {
            OMD_GFX_CALL(SkinnedMeshPass, Init());
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(SkinnedMeshPass, Shutdown());
        }

        // Replaces the current draw item list - see StaticMeshPass::SetDrawItems, same
        // not-per-frame contract.
        static void SetDrawItems(const std::vector<SkinnedMeshDrawItem>& items)
        {
            OMD_GFX_CALL(SkinnedMeshPass, SetDrawItems(items));
        }

        static void Render(const SkinnedMeshRenderDesc& desc)
        {
            OMD_GFX_CALL(SkinnedMeshPass, Render(desc));
        }
    };
}
