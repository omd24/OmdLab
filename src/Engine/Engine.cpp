#include "Engine.h"

#include "Asset/Asset.h"
#include "Foundation/Foundation.h"
#include "Renderer/Renderer.h"

#include <cstdio>

namespace Engine
{
    const char* GetName()
    {
        return "Engine";
    }

    void PrintDependencyChain()
    {
        std::printf(
            "%s -> %s -> %s -> %s\n",
            GetName(),
            Renderer::GetName(),
            Asset::GetName(),
            Foundation::GetName());
    }
}
