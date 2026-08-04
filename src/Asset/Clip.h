#pragma once

#include "Types.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Asset
{
    enum class AnimationPath
    {
        Translation,
        Rotation,
        Scale,
        // TODO(OM): add a Weights path (morph-target animation) once a source asset actually
        // uses it - the validated stick-figure clip only animates translation/rotation/scale,
        // so the importer currently skips (and logs) any "weights" channel it encounters.
    };

    // One sampled curve, targeting a single node's transform component - not assumed to
    // target a joint specifically, since glTF allows animating any node (a camera, a light,
    // ...). Baked/sampled per-frame keyframes are expected here (see the animation-workflow
    // validation notes on why the control-rig bake produces dense curves) but the importer
    // stores whatever keyframes the source file provides, dense or sparse, without resampling.
    struct AnimationChannel
    {
        int32_t targetNodeIndex = kInvalidIndex;
        AnimationPath path = AnimationPath::Translation;
        std::vector<float> keyTimes;              // Seconds, ascending.
        std::vector<DirectX::XMFLOAT4> keyValues; // xyz used for Translation/Scale, xyzw for Rotation.
    };

    struct Clip
    {
        std::string name;
        float durationSeconds = 0.0f;
        std::vector<AnimationChannel> channels;
    };
}
