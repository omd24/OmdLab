#pragma once

#include "Renderer/ForwardRenderDesc.h"

namespace Renderer
{
    struct ForwardPassDX12
    {
        static void Init();
        static void Shutdown();
        static void Render(const ForwardRenderDesc& desc);
    };
}
