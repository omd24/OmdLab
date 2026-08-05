#pragma once

#include "Types.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Asset
{
    struct Vertex
    {
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 normal = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT2 uv0 = { 0.0f, 0.0f };          // (0, 0) when the source has no UVs.
        DirectX::XMFLOAT4 tangent = { 0.0f, 0.0f, 0.0f, 1.0f };
    };

    // Present only for vertices belonging to a skinned primitive (see Primitive::skinning).
    // Joint indices are local to the primitive's Skin (Model::skins[skinIndex]), matching
    // glTF's own JOINTS_0 convention - they are NOT indices into Model::nodes.
    struct SkinningData
    {
        uint16_t jointIndices[4] = { 0, 0, 0, 0 };
        float jointWeights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // One glTF mesh primitive - its own vertex/index buffer and its own material, so a mesh
    // with multiple materials (typical for a large multi-material scene, not just this
    // project's single-material stick-figure pieces) is representable without splitting Mesh
    // itself.
    struct Primitive
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<SkinningData> skinning; // Empty when this primitive isn't skinned.
        int32_t materialIndex = kInvalidIndex;
    };

    struct Mesh
    {
        std::string name;
        std::vector<Primitive> primitives;
    };
}
