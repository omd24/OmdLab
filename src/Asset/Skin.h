#pragma once

#include "Types.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Asset
{
    // One joint within a Skin. parentJointIndex is local to this Skin's joints array (found by
    // walking the joint's node up its parent chain until another one of this skin's joints is
    // found, or the hierarchy runs out) - deliberately not "the joint's node's parent node",
    // since a skin's joints aren't guaranteed to be immediate node-parent/child pairs.
    struct Joint
    {
        std::string name;
        int32_t nodeIndex = kInvalidIndex;         // Where this joint actually lives in Model::nodes.
        int32_t parentJointIndex = kInvalidIndex;  // Index into this Skin's joints array; kInvalidIndex for a root joint.
        DirectX::XMFLOAT4X4 inverseBindMatrix = { 1.0f, 0.0f, 0.0f, 0.0f,
                                                    0.0f, 1.0f, 0.0f, 0.0f,
                                                    0.0f, 0.0f, 1.0f, 0.0f,
                                                    0.0f, 0.0f, 0.0f, 1.0f };
    };

    struct Skin
    {
        std::string name;
        std::vector<Joint> joints;
    };
}
