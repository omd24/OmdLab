#pragma once

#include "CombatDsl.h"
#include "MoveTable.h"
#include "Asset/Clip.h"
#include "Engine/Collision.h"
#include "Engine/Input.h"

#include <cstdint>
#include <entt.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Game
{
    // Per-entity state-machine state - which of the shared states.combat states this fighter is
    // currently in, how long it's been there, and (while in "Attack") which move is playing and
    // how long into it. String-keyed to match the DSL's own generic state/move names - see
    // FighterState.cpp's own comment for why a small set of hardcoded state-name comparisons is
    // fine here despite the transition executor otherwise being fully generic.
    struct FighterState
    {
        std::string currentState = "Idle";
        uint32_t framesInState = 0;
        // How many frames the current state should last before "animationFinished" reports true
        // - set at the moment a state is entered (Attack: the active move's own startup+active+
        // recovery frame count; HitStun: the attacking move's onHitStun/onBlockStun frame count,
        // captured at the moment of the hit - see Phase 5b). 0 for states with no fixed duration
        // (Idle/Walk/Run/KO), which simply never report animationFinished.
        uint32_t stateDurationFrames = 0;
        std::string activeMoveId; // Non-empty only in "Attack".
        uint32_t framesInMove = 0; // Kept in sync with framesInState today (both reset together on entry/cancel) - see FighterState.cpp.
        // Latched true the first tick this move's hitbox lands a hit, stays true for the rest of
        // the move - a cancel's "hitConfirmed" condition means "this move connected at some
        // point during its active window", not "connected on this exact tick".
        bool moveHitConfirmed = false;
        // Which reaction clip to show while in "HitStun" ("hit_reaction" or "block_hit", picked
        // at the moment of the hit) - HitStun has no MoveDefinition of its own to read a clip
        // from the way "Attack" does, so this is set explicitly when the state is entered.
        std::string hitReactionClip;
    };

    struct Health
    {
        int32_t current = 100;
        int32_t max = 100;
    };

    // Bridges Engine::Hitbox::moveId (an opaque uint32_t) back to whichever MoveTable it indexes
    // into - present on any entity that can carry a Hitbox, a real fighter (pointing at its own
    // CharacterDefinition::moveTable) or the dev collision test rig (pointing at a tiny
    // throwaway one-move table) alike, so hit resolution never branches on "is this a real
    // fighter."
    struct MoveSource
    {
        const MoveTable* moveTable = nullptr;
    };

    // Pure function: does this tick's input select a new move? Checks each MoveDefinition's
    // inputButton field against the latest InputCommand's pressed-this-tick edge, in table
    // order (first match wins, same tie-break rule used for state transitions) - no motion-input
    // sequences yet, just single-button triggers (see MoveTable.h).
    std::optional<std::string> SelectMove(const CharacterDefinition& characterDefinition, const Engine::InputHistory& inputHistory);

    // Everything UpdateFighterState needs for one entity's one tick. thisTickInput/
    // lastCollisionEvents are the same values already produced elsewhere in the tick loop, just
    // threaded through rather than re-derived. lastCollisionEvents is deliberately the PREVIOUS
    // tick's result (see the call site in main.cpp) - reacting to it and updating this entity's
    // Transform/ClipPlayback/Hitbox here, before Engine::ResolveCollisions runs again, is what
    // keeps a tick's own state changes visible to collision detection on the very next tick
    // rather than one tick further delayed.
    struct FighterUpdateInput
    {
        const Engine::InputCommand& thisTickInput;
        const Engine::InputHistory& inputHistory; // Threaded through to SelectMove - see its own signature.
        const Engine::CollisionEvents& lastCollisionEvents;
        const CombatDsl::CombatFile& sharedStates;
        const CharacterDefinition& characterDefinition;
        const std::vector<Asset::Clip>& availableClips; // Resolves a state/move's animationClip name to a ClipPlayback index.
    };

    // The per-tick state-machine executor - evaluates the current state's transitions (from
    // input.sharedStates) against this entity's FighterState/tick data, switches state on the
    // first true one (file order, first match wins), and applies whatever that state means
    // (movement, clip, hitbox - see FighterState.cpp's own comment on where that meaning lives).
    // entity must already carry FighterState, Health, Transform, and ClipPlayback components.
    void UpdateFighterState(entt::registry& registry, entt::entity entity, const FighterUpdateInput& input);
}
