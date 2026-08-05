#pragma once

#include "BufferHandle.h"
#include "TextureHandle.h"

#include <DirectXMath.h>

namespace Renderer
{
    // One indexed draw call: a vertex/index buffer pair, the texture to sample, and a world
    // matrix. Renderer-owned boundary type - populated by whichever caller has already done
    // the Asset-CPU-data-to-Renderer-GPU-resource translation (Game/main.cpp for now, since
    // Engine's real connective resource layer doesn't exist yet; see the incremental plan).
    // StaticMeshPass never sees a glTF/Asset::Model/Asset::Material - only this.
    struct StaticMeshDrawItem
    {
        BufferHandle vertexBuffer;
        // Caller-specified, not assumed - the source vertex struct (Asset::Vertex, today)
        // may carry fields this pass's own input layout doesn't declare (e.g. tangent),
        // which still count toward the actual per-vertex byte stride in the uploaded buffer.
        unsigned int vertexStride = 0;
        BufferHandle indexBuffer;
        unsigned int indexCount = 0;
        TextureHandle baseColorTexture;
        DirectX::XMFLOAT4X4 world;
    };
}
