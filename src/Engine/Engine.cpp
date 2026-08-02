#include "Engine.h"

#include "Asset/Asset.h"
#include "Foundation/Foundation.h"
#include "Foundation/Log.h"
#include "Renderer/Renderer.h"

namespace Engine
{
    const char* GetName()
    {
        return "Engine";
    }

    void PrintDependencyChain()
    {
        Foundation::Log::Write(
            Foundation::Log::Severity::Info,
            "Engine",
            "%s -> %s -> %s -> %s",
            GetName(),
            Renderer::GetName(),
            Asset::GetName(),
            Foundation::GetName());
    }
}
