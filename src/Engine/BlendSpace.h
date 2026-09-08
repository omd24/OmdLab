#pragma once

#include <cstdint>
#include <vector>

namespace Asset
{
    struct Model;
    struct Clip;
}

namespace Engine
{
    struct Pose;

    // One clip pinned at a position along a 1-D blend axis (e.g. signed movement speed:
    // walk_backward at -walkSpeed, idle at 0, walk_forward at +walkSpeed, running at +runSpeed).
    struct BlendSpace1DSample
    {
        std::int32_t clipIndex = -1;
        float param = 0.0f;
    };

    // A 1-D blend space: sample poses interpolated along one continuous parameter. `samples`
    // MUST be sorted by `param` ascending. Used for locomotion (idle/walk/run by speed).
    struct BlendSpace1D
    {
        std::vector<BlendSpace1DSample> samples;
    };

    // Pose at `param`: find the two samples bracketing it (clamped to the ends) and Blend their
    // poses by the [0,1] position between them. Every sample clip is evaluated at
    // phase01 * clipDuration - one shared normalized cycle position - so their step cycles stay
    // phase-locked across the blend and feet don't skate. Empty space -> bind pose.
    Pose EvaluateBlendSpace1D(
        const BlendSpace1D& space, float param, float phase01, const Asset::Model& model, const std::vector<Asset::Clip>& clips);

    // How fast the shared phase should advance right now, in cycles/second: the bracket-
    // interpolated mean of the two bracketing samples' 1/duration. Caller advances:
    // phase01 = fmod(phase01 + rate * dt, 1).
    float BlendSpace1DPhaseRate(const BlendSpace1D& space, float param, const std::vector<Asset::Clip>& clips);
}
