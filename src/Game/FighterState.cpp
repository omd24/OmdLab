#include "FighterState.h"

#include "GameConstants.h"
#include "InputBindings.h"
#include "Asset/Model.h"
#include "Engine/Animation.h"
#include "Engine/Animator.h"
#include "Engine/Components.h"
#include "Engine/FixedTimestep.h"
#include "Foundation/Log.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Global, single-stage gravity for Jump's closed-form arc - not per-character/authored,
    // matching the flat-single-stage, one-character-for-now scope. Paired with CharacterStats::
    // jumpSpeed (7.5) for a snappy ~0.69s total airtime, peak height ~1.30 - a fighting-game
    // jump, not the old ~1.67s float. The jump clip is phase-mapped onto this arc (see the
    // Jump branch of the clip path below), so the airtime no longer has to match any clip's
    // own length. Scaled by kCombatSpeedMultiplier so airtime tracks the global clip-speed
    // lever. Eyeball-tuned - adjust this or jumpSpeed to retune (g = 8*peakHeight/airtime^2).
    constexpr float kGravity = 18.0f * Game::kCombatSpeedMultiplier;

    // Total push-back distances, queued into FighterState::pushbackRemaining the tick HitStun
    // is entered and eased out over the next several ticks (see kPushbackDecayPerTick below) -
    // not applied as one instant displacement, which read as an unnatural teleport-style snap
    // when first tried. Eyeball-tuned starting points, same "adjust the constant to change
    // feel" precedent as kGravity above.
    constexpr float kHitPushbackDistance = 0.35f;     // Defender, on a real (unblocked) hit.
    constexpr float kBlockPushbackDistance = 0.45f;   // Defender, on block - a bit more than a hit, standard "pushblock" spacing incentive.
    constexpr float kAttackerRecoilDistance = 0.1f;   // Attacker, same distance regardless of hit/block.
    // Fraction of pushbackRemaining applied each tick - an ease-out (biggest step first,
    // tapering toward zero), not a fixed-duration slide. At 60Hz this covers ~90% of the total
    // distance within about 6 ticks (~100ms) - fast enough to read as a reaction to the hit,
    // not a lingering slide.
    constexpr float kPushbackDecayPerTick = 0.35f;

    int32_t FindClipIndex(const std::vector<Asset::Clip>& clips, const std::string& name)
    {
        for (size_t i = 0; i < clips.size(); ++i)
        {
            if (clips[i].name == name)
            {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }


    float SmoothStep01(float u)
    {
        u = std::clamp(u, 0.0f, 1.0f);
        return u * u * (3.0f - 2.0f * u); // zero slope at both ends
    }
    float SmootherStep01(float u)
    {
        u = std::clamp(u, 0.0f, 1.0f);
        return u * u * u * (u * (u * 6.0f - 15.0f) + 10.0f); // zero slope AND curvature at both ends
    }

    // Reshapes an attack's clip playback from a tick count into a seconds offset, when the move
    // authored a timing curve. THREE segments over the move's own [startup | active | recovery]
    // budget, all measured in the same speed-scaled ticks framesInMove counts in:
    //   1. wind-up  [0, startup)          clip 0 -> windupFraction, smoothstep (settles into the coil)
    //   2. snap     [startup, startup+snap) windupFraction -> contactFraction, smoothstep (explosive but continuous)
    //   3. settle   [.., total)           contactFraction -> 1, smootherstep
    // Every segment boundary meets at ~zero clip-time velocity, so playback never lurches - the
    // old design's hard "hold" segment butting against a power-curve ramp is exactly what made
    // the end of a move stutter. The soft dwell at the strike now comes for free: both the snap
    // and the settle move slowly near contactFraction. The crisp gameplay freeze is hitStop,
    // applied separately (FighterState::hitStopRemaining) - the animation itself never stops.
    // The result is never wrapped/looped; it ends just shy of the clip's final frame and the
    // Attack state exits (animationFinished) before it would matter.
    float WarpedClipSeconds(const Game::MoveDefinition& move, uint32_t framesInMove, float clipDuration)
    {
        const float startup = static_cast<float>(move.startupFrames);
        const float total = static_cast<float>(move.startupFrames + move.activeFrames + move.recoveryFrames);
        float snap = static_cast<float>(std::max<uint32_t>(1u, move.timingSnapFrames));
        if (startup + snap > total - 1.0f)
        {
            snap = std::max(1.0f, total - 1.0f - startup);
        }
        const float windupFraction = std::clamp(move.timingWindupFraction, 0.0f, 1.0f);
        const float contactFraction = std::clamp(move.timingContactFraction, 0.0f, 1.0f);
        const float f = static_cast<float>(framesInMove);

        float fraction;
        if (f < startup && startup > 0.0f)
        {
            fraction = windupFraction * SmoothStep01(f / startup);
        }
        else if (f < startup + snap)
        {
            fraction = windupFraction + (contactFraction - windupFraction) * SmoothStep01((f - startup) / snap);
        }
        else
        {
            const float denom = total - startup - snap;
            const float u = denom > 0.0f ? (f - startup - snap) / denom : 1.0f;
            fraction = contactFraction + (1.0f - contactFraction) * SmootherStep01(u);
        }

        float seconds = std::clamp(fraction, 0.0f, 1.0f) * clipDuration;
        // Same epsilon-back-from-the-end guard SetClip's own non-looping path uses.
        constexpr float kEndEpsilon = 1.0f / 120.0f;
        return std::min(seconds, std::max(0.0f, clipDuration - kEndEpsilon));
    }

    // Sets (not Advance()s) the clip and its playback time directly from a tick-derived frame
    // count - see main.cpp's own comment on why this entity's animation is now driven by the
    // same clock as the combat logic instead of wall-clock deltaSeconds. Every clip plays back
    // kCombatSpeedMultiplier times faster than real elapsed ticks (see that constant's own
    // comment on GameConstants.h) - safe to apply uniformly here since every fixed-duration
    // state's own frame counts are scaled by the same factor at load time (MoveTable.cpp's
    // BuildMoveTable), keeping clip and state duration in sync.
    //
    // The active clip's playback time for a state at `framesElapsed` ticks in. Pure - the
    // Animator (Play/SetTime) does the actual playback + crossfade; this only answers "what
    // second of the clip", kept separate so playback logic never mixes with time selection.
    //
    // attackMove != nullptr AND it authored a timing curve: the linear tick->seconds map is
    // replaced by WarpedClipSeconds (the anticipation / snap / settle reshaping). Passed only
    // from the Attack branch of ApplyStateEffects; nullptr everywhere else = the same linear
    // playback every other state always had. loop=false holds a hair before the clip's final
    // frame instead of wrapping - KO/death shouldn't visibly loop.
    float ComputeClipSeconds(
        const Asset::Clip& clip, uint32_t framesElapsed, bool loop, const Game::MoveDefinition* attackMove)
    {
        const float clipDuration = clip.durationSeconds;

        if (attackMove != nullptr && attackMove->HasTimingCurve() && clipDuration > 0.0f)
        {
            return WarpedClipSeconds(*attackMove, framesElapsed, clipDuration);
        }

        float t = static_cast<float>(framesElapsed) * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds * Game::kCombatSpeedMultiplier;
        if (clipDuration > 0.0f)
        {
            if (loop)
            {
                t = fmodf(t, clipDuration);
            }
            else
            {
                constexpr float kEndEpsilon = 1.0f / 60.0f;
                const float heldT = clipDuration - kEndEpsilon;
                if (t > heldT)
                {
                    t = heldT;
                }
            }
        }
        return t;
    }

    // Crossfade length when a fighter's visible clip changes, keyed on the state it's entering.
    // Reactive states snap in (a flinch shouldn't ease); settling to a locomotion/idle state is
    // the smooth case - and the one that fixes the Attack -> Idle hard cut. In sim seconds.
    float BlendInSeconds(const std::string& targetState)
    {
        if (targetState == "HitStun") return 0.04f;
        if (targetState == "KO") return 0.03f;
        if (targetState == "Attack") return 0.05f;
        if (targetState == "Jump") return 0.06f;
        return 0.10f; // Idle / Walk / Run
    }

    // Bounds on the Attack recovery blend-out's crossfade length. Floor: a move with a very
    // short recovery still eases home rather than cutting. Ceiling: a long authored recovery
    // (kick_light's is ~0.7s) shouldn't spend all of it easing to idle - that reads as a
    // sluggish return. Past the ceiling the crossfade is done and the fighter simply stands in
    // live idle for the rest of the (gameplay) recovery lock - standing, not frozen.
    constexpr float kMinRecoveryBlendSeconds = 0.13f;
    constexpr float kMaxRecoveryBlendSeconds = 0.20f;

    // Advances the locomotion blend space's param + phase by one fixed tick toward
    // targetUnitsPerSecond, sanitising both accumulators first. A single NaN/Inf (a bad input
    // axis, a degenerate blend-space sample) would otherwise feed back through the += / fmod
    // every tick forever - the pose freezes and the mesh renders as a collapsed point, i.e.
    // invisible, until restart (PoseIsFinite is the render-side backstop for the same class of
    // fault). Shared by the Idle/Walk/Run path and the Attack recovery blend-out so the guard
    // can't drift between them.
    void DriveLocomotion(
        Engine::Animator& animator, const Engine::BlendSpace1D& space, const std::vector<Asset::Clip>& clips,
        float targetUnitsPerSecond)
    {
        constexpr float kFixedDt = Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        constexpr float kParamEase = 12.0f * kFixedDt; // < 1: stable fraction-per-tick settle (~0.08s)

        if (!std::isfinite(animator.blendParam))
        {
            animator.blendParam = 0.0f;
        }
        if (!std::isfinite(targetUnitsPerSecond))
        {
            targetUnitsPerSecond = 0.0f;
        }
        animator.blendParam += (targetUnitsPerSecond - animator.blendParam) * kParamEase;

        float rate = Engine::BlendSpace1DPhaseRate(space, animator.blendParam, clips);
        if (!std::isfinite(rate))
        {
            rate = 0.0f;
        }
        float nextPhase = animator.blendPhase01 + rate * kFixedDt;
        if (!std::isfinite(nextPhase))
        {
            nextPhase = 0.0f;
        }
        animator.blendPhase01 = std::fmod(nextPhase, 1.0f);
    }

    // Computes the world-space offset (relative to the entity's own Transform, matching
    // Engine::ComputeWorldAabb's own "position + offset" contract) a hitbox should use this
    // tick. Root-relative (today's original behavior, returns the authored offset unchanged)
    // when no bone is attached; otherwise reads the named joint's own current animated position
    // - via the exact same Engine::EvaluateNodeWorldTransforms call UpdateSkinnedPose already
    // makes once per render frame for the visible mesh (main.cpp), just for one joint's position
    // here instead of the whole skin - harmless duplicate work at this entity count, same
    // precedent as SelectMove already being evaluated twice per tick. The authored offset is
    // still added on top as a small world-axis-aligned refinement, not rotated into the joint's
    // own orientation - CollisionBox itself already ignores rotation everywhere else in this
    // system (see its own header comment), so this isn't a new inconsistency, just the same one.
    // facingRight/baseFacingCorrectionRadians replace a plain CharacterDefinition reference so
    // this reads the CURRENT tick's facing (which can flip mid-fight to face the opponent - see
    // FighterState::facingRight's own comment) rather than a fixed per-character constant. When
    // facing left, box.offset.x is also mirrored (in addition to baseFacingCorrectionRadians
    // gaining +180 degrees below) - a hitbox's small authored "reach further out this way"
    // refinement needs to point the mirrored direction too, or it would keep pushing toward the
    // same absolute world side regardless of which way the character is actually turned.
    // groundingOffsetY: the same world-space Y shift the visible mesh is drawn with (see
    // CharacterDefinition::groundingOffsetY / main.cpp's updateFighterRender) - added to every
    // returned offset here so a bone-attached hitbox drops with the mesh instead of floating at
    // the un-shifted skeleton height. Applied to both the joint-attached and the root-relative
    // fallback path, since the mesh shift is unconditional.
    DirectX::XMFLOAT3 ResolveHitboxOffset(
        const Game::MoveHitboxDef& hitboxDef, float baseFacingCorrectionRadians, bool facingRight, float groundingOffsetY,
        const Asset::Model& model, const std::vector<Asset::Clip>& availableClips, const std::string& clipName, uint32_t framesElapsed,
        const Engine::Transform& entityTransform)
    {
        if (hitboxDef.resolvedJointIndex < 0)
        {
            DirectX::XMFLOAT3 offset = hitboxDef.box.offset;
            if (!facingRight)
            {
                offset.x = -offset.x;
            }
            offset.y += groundingOffsetY;
            return offset;
        }

        const int32_t clipIndex = FindClipIndex(availableClips, clipName);
        if (clipIndex < 0)
        {
            // Shouldn't happen - SetClip already resolved this exact same clip name this same
            // tick. Fail soft rather than crash: root-relative is a worse-but-safe placement.
            DirectX::XMFLOAT3 offset = hitboxDef.box.offset;
            offset.y += groundingOffsetY;
            return offset;
        }
        const Asset::Clip& clip = availableClips[static_cast<size_t>(clipIndex)];
        float t = static_cast<float>(framesElapsed) * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        if (clip.durationSeconds > 0.0f)
        {
            t = fmodf(t, clip.durationSeconds);
        }

        // rootTransform = Identity here on purpose, matching UpdateSkinnedPose's own call.
        // Deliberately NOT multiplying by characterDefinition.assetCorrection (the 100x mesh-
        // vertex-scale fix - see its own comment) - verified empirically (a per-tick log across
        // a whole punch's active window) that this asset's joint/skeleton hierarchy is already
        // in true world-scale units on its own, unlike the mesh's own vertex data. Applying the
        // correction here produced positions ~100x too large (hand height ~110 instead of ~1.1);
        // omitting it lands exactly where a hand should be, and the position visibly tracks
        // smoothly through the punch's whole arc. Plausible cause, not fully root-caused: this
        // asset's skeleton root likely isn't a descendant of the mesh's own scale-wrapper node -
        // common for retargeted rigs where the Armature and mesh are siblings rather than
        // nested - so worth re-verifying if a future character asset needs bone attachment too,
        // rather than assuming this asset's behavior generalizes.
        const std::vector<DirectX::XMMATRIX> nodeWorldTransforms =
            Engine::EvaluateNodeWorldTransforms(model, DirectX::XMMatrixIdentity(), &clip, t);
        // facingCorrectionRadians (unlike assetCorrection's scale, just above) DOES apply here -
        // see its own comment on CharacterDefinition for why: it has to move the skeleton, or a
        // hitbox would visibly detach from the mesh it's meant to track. +180 degrees when
        // facing left - see this function's own header comment.
        const float effectiveFacingRadians = baseFacingCorrectionRadians + (facingRight ? 0.0f : DirectX::XM_PI);
        const DirectX::XMMATRIX facingCorrection = DirectX::XMMatrixRotationY(effectiveFacingRadians);
        const DirectX::XMMATRIX jointToWorld = nodeWorldTransforms[static_cast<size_t>(hitboxDef.resolvedJointIndex)] * facingCorrection *
                                                Engine::ComputeWorldMatrix(entityTransform);
        DirectX::XMFLOAT3 jointWorldPos;
        DirectX::XMStoreFloat3(&jointWorldPos, DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), jointToWorld));

        const float mirroredOffsetX = facingRight ? hitboxDef.box.offset.x : -hitboxDef.box.offset.x;
        // Relative to the entity's own Transform.position - ComputeWorldAabb adds that back on
        // top at collision-test time, the same "offset is relative to the entity" contract a
        // root-relative hitbox already relies on, just computed from the joint's live position
        // instead of a fixed authored number.
        return DirectX::XMFLOAT3{
            jointWorldPos.x - entityTransform.position.x + mirroredOffsetX,
            jointWorldPos.y - entityTransform.position.y + hitboxDef.box.offset.y + groundingOffsetY,
            jointWorldPos.z - entityTransform.position.z + hitboxDef.box.offset.z,
        };
    }

    // Sets up FighterState for having just entered a state - shared by both the states.combat
    // transition path and the in-Attack cancel path below, since a cancel is "leave one move,
    // enter another" without a state change, which needs almost the same bookkeeping as a real
    // transition. moveId is the newly-active move's id, or empty for any non-Attack state.
    void EnterState(Game::FighterState& state, const Game::CharacterDefinition& characterDefinition, const std::string& moveId)
    {
        state.framesInState = 0;
        state.framesInMove = 0;
        state.moveHitConfirmed = false;
        state.activeMoveId = moveId;
        state.stateDurationFrames = 0;
        if (!moveId.empty())
        {
            if (const Game::MoveDefinition* move = characterDefinition.moveTable.FindById(moveId))
            {
                state.stateDurationFrames = move->startupFrames + move->activeFrames + move->recoveryFrames;
            }
        }
    }

    // All state-name-specific *meaning* lives here - the transition-walking loop in
    // UpdateFighterState and the DSL evaluator itself never see or care what "Walk"/"Attack"/
    // "HitStun"/"KO" mean (see FighterState.h's own comment for why that split is deliberate:
    // the design doc explicitly sanctions the executor knowing state names, just not baking that
    // knowledge into the generic parts).
    void ApplyStateEffects(entt::registry& registry, entt::entity entity, const Game::FighterState& state, const Game::CharacterDefinition& characterDefinition, const Engine::InputCommand& input, const std::vector<Asset::Clip>& availableClips, const Asset::Model& characterModel, bool forceShowAllHitboxes)
    {
        Engine::Transform& transform = registry.get<Engine::Transform>(entity);
        Engine::Animator& animator = registry.get<Engine::Animator>(entity);

        // Grounded by default - only Jump's own branch below overrides this. No other state
        // touches position.y, so without this reset, getting interrupted out of Jump early (hit
        // mid-air -> HitStun) would otherwise leave the character stuck at whatever height it
        // was hit at.
        transform.position.y = 0.0f;

        float moveUnitsPerSecond = 0.0f;
        std::string clipName;
        // Idle/Walk/Run drive their animation from the locomotion blend space (keyed on signed
        // movement speed), not a single clip. The other states set clipName and go through the
        // single-clip path further down.
        bool locomotion = false;
        // Set by the Attack branch once a move's strike motion (startup + active) is spent: the
        // attack clip stops being sampled and the pose crossfades to idle across the remaining
        // recovery - see the animator-drive section below.
        bool recoveryBlendOut = false;
        const Game::MoveHitboxDef* activeHitbox = nullptr;
        const Game::MoveDefinition* activeMove = nullptr;

        if (state.currentState == "Idle")
        {
            locomotion = true;
        }
        else if (state.currentState == "Walk")
        {
            locomotion = true;
            moveUnitsPerSecond = characterDefinition.stats.walkSpeed * input.axis;
        }
        else if (state.currentState == "Run")
        {
            // Run is a committed forward dash, not conditional on the movement axis at all -
            // holding just the Run button moves forward (matches this being a rushdown
            // mechanic - see the design doc's "Run mechanic confirmed" note), and holding Back
            // doesn't reverse it. A backward run was tried and dropped for now - the clip shows
            // the character's back to the camera, which reads wrong for this side-view game.
            locomotion = true;
            moveUnitsPerSecond = characterDefinition.stats.runSpeed;
        }
        else if (state.currentState == "Jump")
        {
            clipName = state.jumpClip.empty() ? "jump_in_place" : state.jumpClip;
            moveUnitsPerSecond = state.jumpHorizontalSpeed;
            const float t = static_cast<float>(state.framesInState) * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
            // Closed-form parabola, evaluated directly from framesInState each tick (same
            // pattern SetClip already uses) - the max(0, ...) clamp is the entire "don't go
            // through the floor" mechanism, valid because the stage is flat.
            transform.position.y = std::max(0.0f, characterDefinition.stats.jumpSpeed * t - 0.5f * kGravity * t * t);
        }
        else if (state.currentState == "Attack")
        {
            activeMove = characterDefinition.moveTable.FindById(state.activeMoveId);
            if (activeMove != nullptr)
            {
                clipName = activeMove->animationClip;
                // Forward lunge: a decelerating step that covers lungeDistance over the
                // startup -> contact window and hits zero speed exactly at the strike, so the
                // attacker plants rather than sliding through. Inert (moveUnitsPerSecond stays 0,
                // the long-standing "moves are stationary" behavior) when lungeDistance == 0.
                if (activeMove->lungeDistance != 0.0f)
                {
                    const uint32_t lungeWindowFrames =
                        activeMove->startupFrames + std::max<uint32_t>(1u, activeMove->timingSnapFrames);
                    if (state.framesInMove < lungeWindowFrames)
                    {
                        const float windowSeconds =
                            static_cast<float>(lungeWindowFrames) * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
                        const float elapsedSeconds =
                            static_cast<float>(state.framesInMove) * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
                        // v(t) = (2D/T)(1 - t/T): integrates to exactly D over [0, T], linear decel to 0 at T.
                        const float peakSpeed = windowSeconds > 0.0f ? (2.0f * activeMove->lungeDistance / windowSeconds) : 0.0f;
                        const float forwardSign = state.facingRight ? 1.0f : -1.0f;
                        moveUnitsPerSecond = forwardSign * peakSpeed * (1.0f - elapsedSeconds / windowSeconds);
                    }
                }
                for (const Game::MoveHitboxDef& hitboxDef : activeMove->hitboxes)
                {
                    // forceShowAllHitboxes bypasses the authored active-frame window (debug aid
                    // only - see FighterUpdateInput's own comment), showing the box for the
                    // state's whole duration so its bone-tracking can be watched continuously.
                    if (forceShowAllHitboxes || (state.framesInMove >= hitboxDef.frameStart && state.framesInMove < hitboxDef.frameEnd))
                    {
                        activeHitbox = &hitboxDef;
                        break;
                    }
                }

                // Hand the pose off to a smooth crossfade-to-idle (the "recovery blend-out")
                // right after the strike LANDS - contact is ~startup + snap - not at the end of
                // the authored active window. The few frames between contact and active-end are
                // the warped clip playing its OWN retract, which (especially with a short
                // recovery) yanks the limb back unnaturally fast; letting the blend-out own the
                // entire return keeps it smooth and lets the state end promptly so the player
                // isn't input-locked watching a recovery animation. The hitbox window is
                // unchanged (checked above independently) - it just tracks a pose that's already
                // easing home over its last frame or two. Needs a locomotion blend space to
                // fade toward - every real character has one. Falls back to active-end for a
                // move with no timing curve.
                uint32_t motionFrames = activeMove->startupFrames + activeMove->activeFrames;
                if (activeMove->HasTimingCurve())
                {
                    // Hand off exactly at contact (end of the snap segment). Not a frame later:
                    // the warp curve's settle segment, compressed by a short recovery, races the
                    // clip's own retract and the handoff snaps the limb back hard.
                    motionFrames = activeMove->startupFrames + std::max<uint32_t>(1u, activeMove->timingSnapFrames);
                }
                if (state.framesInMove >= motionFrames && state.stateDurationFrames > motionFrames &&
                    !characterDefinition.locomotionBlendSpace.samples.empty())
                {
                    recoveryBlendOut = true;
                }
            }
        }
        else if (state.currentState == "HitStun")
        {
            clipName = state.hitReactionClip.empty() ? "hit_reaction" : state.hitReactionClip;
        }
        else if (state.currentState == "KO")
        {
            clipName = "ko";
            // Settle-to-ground ramp. The retargeted "ko" clip keeps its root joint at standing
            // hip height through the whole fall, so without this the body finishes lying in
            // mid-air. Ease position.y from 0 to koDropOffsetY across framesInState in
            // [koDropStartFrame, koDropEndFrame] (both already speed-scaled at load), then hold
            // there - set directly like Jump's own position.y, overriding the grounded-by-
            // default reset above. koDropOffsetY == 0 leaves KO untouched; an empty/inverted
            // window degrades to an instant step at koDropStartFrame.
            if (characterDefinition.koDropOffsetY != 0.0f)
            {
                const uint32_t f = state.framesInState;
                float progress = 1.0f;
                if (f <= characterDefinition.koDropStartFrame)
                {
                    progress = 0.0f;
                }
                else if (f < characterDefinition.koDropEndFrame)
                {
                    const float span =
                        static_cast<float>(characterDefinition.koDropEndFrame - characterDefinition.koDropStartFrame);
                    const float x = static_cast<float>(f - characterDefinition.koDropStartFrame) / span;
                    progress = x * x * (3.0f - 2.0f * x); // smoothstep
                }
                transform.position.y = characterDefinition.koDropOffsetY * progress;
            }
        }

        // A non-finite speed here would poison transform.position permanently (a NaN world
        // matrix renders nothing, and position is only ever accumulated, never recomputed) -
        // this cannot happen from any known path, but the failure is severe and silent, so
        // clamp it at the one point it would enter.
        if (!std::isfinite(moveUnitsPerSecond))
        {
            moveUnitsPerSecond = 0.0f;
        }
        if (moveUnitsPerSecond != 0.0f)
        {
            transform.position.x += moveUnitsPerSecond * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        }
        // Stage bounds - applied once here regardless of which state above actually moved the
        // character (Walk/Run/Jump all funnel through moveUnitsPerSecond), same "one clamp,
        // not one per state" reasoning as Jump's own Y-ground clamp. kStageMovementHalfWidth is
        // derived from the ground plane's own kStageHalfWidth (GroundPlane.cpp) minus a wall
        // margin, so the visible floor and this clamp stay tied to one stage-size constant while
        // still keeping the whole fighter body on the floor - see GameConstants.h.
        transform.position.x = std::clamp(transform.position.x, -Game::kStageMovementHalfWidth, Game::kStageMovementHalfWidth);
        if (!std::isfinite(transform.position.x))
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Error, "FighterState", "non-finite transform.position.x - reset to 0 (state '%s')",
                state.currentState.c_str());
            transform.position.x = 0.0f;
        }

        // Masked overlay is opt-in per tick (only the in-place jump sets it, below) - clear it
        // here so it never lingers into another state.
        animator.overlayClipIndex = -1;
        animator.overlayMask = nullptr;
        animator.overlayWeight = 1.0f;

        if (locomotion)
        {
            // Blend space instead of a single clip. PlayBlendSpace() crossfades on entry
            // (no-op once we're on it); blendParam / blendPhase01 are driven by DriveLocomotion,
            // which also sanitises them. Easing the param (rather than snapping) is what keeps a
            // released stick from jumping walk-pose -> idle-pose in one tick, and keeps the
            // Walk<->Idle<->Run changes soft (they don't re-arm the crossfade - same source).
            const Engine::BlendSpace1D& space = characterDefinition.locomotionBlendSpace;
            animator.PlayBlendSpace(characterModel, availableClips, &space, BlendInSeconds(state.currentState));
            DriveLocomotion(animator, space, availableClips, moveUnitsPerSecond);
        }
        else if (recoveryBlendOut)
        {
            // Crossfade the leftover attack pose to idle across the rest of the recovery window,
            // finishing right as the Attack state exits. PlayBlendSpace bakes the current pose
            // into prevPose on this first tick and no-ops afterwards; Animator::Advance burns
            // the weight. Target speed 0 -> the idle pose; phase keeps ticking so idle isn't
            // frozen mid-fade. When the state finally flips to Idle its own PlayBlendSpace call
            // is a same-space no-op and the fade is already complete.
            const Engine::BlendSpace1D& space = characterDefinition.locomotionBlendSpace;
            const float recoverySeconds = static_cast<float>(state.stateDurationFrames - state.framesInMove) *
                                            Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
            const float blendSeconds =
                std::clamp(recoverySeconds, kMinRecoveryBlendSeconds, kMaxRecoveryBlendSeconds);
            animator.PlayBlendSpace(characterModel, availableClips, &space, blendSeconds);
            DriveLocomotion(animator, space, availableClips, 0.0f);
        }
        else if (!clipName.empty())
        {
            const int32_t clipIndex = FindClipIndex(availableClips, clipName);
            if (clipIndex >= 0)
            {
                const Asset::Clip& clip = availableClips[static_cast<size_t>(clipIndex)];
                // Jump maps its clip onto the physical arc: one full playthrough (no loop) over
                // the airtime, so takeoff -> apex -> land line up with the parabola whatever the
                // clip's own length is - the retargeted jump clips run 0.7s to 1.97s, so a
                // fixed-rate playback either loops a short one mid-air or hangs on a long one.
                // KO holds its last frame; everything else (Attack/HitStun) ends via
                // animationFinished before its clip would wrap, so it loops harmlessly.
                const bool jumpArc = state.currentState == "Jump" && state.stateDurationFrames > 0;
                const bool loopClip = state.currentState != "KO" && !jumpArc;
                animator.PlayClip(characterModel, availableClips, clipIndex, loopClip, BlendInSeconds(state.currentState));
                if (jumpArc && state.jumpClip == "jump_in_place")
                {
                    // A neutral jump is a HELD pose plus one smooth sway - never the clip
                    // playing back (which, sped onto the short arc, loops the arms and head
                    // rapidly). Base = an early frame of the jump clip, held. Overlay
                    // = a slightly later frame of the SAME clip, held, blended in by a single
                    // sine hump over the airtime (0 at takeoff -> peak at apex -> 0 on landing).
                    // The result morphs once between two frames of the jump and back - a subtle
                    // rise-and-settle of the whole body, no oscillation. Directional jumps use
                    // short clips and phase-map fine (below).
                    const float phase = std::clamp(
                        static_cast<float>(state.framesInState) / static_cast<float>(state.stateDurationFrames), 0.0f, 1.0f);
                    constexpr float kHeldFraction = 0.10f;
                    constexpr float kSwayToFraction = 0.35f;
                    constexpr float kSwayAmount = 0.6f;
                    animator.SetTime(kHeldFraction * clip.durationSeconds);
                    if (!characterDefinition.jumpFloatMask.empty())
                    {
                        animator.overlayClipIndex = clipIndex; // same clip, a later held frame
                        animator.overlayClipSeconds = kSwayToFraction * clip.durationSeconds;
                        animator.overlayMask = &characterDefinition.jumpFloatMask;
                        animator.overlayWeight = std::sin(phase * DirectX::XM_PI) * kSwayAmount;
                    }
                }
                else if (jumpArc)
                {
                    // Phase-map the clip onto the arc: one playthrough over the airtime.
                    const float phase = std::clamp(
                        static_cast<float>(state.framesInState) / static_cast<float>(state.stateDurationFrames), 0.0f, 1.0f);
                    animator.SetTime(phase * clip.durationSeconds);
                }
                else
                {
                    animator.SetTime(ComputeClipSeconds(clip, state.framesInState, loopClip, activeMove));
                }
            }
        }

        // Hitbox attach/detach - only ever present while a move's authored window says so. The
        // move's stable table index (not a hash) is what Engine::Hitbox::moveId carries, per the
        // MoveSource bridge (see FighterState.h) hit resolution reads it back through.
        if (activeHitbox != nullptr && activeMove != nullptr)
        {
            const int32_t moveIndex = characterDefinition.moveTable.IndexOf(activeMove->id);
            Engine::CollisionBox worldBox = activeHitbox->box;
            worldBox.offset = ResolveHitboxOffset(
                *activeHitbox, characterDefinition.facingCorrectionRadians, state.facingRight, characterDefinition.groundingOffsetY,
                characterModel, availableClips, activeMove->animationClip, state.framesInState, transform);
            registry.emplace_or_replace<Engine::Hitbox>(entity, Engine::Hitbox{ worldBox, static_cast<uint32_t>(moveIndex) });
        }
        else if (registry.all_of<Engine::Hitbox>(entity))
        {
            registry.remove<Engine::Hitbox>(entity);
        }
    }
}

namespace Game
{
    void BuildLocomotionBlendSpace(const std::vector<Asset::Clip>& clips, CharacterDefinition& characterDefinition)
    {
        const float walk = characterDefinition.stats.walkSpeed;
        const float run = characterDefinition.stats.runSpeed;
        const struct
        {
            const char* clip;
            float param;
        } wanted[] = {
            { "walk_backward", -walk },
            { "idle", 0.0f },
            { "walk_forward", walk },
            { "running", run },
        };

        Engine::BlendSpace1D& space = characterDefinition.locomotionBlendSpace;
        space.samples.clear();
        for (const auto& w : wanted)
        {
            const int32_t index = FindClipIndex(clips, w.clip);
            if (index < 0)
            {
                Foundation::Log::Write(
                    Foundation::Log::Severity::Warning, "FighterState", "locomotion blend space: clip '%s' not found - skipped", w.clip);
                continue;
            }
            space.samples.push_back(Engine::BlendSpace1DSample{ index, w.param });
        }
        std::sort(
            space.samples.begin(), space.samples.end(),
            [](const Engine::BlendSpace1DSample& a, const Engine::BlendSpace1DSample& b) { return a.param < b.param; });

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "FighterState", "locomotion blend space: %zu sample(s) over [%.2f, %.2f]",
            space.samples.size(), space.samples.empty() ? 0.0f : space.samples.front().param,
            space.samples.empty() ? 0.0f : space.samples.back().param);
    }

    void BuildJumpFloatMask(const Asset::Model& model, CharacterDefinition& characterDefinition)
    {
        // Which nodes take part in the in-place jump's sway overlay - all of them, uniformly;
        // the per-frame amount is Animator::overlayWeight (a sine hump over the airtime), not
        // baked here. A per-node vector rather than a bare flag so the sway can be shaped
        // per-body-part later (e.g. calmer legs) without an API change.
        characterDefinition.jumpFloatMask.assign(model.nodes.size(), 1.0f);
        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "FighterState", "jump float mask: %zu node(s)", model.nodes.size());
    }

    std::optional<std::string> SelectMove(const CharacterDefinition& characterDefinition, const Engine::InputHistory& inputHistory)
    {
        const Engine::InputCommand& latest = inputHistory.Latest();
        for (const MoveDefinition& move : characterDefinition.moveTable.moves)
        {
            const std::optional<FighterButton> button = ParseFighterButtonName(move.inputButton);
            if (!button.has_value())
            {
                continue;
            }
            const Engine::ButtonState& buttonState = latest.buttons[static_cast<size_t>(*button)];
            if (buttonState.pressedThisTick)
            {
                return move.id;
            }
        }
        return std::nullopt;
    }

    void UpdateFighterState(entt::registry& registry, entt::entity entity, const FighterUpdateInput& input)
    {
        FighterState& state = registry.get<FighterState>(entity);

        // Impact hitstop: while frozen, this fighter's whole tick is a no-op except for burning
        // down the counter. Every clock stays put - framesInState/framesInMove don't advance,
        // transitions aren't evaluated, ApplyStateEffects doesn't run (so the pose, the hitbox,
        // and the Transform all hold exactly where last tick left them), and pushbackRemaining's
        // ease-out is paused too, so knockback begins only once the freeze lifts. Set on both
        // fighters together at the moment of the hit (see the HitStun-entry block below).
        if (state.hitStopRemaining > 0)
        {
            --state.hitStopRemaining;
            return;
        }

        // Attacker-side bookkeeping for cancels: did THIS entity's hitbox land a hit last tick?
        // Latched, not tick-local (see FighterState.h's own comment on moveHitConfirmed) - reset
        // whenever a move/cancel is (re-)entered via EnterState.
        if (state.currentState == "Attack" && !state.moveHitConfirmed)
        {
            for (const Engine::HitEvent& hit : input.lastCollisionEvents.hits)
            {
                if (hit.attacker == entity)
                {
                    state.moveHitConfirmed = true;
                    break;
                }
            }
        }

        // Defender-side: was I hit last tick, and by which move? Only the first such hit this
        // tick is handled - simultaneous multi-attacker hits aren't modeled yet (nothing to model
        // them against before a second real fighter exists). Looked up via MoveSource (see
        // FighterState.h) rather than assuming the attacker is any particular kind of entity.
        bool wasHitThisTick = false;
        const MoveDefinition* attackingMove = nullptr;
        // Only ever read once attackingMove is non-null (see the HitStun-entry push-back below),
        // which only happens once hit.attacker has already resolved a real MoveSource just below -
        // always a valid, currently-registered entity by construction.
        entt::entity attackingEntity = entt::null;
        for (const Engine::HitEvent& hit : input.lastCollisionEvents.hits)
        {
            if (hit.defender == entity)
            {
                wasHitThisTick = true;
                attackingEntity = hit.attacker;
                if (const MoveSource* attackerMoveSource = registry.try_get<MoveSource>(hit.attacker))
                {
                    if (attackerMoveSource->moveTable != nullptr &&
                        hit.moveId < static_cast<uint32_t>(attackerMoveSource->moveTable->moves.size()))
                    {
                        attackingMove = &attackerMoveSource->moveTable->moves[hit.moveId];
                    }
                }
                break;
            }
        }

        const std::optional<std::string> selectedMoveId = SelectMove(input.characterDefinition, input.inputHistory);

        const CombatDsl::StateDecl* currentStateDecl = nullptr;
        for (const CombatDsl::StateDecl& decl : input.sharedStates.states)
        {
            if (decl.name == state.currentState)
            {
                currentStateDecl = &decl;
                break;
            }
        }

        bool stateChanged = false;
        if (currentStateDecl != nullptr)
        {
            constexpr float kAxisDeadzone = 0.1f;
            CombatDsl::EvaluationContext context;
            context.getFlag = [&](const std::string& name) -> bool
            {
                if (name == "moveAxisNonZero") return std::fabs(input.thisTickInput.axis) > kAxisDeadzone;
                if (name == "runHeld") return input.thisTickInput.buttons[static_cast<size_t>(FighterButton::Run)].held;
                if (name == "jumpPressed") return input.thisTickInput.buttons[static_cast<size_t>(FighterButton::Jump)].pressedThisTick;
                if (name == "moveSelected") return selectedMoveId.has_value();
                if (name == "wasHit") return wasHitThisTick;
                if (name == "healthDepleted") return registry.get<Health>(entity).current <= 0;
                // True once a fixed-duration state (Attack: the active move's own frame count;
                // HitStun: the attacking move's stun frames, set at hit time) has run its course.
                // stateDurationFrames == 0 means "no fixed duration" (Idle/Walk/Run/KO), which
                // never reports finished - avoids a state with no authored duration accidentally
                // satisfying framesInState >= 0 on its very first tick.
                if (name == "animationFinished") return state.stateDurationFrames > 0 && state.framesInState >= state.stateDurationFrames;
                return false;
            };
            context.getNumber = [&state](const std::string& name) -> float
            {
                if (name == "frame") return static_cast<float>(state.framesInState);
                return 0.0f;
            };

            for (const CombatDsl::Transition& transition : currentStateDecl->transitions)
            {
                if (CombatDsl::EvaluateCondition(*transition.condition, context))
                {
                    // Captured before overwriting currentState - "was I blocking" needs to know
                    // what I was doing at the moment I got hit, and "cannot block while running"
                    // is defined in terms of that previous state, not whatever we're about to
                    // become.
                    const std::string previousState = state.currentState;
                    state.currentState = transition.toState;

                    if (state.currentState == "Attack")
                    {
                        EnterState(state, input.characterDefinition, selectedMoveId.value_or(""));
                    }
                    else if (state.currentState == "Jump")
                    {
                        EnterState(state, input.characterDefinition, "");
                        // Direction locked at takeoff from the movement axis's sign at this exact
                        // instant, no mid-air steering afterward - see FighterState.h's own
                        // comment on jumpHorizontalSpeed.
                        const float axis = input.thisTickInput.axis;
                        const float walkSpeed = input.characterDefinition.stats.walkSpeed;
                        if (axis > kAxisDeadzone)
                        {
                            state.jumpClip = "jump_forward";
                            state.jumpHorizontalSpeed = walkSpeed;
                        }
                        else if (axis < -kAxisDeadzone)
                        {
                            state.jumpClip = "jump_backward";
                            state.jumpHorizontalSpeed = -walkSpeed;
                        }
                        else
                        {
                            state.jumpClip = "jump_in_place";
                            state.jumpHorizontalSpeed = 0.0f;
                        }
                        // Total airtime from the closed-form parabola's own root - precomputed
                        // once here exactly the way Attack/HitStun precompute their own
                        // stateDurationFrames, so landing falls out of the existing
                        // animationFinished flag for free.
                        const float jumpSpeed = input.characterDefinition.stats.jumpSpeed;
                        const float totalAirtimeSeconds = kGravity > 0.0f ? (2.0f * jumpSpeed / kGravity) : 0.0f;
                        state.stateDurationFrames = static_cast<uint32_t>(
                            std::round(totalAirtimeSeconds / Engine::FixedTimestepAccumulator::kFixedDeltaSeconds));
                    }
                    else if (state.currentState == "HitStun" && attackingMove != nullptr)
                    {
                        // Damage/stun-duration/reaction-clip are applied here, as a one-time side
                        // effect of the transition into HitStun actually firing - never from raw
                        // HitEvent presence, which is what keeps a multi-tick-active hitbox from
                        // re-triggering every tick it's still overlapping (states.combat itself
                        // has no wasHit-driven transition inside HitStun's own body, so this
                        // whole block simply can't run again until HitStun is exited).
                        const bool wasBlocking =
                            previousState != "Run" && input.thisTickInput.buttons[static_cast<size_t>(FighterButton::Block)].held;
                        EnterState(state, input.characterDefinition, "");

                        // Impact hitstop - freeze both fighters for the attacking move's authored
                        // count. Set here rather than at HitEvent time so it fires exactly once
                        // per hit (same reasoning as damage/stun just below), and on both hit and
                        // block. The freeze itself is the early-out at the top of this function;
                        // this only arms the counters. EnterState above already reset this
                        // defender's other per-state bookkeeping but deliberately not
                        // hitStopRemaining, so setting it after that call is safe.
                        if (attackingMove->hitStopFrames > 0)
                        {
                            state.hitStopRemaining = attackingMove->hitStopFrames;
                            if (FighterState* attackerState = registry.try_get<FighterState>(attackingEntity))
                            {
                                attackerState->hitStopRemaining = attackingMove->hitStopFrames;
                            }
                        }

                        // Push-back - queues a total distance into pushbackRemaining, eased out
                        // over the next several ticks (see UpdateFighterState's own comment),
                        // rather than an instant displacement. Only applies if the attacker
                        // entity is still around to read a position from (always true in
                        // practice - see attackingEntity's own comment above).
                        const Engine::Transform& defenderTransform = registry.get<Engine::Transform>(entity);
                        if (const Engine::Transform* attackerTransform = registry.try_get<Engine::Transform>(attackingEntity))
                        {
                            // 1D gameplay axis (see GameConstants.h's own comment) - no vector
                            // normalize needed. Direction = which side of the attacker the
                            // defender is currently standing on.
                            const float pushDir = (defenderTransform.position.x >= attackerTransform->position.x) ? 1.0f : -1.0f;
                            const float defenderPushDistance = wasBlocking ? kBlockPushbackDistance : kHitPushbackDistance;
                            state.pushbackRemaining = pushDir * defenderPushDistance;
                            if (FighterState* attackerState = registry.try_get<FighterState>(attackingEntity))
                            {
                                attackerState->pushbackRemaining = -pushDir * kAttackerRecoilDistance;
                            }
                        }

                        if (wasBlocking)
                        {
                            state.stateDurationFrames = attackingMove->onBlockStunFrames;
                            state.hitReactionClip = "block_hit";
                            // No chip damage on block yet - not modeled.
                        }
                        else
                        {
                            Health& health = registry.get<Health>(entity);
                            health.current = std::max(0, health.current - attackingMove->damage);
                            state.stateDurationFrames = attackingMove->onHitStunFrames;
                            state.hitReactionClip = "hit_reaction";
                        }
                    }
                    else
                    {
                        EnterState(state, input.characterDefinition, "");
                    }
                    stateChanged = true;
                    break;
                }
            }
        }

        // Cancels are a same-state (Attack -> Attack) move swap, a different mechanism from a
        // states.combat transition - only checked when the state itself didn't just change, so
        // getting reactively interrupted (HitStun/KO) always takes priority over canceling.
        //
        // A cancel's own DSL conditions (hitConfirmed, frame >= N, ...) only define the WINDOW a
        // cancel is legal in - they never fire it by themselves. The player must also actually
        // press the CANCEL TARGET's own input button on this exact tick - checked directly here,
        // not via SelectMove's result: SelectMove always resolves a button press to the first
        // move in the table that uses it, which is exactly what a combo string wants for
        // entering it fresh from neutral (Punch always starts "punch", not one of its own
        // follow-ups), but is the wrong question mid-combo, where the very same button should
        // resolve to whichever follow-up THIS move's own cancel list names. A three-hit chain on
        // one button (e.g. Punch, Punch, Punch advancing through three different moves) needs
        // the second and third presses read against the CURRENT move's cancels, not against
        // SelectMove's single table-wide answer.
        if (!stateChanged && state.currentState == "Attack")
        {
            if (const MoveDefinition* activeMove = input.characterDefinition.moveTable.FindById(state.activeMoveId))
            {
                CombatDsl::EvaluationContext cancelContext;
                cancelContext.getFlag = [&state](const std::string& name) -> bool
                {
                    if (name == "hitConfirmed") return state.moveHitConfirmed;
                    return false;
                };
                cancelContext.getNumber = [&state](const std::string& name) -> float
                {
                    if (name == "frame") return static_cast<float>(state.framesInMove);
                    return 0.0f;
                };
                for (const CombatDsl::Cancel& cancel : activeMove->cancels)
                {
                    const MoveDefinition* target = input.characterDefinition.moveTable.FindById(cancel.toMoveId);
                    const std::optional<FighterButton> targetButton =
                        target != nullptr ? ParseFighterButtonName(target->inputButton) : std::nullopt;
                    const bool targetButtonPressedThisTick =
                        targetButton.has_value() &&
                        input.thisTickInput.buttons[static_cast<size_t>(*targetButton)].pressedThisTick;
                    if (targetButtonPressedThisTick && CombatDsl::EvaluateCondition(*cancel.condition, cancelContext))
                    {
                        EnterState(state, input.characterDefinition, cancel.toMoveId);
                        break;
                    }
                }
            }
        }

        // Push-back ease-out - applied every tick regardless of current state (an attacker's own
        // recoil should keep sliding out even after Attack's recovery ends, not just while the
        // defender is in HitStun), before ApplyStateEffects so its own existing stage-bounds
        // clamp covers the combined result of this plus whatever this tick's state moves.
        if (state.pushbackRemaining != 0.0f)
        {
            Engine::Transform& pushbackTransform = registry.get<Engine::Transform>(entity);
            const float step = state.pushbackRemaining * kPushbackDecayPerTick;
            pushbackTransform.position.x += step;
            state.pushbackRemaining -= step;
            if (std::fabs(state.pushbackRemaining) < 0.001f)
            {
                state.pushbackRemaining = 0.0f;
            }
        }

        ApplyStateEffects(
            registry, entity, state, input.characterDefinition, input.thisTickInput, input.availableClips, input.characterModel,
            input.forceShowAllHitboxes);

        // Advance the crossfade one sim tick's worth (in sim seconds, so captures are
        // reproducible). Skipped during hitstop by the early-return at the top of this function -
        // both fighters' blends freeze with everything else, which is correct.
        registry.get<Engine::Animator>(entity).Advance(Engine::FixedTimestepAccumulator::kFixedDeltaSeconds);

        ++state.framesInState;
        if (state.currentState == "Attack")
        {
            ++state.framesInMove;
        }
    }

    void ForceEnterAttackMove(
        entt::registry& registry, entt::entity entity, const CharacterDefinition& characterDefinition, const std::string& moveId)
    {
        FighterState& state = registry.get<FighterState>(entity);
        state.currentState = "Attack";
        EnterState(state, characterDefinition, moveId);
    }

    void PreviewMoveHitbox(
        entt::registry& registry, entt::entity entity, const CharacterDefinition& characterDefinition, const std::string& moveId,
        uint32_t frameCounter, const std::vector<Asset::Clip>& availableClips, const Asset::Model& characterModel,
        bool forceShowAllHitboxes)
    {
        // A throwaway, never-stored FighterState - ApplyStateEffects only reads the fields it's
        // given, so this reuses its exact "Attack" branch (clip selection, hitbox attach) without
        // needing a second copy of that logic here, and without touching the entity's real
        // FighterState component at all.
        FighterState previewState;
        previewState.currentState = "Attack";
        previewState.activeMoveId = moveId;
        previewState.framesInState = frameCounter;
        previewState.framesInMove = frameCounter;
        ApplyStateEffects(
            registry, entity, previewState, characterDefinition, Engine::InputCommand{}, availableClips, characterModel,
            forceShowAllHitboxes);
    }
}
