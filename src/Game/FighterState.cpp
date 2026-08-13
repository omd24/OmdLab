#include "FighterState.h"

#include "GameConstants.h"
#include "InputBindings.h"
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
    void ApplyStateEffects(entt::registry& registry, entt::entity entity, const Game::FighterState& state, const Game::CharacterDefinition& characterDefinition, const Engine::InputCommand& input, const std::vector<Asset::Clip>& availableClips)
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
            clipName = "running";
            moveUnitsPerSecond = characterDefinition.stats.runSpeed * input.axis;
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
                    if (state.framesInMove >= hitboxDef.frameStart && state.framesInMove < hitboxDef.frameEnd)
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
            registry.emplace_or_replace<Engine::Hitbox>(entity, Engine::Hitbox{ activeHitbox->box, static_cast<uint32_t>(moveIndex) });
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
        for (const Engine::HitEvent& hit : input.lastCollisionEvents.hits)
        {
            if (hit.defender == entity)
            {
                wasHitThisTick = true;
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

        ApplyStateEffects(registry, entity, state, input.characterDefinition, input.thisTickInput, input.availableClips);

        ++state.framesInState;
        if (state.currentState == "Attack")
        {
            ++state.framesInMove;
        }
    }
}
