#pragma once

#include "Types.h"

#include <string>

namespace Asset
{
    // One glTF material, kept close to glTF's own metallic-roughness model since that's the
    // only source format this engine imports today (see the Asset format extensibility
    // decision - a future non-glTF importer would populate the same fields from whatever its
    // own material model is). Factors always hold a usable value (glTF's own spec defaults
    // when a material omits them); texture indices are kInvalidIndex when a slot isn't backed
    // by a texture. The validated stick-figure test asset is flat-colored/unlit with zero
    // textures, so every texture index is expected to be kInvalidIndex for it today - that's
    // the case this struct must not special-case away.
    struct Material
    {
        std::string name;

        float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float emissiveFactor[3] = { 0.0f, 0.0f, 0.0f };

        int32_t baseColorTexture = kInvalidIndex;
        int32_t normalTexture = kInvalidIndex;
        int32_t metallicRoughnessTexture = kInvalidIndex;
        int32_t emissiveTexture = kInvalidIndex;
    };
}
