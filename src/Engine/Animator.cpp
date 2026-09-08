#include "Animator.h"

#include "Asset/Clip.h"
#include "Asset/Model.h"
#include "Foundation/Log.h"

#include <algorithm>
#include <utility>

namespace Engine
{
    namespace
    {
        // The active source sampled with no crossfade applied - a clip or the blend space.
        Pose SampleActive(const Animator& a, const Asset::Model& model, const std::vector<Asset::Clip>& clips)
        {
            if (a.useBlendSpace)
            {
                return a.blendSpace != nullptr ? EvaluateBlendSpace1D(*a.blendSpace, a.blendParam, a.blendPhase01, model, clips)
                                               : SampleClipPose(model, nullptr, 0.0f);
            }
            const Asset::Clip* clip =
                (a.clipIndex >= 0 && a.clipIndex < static_cast<std::int32_t>(clips.size())) ? &clips[static_cast<size_t>(a.clipIndex)]
                                                                                            : nullptr;
            return SampleClipPose(model, clip, a.clipSeconds);
        }
    }

    void Animator::PlayClip(
        const Asset::Model& model, const std::vector<Asset::Clip>& clips, std::int32_t newClipIndex, bool newLoop, float blendSeconds)
    {
        if (!useBlendSpace && newClipIndex == clipIndex)
        {
            loop = newLoop;
            return;
        }

        if (blendSeconds > 0.0f)
        {
            prevPose = SampleActive(*this, model, clips);
            blendDuration = blendSeconds;
            blendRemaining = blendSeconds;
        }
        else
        {
            prevPose.nodes.clear();
            blendDuration = 0.0f;
            blendRemaining = 0.0f;
        }

        useBlendSpace = false;
        blendSpace = nullptr;
        clipIndex = newClipIndex;
        loop = newLoop;
        clipSeconds = 0.0f;
    }

    void Animator::PlayBlendSpace(
        const Asset::Model& model, const std::vector<Asset::Clip>& clips, const BlendSpace1D* space, float blendSeconds)
    {
        if (useBlendSpace && space == blendSpace)
        {
            return;
        }

        if (blendSeconds > 0.0f)
        {
            prevPose = SampleActive(*this, model, clips);
            blendDuration = blendSeconds;
            blendRemaining = blendSeconds;
        }
        else
        {
            prevPose.nodes.clear();
            blendDuration = 0.0f;
            blendRemaining = 0.0f;
        }

        useBlendSpace = true;
        blendSpace = space;
        // blendPhase01 deliberately not reset - stride continuity.
    }

    void Animator::Advance(float dtSeconds)
    {
        if (blendRemaining <= 0.0f)
        {
            return;
        }
        blendRemaining -= dtSeconds;
        if (blendRemaining <= 0.0f)
        {
            blendRemaining = 0.0f;
            prevPose.nodes.clear();
        }
    }

    float Animator::ActiveWeight() const
    {
        if (blendDuration <= 0.0f)
        {
            return 1.0f;
        }
        const float linear = std::clamp(1.0f - blendRemaining / blendDuration, 0.0f, 1.0f);
        // Ease in AND out (zero slope at both ends) rather than a straight ramp - the incoming
        // pose accelerates off the outgoing one and decelerates into its target. A linear
        // crossfade reads as mechanical, especially on a longer blend like an attack's
        // recovery return, where the limb should settle into rest, not slide to it at
        // constant speed.
        return linear * linear * linear * (linear * (linear * 6.0f - 15.0f) + 10.0f); // smootherstep
    }

    Pose EvaluateAnimator(const Animator& animator, const Asset::Model& model, const std::vector<Asset::Clip>& clips)
    {
        Pose activePose = SampleActive(animator, model, clips);
        Pose result =
            animator.Blending() ? Blend(animator.prevPose, activePose, animator.ActiveWeight()) : std::move(activePose);

        // Masked overlay: a second clip replaces the mask's subtree of the body (e.g. calm
        // arms over an otherwise-frantic jump clip). Applied after the crossfade so a fighter
        // blending into an overlaid state still gets the overlay on this frame's result.
        if (animator.overlayClipIndex >= 0 && animator.overlayClipIndex < static_cast<std::int32_t>(clips.size()) &&
            animator.overlayMask != nullptr && !animator.overlayMask->empty())
        {
            const Pose overlayPose =
                SampleClipPose(model, &clips[static_cast<size_t>(animator.overlayClipIndex)], animator.overlayClipSeconds);
            result = BlendMasked(result, overlayPose, *animator.overlayMask, animator.overlayWeight);
        }

        // Last line of defence against a non-finite pose reaching skinning, which would render
        // the mesh as a collapsed point (i.e. invisible) and, if the bad value feeds an
        // accumulator, keep it that way. Fall back to the rest pose for this frame and log at
        // Error: this should never fire, so if it does it is a real upstream defect (degenerate
        // clip quaternion, poisoned blendParam / blendPhase01, garbage crossfade source) and
        // the log is warranted. The upstream accumulators are sanitised at their source (the
        // locomotion / recovery-blend paths in FighterState.cpp), so a single bad frame here
        // recovers on the next.
        if (!PoseIsFinite(result))
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Error, "Animator",
                "non-finite pose (useBlendSpace=%d clipIndex=%d blendParam=%.4f blendPhase01=%.4f activeWeight=%.3f) - rest pose substituted",
                animator.useBlendSpace ? 1 : 0, animator.clipIndex, animator.blendParam, animator.blendPhase01, animator.ActiveWeight());
            return SampleClipPose(model, nullptr, 0.0f);
        }
        return result;
    }
}
