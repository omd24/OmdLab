#pragma once

#include "Renderer/StaticMeshDrawItem.h"

#include <vector>

namespace LocalTestScene
{
    // Loads a local multi-mesh/multi-material glTF test scene from local/ if present and
    // returns its draw items via Engine's connective resource layer - empty if the source file
    // isn't found (dev-only test content, see the "Bulk external test content" working
    // convention; never required for a normal build/run). The caller is responsible for
    // combining this with any other source's draw items before handing the combined list to
    // Renderer::StaticMeshPass::SetDrawItems() - this doesn't call it directly, since it isn't
    // the only contributor to that one shared list.
    std::vector<Renderer::StaticMeshDrawItem> LoadIfAvailable();
}
