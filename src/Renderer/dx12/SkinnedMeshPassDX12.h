#pragma once

#include "Renderer/SkinnedMeshDrawItem.h"
#include "Renderer/SkinnedMeshRenderDesc.h"

#include <vector>

namespace Renderer
{
    struct SkinnedMeshPassDX12
    {
        static void Init();
        static void Shutdown();
        static void SetDrawItems(const std::vector<SkinnedMeshDrawItem>& items);
        static void Render(const SkinnedMeshRenderDesc& desc);
    };
}
