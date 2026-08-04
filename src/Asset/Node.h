#pragma once

#include "Types.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Asset
{
    // One node in the scene's transform hierarchy. Not assumed to be "a bone" or "a mesh
    // instance" specifically - a node may carry a mesh, a skin, both, or neither (a pure
    // transform/group node, or one of the stick-figure rig's bone custom-shape widget nodes,
    // which naturally end up with meshIndex == kInvalidIndex since they carry no glTF mesh -
    // no special-case filtering needed in the importer for those).
    struct Node
    {
        std::string name;
        DirectX::XMFLOAT3 translation = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Identity quaternion.
        DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };

        int32_t meshIndex = kInvalidIndex;
        int32_t skinIndex = kInvalidIndex;
        std::vector<int32_t> childNodeIndices; // Indices into Model::nodes.
    };
}
