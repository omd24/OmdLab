#include "FighterState.h"

#include "GameConstants.h"
#include "InputBindings.h"
#include "Asset/Model.h"
#include "Engine/Animation.h"
#include "Engine/ClipPlayback.h"
#include "Engine/Components.h"
#include "Engine/FixedTimestep.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Global, single-stage gravity for Jump's closed-form arc - not per-character/authored,
    // matching the flat-single-stage, one-character-for-now scope. Paired with CharacterStats::
    // jumpSpeed's own default so a neutral jump's total airtime lands close to jump_in_place's
    // own ~1.967s as a starting anchor - an eyeball-tuned starting point, adjust either constant
    // to change jump feel.
    constexpr float kGravity = 3.0f;

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

    // Sets (not Advance()s) the clip and its playback time directly from a tick-derived frame
    // count - see main.cpp's own comment on why this entity's animation is now driven by the
    // same clock as the combat logic instead of wall-clock deltaSeconds.
    void SetClip(Engine::ClipPlayback& playback, const std::vector<Asset::Clip>& clips, const std::string& clipName, uint32_t framesElapsed)
    {
        const int32_t clipIndex = FindClipIndex(clips, clipName);
        if (clipIndex < 0)
        {
            return;
        }
        playback.clipIndex = clipIndex;
        float t = static_cast<float>(framesElapsed) * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        const float clipDuration = clips[clipIndex].durationSeconds;
        if (clipDuration > 0.0f)
        {
            t = fmodf(t, clipDuration);
        }
        playback.playbackTimeSeconds = t;
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
    DirectX::XMFLOAT3 ResolveHitboxOffset(
        const Game::MoveHitboxDef& hitboxDef, float baseFacingCorrectionRadians, bool facingRight, const Asset::Model& model,
        const std::vector<Asset::Clip>& availableClips, const std::string& clipName, uint32_t framesElapsed,
        const Engine::Transform& entityTransform)
    {
        if (hitboxDef.resolvedJointIndex < 0)
        {
            DirectX::XMFLOAT3 offset = hitboxDef.box.offset;
            if (!facingRight)
            {
                offset.x = -offset.x;
            }
            return offset;
        }

        const int32_t clipIndex = FindClipIndex(availableClips, clipName);
        if (clipIndex < 0)
        {
            // Shouldn't happen - SetClip already resolved this exact same clip name this same
            // tick. Fail soft rather than crash: root-relative is a worse-but-safe placement.
            return hitboxDef.box.offset;
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
            jointWorldPos.y - entityTransform.position.y + hitboxDef.box.offset.y,
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
        Engine::ClipPlayback& clipPlayback = registry.get<Engine::ClipPlayback>(entity);

        // Grounded by default - only Jump's own branch below overrides this. No other state
        // touches position.y, so without this reset, getting interrupted out of Jump early (hit
        // mid-air -> HitStun) would otherwise leave the character stuck at whatever height it
        // was hit at.
        transform.position.y = 0.0f;

        float moveUnitsPerSecond = 0.0f;
        std::string clipName;
        const Game::MoveHitboxDef* activeHitbox = nullptr;
        const Game::MoveDefinition* activeMove = nullptr;

        if (state.currentState == "Idle")
        {
            clipName = "idle";
        }
        else if (state.currentState == "Walk")
        {
            clipName = input.axis >= 0.0f ? "walk_forward" : "walk_backward";
            moveUnitsPerSecond = characterDefinition.stats.walkSpeed * input.axis;
        }
        else if (state.currentState == "Run")
        {
            // Run is a committed forward dash, not conditional on the movement axis at all -
            // holding just the Run button moves forward (matches this being a rushdown
            // mechanic - see the design doc's "Run mechanic confirmed" note), and holding Back
            // doesn't reverse it. A backward run (run_backward clip, imported but otherwise
            // unused) was tried and dropped for now - the clip shows the character's back to the
            // camera, which reads wrong for this side-view game; holding Back during Run
            // currently has no distinct effect. Backlog idea (not MVP): a deliberate "retreat"
            // mode using this same clip, gated behind a real gameplay cost (e.g. extra damage
            // taken) rather than just re-adding it as a free direction toggle.
            clipName = "running";
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
                // No movement while attacking - moves are stationary for now, matching this
                // phase's scope (no move-authored momentum/lunge distance exists in MoveDefinition
                // yet).
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
            }
        }
        else if (state.currentState == "HitStun")
        {
            clipName = state.hitReactionClip.empty() ? "hit_reaction" : state.hitReactionClip;
        }
        else if (state.currentState == "KO")
        {
            clipName = "ko";
        }

        if (moveUnitsPerSecond != 0.0f)
        {
            transform.position.x += moveUnitsPerSecond * Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        }
        // Stage bounds - applied once here regardless of which state above actually moved the
        // character (Walk/Run/Jump all funnel through moveUnitsPerSecond), same "one clamp,
        // not one per state" reasoning as Jump's own Y-ground clamp. kStageHalfWidth is also
        // what the ground plane's own mesh is sized from (GroundPlane.cpp) - one shared constant
        // so the visible floor and this clamp can't drift out of sync with each other.
        transform.position.x = std::clamp(transform.position.x, -Game::kStageHalfWidth, Game::kStageHalfWidth);
        if (!clipName.empty())
        {
            SetClip(clipPlayback, availableClips, clipName, state.framesInState);
        }

        // Hitbox attach/detach - only ever present while a move's authored window says so. The
        // move's stable table index (not a hash) is what Engine::Hitbox::moveId carries, per the
        // MoveSource bridge (see FighterState.h) hit resolution reads it back through.
        if (activeHitbox != nullptr && activeMove != nullptr)
        {
            const int32_t moveIndex = characterDefinition.moveTable.IndexOf(activeMove->id);
            Engine::CollisionBox worldBox = activeHitbox->box;
            worldBox.offset = ResolveHitboxOffset(
                *activeHitbox, characterDefinition.facingCorrectionRadians, state.facingRight, characterModel, availableClips,
                activeMove->animationClip, state.framesInState, transform);
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
        // press the target move's own input button on this exact tick (selectedMoveId, already
        // computed above the same way a neutral Attack-state entry is) - otherwise a move whose
        // cancel window is open (e.g. hit-confirmed) would auto-execute with no input at all,
        // rather than requiring the real "Punch, then Kick" button sequence a cancel is supposed
        // to be.
        if (!stateChanged && state.currentState == "Attack" && selectedMoveId.has_value())
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
                    if (cancel.toMoveId == *selectedMoveId && CombatDsl::EvaluateCondition(*cancel.condition, cancelContext))
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

        ++state.framesInState;
        if (state.currentState == "Attack")
        {
            ++state.framesInMove;
        }
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
