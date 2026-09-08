#pragma once

#include "Animation.h"
#include "BlendSpace.h"

#include <cstdint>
#include <vector>

namespace Asset
{
    struct Model;
    struct Clip;
}

namespace Engine
{
    // Per-entity animation playback + crossfade. The active source is EITHER a single clip
    // (clipIndex / clipSeconds) OR a 1-D blend space (locomotion: blendSpace / blendParam /
    // blendPhase01). PlayClip() / PlayBlendSpace() switch between them, capturing the outgoing
    // pose (baked to prevPose) for a short crossfade so transitions ease instead of cut.
    //
    // Division of labour: the caller (Game) drives the active source's time every tick -
    // SetTime() for a clip, blendParam + blendPhase01 for a blend space; this struct does NOT
    // advance them. It only burns the crossfade weight (Advance) and composites
    // (EvaluateAnimator).
    struct Animator
    {
        // --- active source: single clip ---
        std::int32_t clipIndex = 0;
        float clipSeconds = 0.0f;
        bool loop = true;

        // --- active source: blend space (when useBlendSpace) ---
        bool useBlendSpace = false;
        const BlendSpace1D* blendSpace = nullptr; // not owned - points at per-character data
        float blendParam = 0.0f;
        float blendPhase01 = 0.0f;

        // --- crossfade: the previous active source, baked to a frozen pose ---
        Pose prevPose;
        float blendRemaining = 0.0f;
        float blendDuration = 0.0f;

        // --- masked overlay: a second clip that replaces part of the body ---
        // When overlayClipIndex >= 0 and overlayMask is set, EvaluateAnimator masks
        // overlayClip (sampled at overlayClipSeconds) onto the result by per-node weight - the
        // legs keep the active source, the masked subtree comes from the overlay. The caller
        // sets all three every tick it wants an overlay and clears overlayClipIndex to -1
        // otherwise (it is NOT self-clearing). Used today for the in-place jump (calm arms from
        // jump_forward over the vertical hop); the additive hit-flinch layer will reuse it.
        std::int32_t overlayClipIndex = -1;
        float overlayClipSeconds = 0.0f;
        float overlayWeight = 1.0f;                      // per-frame dial over the whole mask (ramp in/out)
        const std::vector<float>* overlayMask = nullptr; // not owned - per-character mask data

        // Switch the active source to a single clip. No-op (beyond adopting newLoop) if it's
        // already the active clip - so calling this every tick with the steady-state clip is
        // cheap. On a real switch with blendSeconds > 0, the current pose is baked into
        // prevPose and crossfaded out over that long; clipSeconds resets to 0.
        void PlayClip(const Asset::Model& model, const std::vector<Asset::Clip>& clips, std::int32_t newClipIndex, bool newLoop, float blendSeconds);

        // Switch the active source to a blend space. No-op if it's already the active space
        // (blendParam/blendPhase01 are set by the caller separately). On a real switch, same
        // bake-and-crossfade as PlayClip; blendPhase01 is NOT reset (stride continuity across
        // leaving and re-entering locomotion).
        void PlayBlendSpace(
            const Asset::Model& model, const std::vector<Asset::Clip>& clips, const BlendSpace1D* space, float blendSeconds);

        void SetTime(float seconds) { clipSeconds = seconds; }

        // Burn the crossfade weight down by dtSeconds of real (sim) time; drop the frozen
        // previous pose when it expires. No-op when not blending.
        void Advance(float dtSeconds);

        bool Blending() const { return blendRemaining > 0.0f && !prevPose.nodes.empty(); }
        // Weight of the ACTIVE source in the crossfade: 0 the instant it blended in, 1 when done.
        float ActiveWeight() const;
    };

    // The animator's pose this frame: the active source (clip or blend space) sampled at its
    // current time/param, or - while blending - that blended with the frozen prevPose by
    // ActiveWeight().
    Pose EvaluateAnimator(const Animator& animator, const Asset::Model& model, const std::vector<Asset::Clip>& clips);
}
