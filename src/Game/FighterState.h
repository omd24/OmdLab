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

namespace Asset
{
    struct Model;
}

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
        // Which of the three directional jump clips to show while in "Jump" ("jump_forward",
        // "jump_backward", or "jump_in_place") - like hitReactionClip, Jump has no
        // MoveDefinition of its own to read a clip from, so this is resolved once (from the
        // movement axis's sign) at the moment Jump is entered and held fixed for the arc's
        // whole duration - no mid-air steering.
        std::string jumpClip;
        // Horizontal speed (units/second, signed - same "forward" convention as InputCommand::
        // axis) locked in at the moment Jump is entered and reapplied every tick for the rest of
        // the arc, rather than read live from input the way Walk/Run compute their own
        // moveUnitsPerSecond - see jumpClip's own comment for why.
        float jumpHorizontalSpeed = 0.0f;
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
        const Asset::Model& characterModel; // Needed to resolve a bone-attached hitbox's current joint position - see FighterState.cpp.
        // Debug aid: while in Attack, shows the active move's hitbox for the state's whole
        // duration instead of gating it to the move's own authored active-frame window - lets a
        // bone-attached hitbox's tracking be watched continuously (via "Collision debug draw")
        // instead of racing an often sub-200ms real active window. False in every real build path
        // except a dev-tools debug checkbox - never changes hit resolution itself, only whether
        // the box exists on ticks it normally wouldn't.
        bool forceShowAllHitboxes = false;
    };

    // The per-tick state-machine executor - evaluates the current state's transitions (from
    // input.sharedStates) against this entity's FighterState/tick data, switches state on the
    // first true one (file order, first match wins), and applies whatever that state means
    // (movement, clip, hitbox - see FighterState.cpp's own comment on where that meaning lives).
    // entity must already carry FighterState, Health, Transform, and ClipPlayback components.
    void UpdateFighterState(entt::registry& registry, entt::entity entity, const FighterUpdateInput& input);

    // Debug preview only (hitbox authoring/adjustment tooling): drives the entity's
    // clip and one move's hitbox exactly as a real Attack would, from an externally-owned
    // looping frame counter instead of the real per-tick FighterState machine - lets a hitbox's
    // offset/half-extent be tuned live via ImGui sliders (main.cpp, which edits
    // CharacterDefinition::moveTable in place) and watched against the move's own animated pose,
    // without needing real input, a real Attack transition, or a target to hit. Never reads or
    // writes the entity's own FighterState/Health components - the caller must not also call
    // UpdateFighterState for the same entity on the same tick while this is active (see
    // main.cpp's own hitboxTuningMoveIndex). Attaches a real Engine::Hitbox component, same as a
    // real Attack always does, so it's visualized via the existing "Collision debug draw" toggle
    // for free - no new debug-draw code needed.
    void PreviewMoveHitbox(
        entt::registry& registry, entt::entity entity, const CharacterDefinition& characterDefinition, const std::string& moveId,
        uint32_t frameCounter, const std::vector<Asset::Clip>& availableClips, const Asset::Model& characterModel,
        bool forceShowAllHitboxes);
}
