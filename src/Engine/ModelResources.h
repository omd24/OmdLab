#pragma once

#include "Renderer/SkinnedMeshDrawItem.h"
#include "Renderer/StaticMeshDrawItem.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

namespace Asset
{
    struct Model;
}

namespace Engine
{
    // The connective resource layer: turns one imported Asset::Model into ready-to-draw
    // Renderer::StaticMeshDrawItems. Uploads each primitive's vertex/index data as GPU buffers,
    // decodes and uploads each referenced base-color texture once (deduplicated by material,
    // falling back to a shared 1x1 white texture for materials with none), and resolves every
    // node's world matrix by walking the full node hierarchy from its scene roots down through
    // every descendant - not just immediate root children, since a mesh node can sit arbitrarily
    // deep under transform/bone nodes. Renderer never sees Asset::Model, a node hierarchy, or a
    // material - only the flat StaticMeshDrawItem list this produces (the Renderer/Asset
    // dependency rule).
    //
    // No skinning or animation is applied - every node's own authored local transform is used
    // directly, which is exactly what a skinned mesh's vertices are already expressed relative
    // to before any joint transform is applied. Correct output for an unanimated (bind-pose)
    // model, not an approximation of one.
    //
    // textureDirectory: the folder Asset::Texture::filePath entries (relative paths from the
    // source glTF) resolve against.
    //
    // rootTransform: applied above every scene root node, identity by default. Exists for a
    // caller-known correction a source file's own authored hierarchy needs (e.g. a unit-scale
    // mismatch from whatever tool produced it) - not something this generic layer can detect
    // or should guess at from the geometry itself.
    std::vector<Renderer::StaticMeshDrawItem> CreateStaticMeshDrawItems(
        const Asset::Model& model, const std::string& textureDirectory,
        const DirectX::XMMATRIX& rootTransform = DirectX::XMMatrixIdentity());

    // One-time GPU setup for every skinned mesh node in the model (a node carrying both a mesh
    // and a skin) - the skinned counterpart to CreateStaticMeshDrawItems, called once at load
    // the same way. Builds an interleaved vertex buffer (position/normal/uv0 plus each
    // primitive's own joint indices/weights), an index buffer, and a persistent, identity-
    // initialized bone palette buffer - each created once via Buffer::Create, never a shared
    // buffer reused across items (see StaticMeshDrawItem's own comment for why).
    //
    // Identity-initialized rather than posed: this function has no notion of a clip or a time,
    // deliberately - re-running it every frame to reflect a changing pose would re-upload the
    // vertex/index buffers every frame too, leaking new GPU resources each time. Per-frame pose
    // updates are Engine::UpdateSkinnedPose's job, operating on the buffers this function
    // creates just once.
    //
    // rootTransform: same contract as CreateStaticMeshDrawItems's own parameter - applied to
    // this function's own node-hierarchy walk when resolving each skinned mesh node's world
    // matrix, unrelated to (and not applied within) the skinning-matrix computation itself -
    // see Engine::ComputeSkinningMatrices's own comment.
    std::vector<Renderer::SkinnedMeshDrawItem> CreateSkinnedMeshDrawItems(
        const Asset::Model& model, const std::string& textureDirectory,
        const DirectX::XMMATRIX& rootTransform = DirectX::XMMatrixIdentity());
}
