#pragma once

#include "Renderer/StaticMeshDrawItem.h"
#include "Renderer/StaticMeshRenderDesc.h"

#include <vector>

namespace Renderer
{
    struct StaticMeshPassDX12
    {
        static void Init();
        static void Shutdown();
        static void SetDrawItems(const std::vector<StaticMeshDrawItem>& items);
        static void Render(const StaticMeshRenderDesc& desc);
    };
}
