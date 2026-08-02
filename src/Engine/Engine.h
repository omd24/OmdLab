#pragma once

namespace Engine
{
    const char* GetName();

    // Calls into Renderer, Asset, and Foundation to prove the dependency
    // chain links and resolves symbols correctly across all four projects.
    void PrintDependencyChain();
}
