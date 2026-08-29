#pragma once

#include "BufferHandle.h"
#include "TextureHandle.h"

namespace Renderer
{
    // Bone palette capacity, shared between C++ and SkinnedMesh.hlsl (kept in sync manually -
    // Shader::CompileShader has no macro-define plumbing yet, and no second caller exists to
    // justify adding it). 32 comfortably covers this project's one skinned asset (22 joints);
    // Engine::CreateSkinnedMeshDrawItems asserts a future skin doesn't exceed it.
    constexpr unsigned int kMaxSkinJoints = 32;

    // One indexed, GPU-skinned draw call. Distinct from StaticMeshDrawItem rather than an
    // extension of it - the vertex layout (joint indices/weights) and root signature (an extra
    // bone palette CBV) genuinely differ, the same test that already justifies ForwardPass/
    // StaticMeshPass coexisting as separate passes.
    struct SkinnedMeshDrawItem
    {
        BufferHandle vertexBuffer;
        unsigned int vertexStride = 0;
        BufferHandle indexBuffer;
        unsigned int indexCount = 0;
        TextureHandle baseColorTexture;
        // Own buffer, created once - see StaticMeshDrawItem's own comment for why a single
        // shared/reused per-draw-item buffer is unsafe.
        BufferHandle worldBuffer;
        // Per-joint skinning matrices, updated in place every frame as the pose advances
        // (unlike worldBuffer, which is set once) - see Engine::UpdateSkinnedPose.
        BufferHandle bonePaletteBuffer;

        // Unset (default-constructed, index == -1) for ordinary opaque content - the pass
        // binds its own shared white/opaque default in that case (see SkinnedMeshPassDX12),
        // mirroring StaticMeshDrawItem::tintBuffer's own comment. Nothing sets this yet
        // (no skinned content currently fades) - built ahead of a concrete need, at the
        // user's own explicit request, so a future VFX/UI feature (a hit-flash, a fade-out)
        // has somewhere to plug in.
        BufferHandle tintBuffer;

        // False (opaque) by default - see StaticMeshDrawItem::transparent's own comment.
        bool transparent = false;
    };
}
