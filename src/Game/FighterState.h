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
        // Snap (not smoothly interpolated) visual facing - true faces the character's default
        // orientation (CharacterDefinition::facingCorrectionRadians as authored), false adds a
        // 180-degree flip on top of it. Set once per tick from outside UpdateFighterState (see
        // main.cpp's own tick-loop comment), before either fighter's state machine runs, so both
        // the render (main.cpp) and the bone-attached hitbox lookup (ResolveHitboxOffset) read
        // the same value this tick - always faces the opponent, regardless of which way this
        // fighter is currently walking/attacking, so crossups/jumping past an opponent don't
        // leave anyone facing backward.
        bool facingRight = true;
        // Ticks this fighter's clocks stay frozen after a hit connects (impact hitstop /
        // freeze-frame). Set on BOTH fighters the tick a hit lands, from the attacking move's
        // own hitStopFrames. While > 0, UpdateFighterState decrements it and returns early that
        // tick - no transitions evaluated, no frame counters advanced, the pose held on its
        // current frame for both. 0 the rest of the time. Deliberately separate from
        // pushbackRemaining below (which starts easing out only once this reaches 0, giving the
        // standard "freeze, then knockback" order) and untouched by EnterState.
        uint32_t hitStopRemaining = 0;
        // Signed world-X distance still left to apply from a push-back impulse, eased out
        // (a fraction applied per tick, decaying toward zero) rather than an instant teleport -
        // see UpdateFighterState's own comment on why. Set once at the moment HitStun is
        // entered (both for the defender being pushed and the attacker's own smaller recoil),
        // untouched by EnterState so it keeps decaying independent of any state change that
        // follows.
        float pushbackRemaining = 0.0f;
        // Bumped every time this fighter enters a new attack move (fresh from neutral OR via a
        // cancel - see EnterState) - a cheap per-entity "which attack occurrence is this" id, so a
        // defender can tell "a genuinely new hit just landed" apart from "the same still-active
        // hitbox is still overlapping" without caring whether the two hits happen to share a
        // moveId (re-throwing the same move later must still count as new; the SAME move instance
        // staying active across several ticks must not). Snapshotted onto Engine::Hitbox (and from
        // there onto Engine::HitEvent) the moment this fighter's own hitbox is (re)built each tick
        // - a defender compares against THAT copy, not this live field, since by the time a
        // (possibly still-frozen-in-its-own-hitstop) defender gets around to reacting to an older
        // HitEvent, the attacker here may already have moved on to a newer instance; comparing
        // live would then wrongly read every hit as new. See lastHitByAttacker's own comment for
        // the other half of this.
        uint32_t attackInstanceId = 0;
        // The (attacker, attackInstanceId) pair this fighter last actually reacted to as a
        // defender - lets HitStun be re-entered by a genuinely new hit (a combo's second/third hit
        // landing while still playing out the first hit's reaction) while still ignoring the SAME
        // hitbox's own continued overlap (including the hitstop-adjacent re-trigger case - see
        // states.combat's own comment on why HitStun otherwise has no self transition). entt::null
        // never matches a real attacker, so the very first hit always counts as new.
        entt::entity lastHitByAttacker = entt::null;
        uint32_t lastHitByAttackerInstanceId = 0;
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

    // Test/tooling hook: forces this entity directly into "Attack" playing `moveId`, bypassing
    // SelectMove and cancels entirely. Needed for a combo-only follow-up move (one whose input
    // button is only ever reachable via another move's cancel list, by design - SelectMove
    // always resolves a button to the first move in the table that uses it, which is the
    // combo's opener, not its own follow-ups) to be exercised in isolation, e.g. by the capture
    // harness. Does the same bookkeeping a real Attack transition does (frame counters, hitbox
    // window) and nothing more - no health/position/hitstop side effects.
    void ForceEnterAttackMove(entt::registry& registry, entt::entity entity, const CharacterDefinition& characterDefinition, const std::string& moveId);

    // Builds characterDefinition.locomotionBlendSpace from its clips: samples for
    // walk_backward @ -walkSpeed, idle @ 0, walk_forward @ +walkSpeed, running @ +runSpeed
    // (any missing clip is skipped, logged). Call once at startup after the model's clips are
    // loaded. The Idle/Walk/Run states drive their animation from this instead of one clip each.
    void BuildLocomotionBlendSpace(const std::vector<Asset::Clip>& clips, CharacterDefinition& characterDefinition);

    // Builds characterDefinition.jumpFloatMask: a per-node blend weight (uniform today) for the
    // in-place jump - near-1 so the jump is mostly a held calm pose with only a subtle bleed of
    // the clip's own motion. Call once at startup after the model is loaded.
    void BuildJumpFloatMask(const Asset::Model& model, CharacterDefinition& characterDefinition);

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
