#pragma once

#include "Renderer/DebugDrawLine.h"
#include "Renderer/DebugDrawRenderDesc.h"

#include <vector>

namespace Renderer
{
    struct DebugDrawPassDX12
    {
        static void Init();
        static void Shutdown();
        static void SetLines(const std::vector<DebugDrawLine>& lines);
        static void Render(const DebugDrawRenderDesc& desc);
    };
}
