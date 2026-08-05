#pragma once

namespace LocalTestScene
{
    // Loads a local multi-mesh/multi-material glTF test scene from local/ if present, builds
    // GPU resources, and hands them to Renderer::StaticMeshPass. Logs and does nothing if the
    // source file isn't found - this is dev-only test content (see the "Bulk external test
    // content" working convention), never required for a normal build/run. Temporary bring-up
    // code: this is the Asset-CPU-data-to-Renderer-GPU-resource translation Engine's real
    // connective resource layer will eventually own (see the incremental plan) - it lives here
    // only because Engine doesn't exist yet.
    void LoadIfAvailable();
}
