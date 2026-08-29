#pragma once

namespace Game
{
    // Stage bounds - X is the real gameplay movement axis (walk/run/jump), clamped so fighters
    // can't wander into infinity; Z is purely a visual "how deep is the stage" dimension, no
    // gameplay logic ever reads it (this is a 2D-camera fighting game - the "2D" is the promise
    // no gameplay system needs Z). One shared source for both the ground plane's own mesh extent
    // (GroundPlane.cpp) and the movement clamp (FighterState.cpp), so retuning stage size never
    // means updating two independently-drifting numbers.
    constexpr float kStageHalfWidth = 6.0f;
    constexpr float kStageHalfDepth = 1.5f;

    // Global playback-rate multiplier for every fighter animation clip (FighterState.cpp's
    // SetClip) - the cheapest lever for "animations feel too slow/sluggish" that doesn't need
    // per-clip retiming work in an animation tool. A clip alone playing faster than its combat
    // state's own authored duration would desync (the clip finishing early, then restarting
    // mid-state) - to keep both moving together, MoveTable.cpp's BuildMoveTable scales every
    // move's own frame-count fields (startup/active/recovery/onHitStun/onBlockStun, and by
    // extension each hitbox's derived active-frame window) by 1/this same multiplier at load
    // time, and FighterState.cpp's kGravity is scaled by this multiplier too so Jump's
    // physics-derived airtime shrinks in lockstep (a smaller peak height is an accepted side
    // effect - retune CharacterStats::jumpSpeed upward if jumps read as too low). Eyeball-tuned
    // starting point, same "adjust the constant to change feel" precedent as kGravity/push-back
    // distances elsewhere in this project - raise or lower freely.
    constexpr float kCombatSpeedMultiplier = 1.2f;

    // Half-width of a fighter's solid body for the fighter-vs-fighter separation check
    // (main.cpp's tick loop) - keeps the two fighters from walking through each other. A
    // separate constant, not read from either fighter's own Hurtbox (an offense/defense
    // volume, semantically different from a solid-body collider, and per-state hurtbox
    // profiles may exist later without needing to also mean "how wide is this body for
    // separation purposes"). Matches the hurtbox's own authored X half-extent today
    // (0.35) as a starting point, not because the two are required to agree.
    constexpr float kFighterBodyHalfWidth = 0.35f;

    // Jump height (world Y) at which a fighter's shadow has faded to kShadowMinAlpha - roughly
    // the default character's own peak jump height (jumpSpeed^2 / (2*kGravity), ~1.25 with
    // today's defaults - see FighterState.cpp/MoveTable.h) so a full neutral jump reads as
    // "faded most of the way," not "barely changed." Eyeball-tuned, not exact for every
    // character - retune if a character's jumpSpeed differs a lot from the default.
    constexpr float kShadowFadeMaxHeight = 1.2f;
    // Never fades fully to zero even at/above kShadowFadeMaxHeight - a faint grounding cue
    // stays visible even at peak height.
    constexpr float kShadowMinAlpha = 0.15f;
}
