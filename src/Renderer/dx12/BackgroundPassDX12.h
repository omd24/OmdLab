#pragma once

#include "Renderer/BackgroundRenderDesc.h"

namespace Renderer
{
    struct BackgroundPassDX12
    {
        static void Init();
        static void Shutdown();
        static void Render(const BackgroundRenderDesc& desc);
    };
}
