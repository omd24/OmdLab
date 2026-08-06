#pragma once

#include "BufferHandle.h"
#include "TextureHandle.h"

#include <DirectXMath.h>

namespace Renderer
{
    // One indexed draw call: a vertex/index buffer pair, the texture to sample, and a world
    // matrix. Renderer-owned boundary type - populated by whichever caller has already done
    // the Asset-CPU-data-to-Renderer-GPU-resource translation (Engine's connective resource
    // layer, see ModelResources.h). StaticMeshPass never sees a glTF/Asset::Model/
    // Asset::Material - only this.
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
        // A buffer the caller creates once (Buffer::Create), holding just this item's world
        // matrix - not a raw XMFLOAT4X4 re-uploaded into one shared buffer at draw time. A
        // single shared upload buffer, Update()-d once per item inside one pass's draw loop,
        // is only actually written by the CPU before the GPU executes any of that frame's
        // recorded commands - every draw in the list would end up reading whichever item's
        // matrix was written last, not its own (a real bug this shape once had, caught when a
        // second content category was added to the same draw list). Each item owning its own
        // buffer, exactly like vertexBuffer/indexBuffer already do, has no such hazard.
        BufferHandle worldBuffer;
    };
}
