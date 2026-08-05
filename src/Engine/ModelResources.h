#pragma once

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
}
