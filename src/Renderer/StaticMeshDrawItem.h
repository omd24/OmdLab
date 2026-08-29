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
    // Asset::Material - only this. "Static" here means rigid, non-skinned vertex data (no
    // bone weights) - the industry-standard sense (Unreal's Static Mesh/Skeletal Mesh split,
    // Unity's equivalent) - not "never moves": worldBuffer below is routinely updated every
    // frame (e.g. Game::FighterShadow's discs tracking their fighter's position).
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

        // Unset (default-constructed, index == -1) for ordinary opaque content - the pass
        // binds its own shared white/opaque default in that case (see StaticMeshPassDX12), so
        // most callers (ground plane, local test scene) never need to touch this. Only a
        // caller whose content actually fades (e.g. a fighter's own shadow) creates and
        // updates a real one - see LitTextured.hlsl's TintConstants for the shader side.
        BufferHandle tintBuffer;

        // False (opaque) by default. True routes this item through the pass's blend-enabled
        // PSO instead of the opaque one, and into the transparent draw order (all opaque items
        // draw first, then all transparent ones - see StaticMeshPassDX12::SetDrawItems).
        bool transparent = false;
    };
}
