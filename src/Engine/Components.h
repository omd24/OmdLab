#pragma once

#include "Renderer/SkinnedMeshDrawItem.h"

#include <DirectXMath.h>

namespace Engine
{
    // Generic per-entity spatial transform - position, orientation, and a uniform scale.
    // Non-uniform scale isn't needed by anything yet; add it if a concrete need appears, per
    // this project's usual "smallest thing that solves the concrete need" bias. Lives in
    // Engine, not Game, since "an entity has a position" has nothing fighting-game-specific
    // about it - the same "Engine provides the mechanism, Game supplies the meaning" split
    // already used for input bindings and the moveset/combat data.
    struct Transform
    {
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Identity quaternion.
        float scale = 1.0f;
    };

    // Composes a world matrix from a Transform - the same Scale * Rotate * Translate order,
    // row-vector convention (v' = v * M), used throughout this project's node-hierarchy walk
    // (see Engine/Animation.cpp). NOT transposed for GPU row_major storage, matching every
    // other CPU-side matrix in this project - callers transpose at the point of upload.
    DirectX::XMMATRIX ComputeWorldMatrix(const Transform& transform);

    // Links an entity to one already-uploaded skinned draw item. Just a pointer into whichever
    // list the caller built via Engine::CreateSkinnedMeshDrawItems - Engine attaches no meaning
    // to which list or why, only that this entity has one. Safe as a raw pointer as long as the
    // owning list is never resized after the items it hands out are linked (true today - draw
    // item lists are built once at load, never appended to afterward).
    struct SkinnedRenderable
    {
        Renderer::SkinnedMeshDrawItem* drawItem = nullptr;
    };
}
