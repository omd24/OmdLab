#pragma once

namespace Game
{
    // Stage bounds - X is the real gameplay movement axis (walk/run/jump); Z is purely a visual
    // "how deep is the stage" dimension, no gameplay logic ever reads it (this is a 2D-camera
    // fighting game - the "2D" is the promise no gameplay system needs Z). kStageHalfWidth is
    // the visible ground plane's own mesh extent (GroundPlane.cpp).
    constexpr float kStageHalfWidth = 6.0f;
    constexpr float kStageHalfDepth = 1.5f;

    // Movement is clamped a margin short of the plane edge, not to it: a fighter stopped exactly
    // at kStageHalfWidth hangs its (wider-than-a-point) body out over the void past the floor.
    // The leftover strip of ground beyond the wall reads as an intentional stage edge, same as a
    // real fighting stage. Eyeball-tuned - raise kStageWallMargin if the body still overhangs,
    // lower it for more usable floor.
    constexpr float kStageWallMargin = 0.5f;
    constexpr float kStageMovementHalfWidth = kStageHalfWidth - kStageWallMargin;
    // TODO(OM): treat the walls as real collision geometry. Movement is clamped on the root
    // position only, so a fighter's limbs (and, mid-move, much of the body) can visibly pass
    // through the boundary. Proper handling needs wall geometry plus clamping against the
    // body's extents (hurtbox / skeleton bounds) instead of the root point.

    // Which fighter is on the left is only re-decided when the two X positions differ by more
    // than this; closer than that they are treated as level and keep the side they already had
    // (see ResolveIsLeftOf). Two fighters can end up at exactly the same X - a jump-over at a
    // wall clamps the jumper to the same X as the cornered opponent - and the sign of a zero
    // difference must not pick a side.
    constexpr float kSideTieEpsilon = 0.01f;

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

    // How far a fighter's root can go. The wall marker stands at kStageMovementHalfWidth; the
    // limit sits one body width short of it (plus a little for the limbs), so a fighter at the
    // limit stands tangent to the wall instead of half inside it.
    constexpr float kStageFighterLimit = kStageMovementHalfWidth - (kFighterBodyHalfWidth + 0.15f);

    // Starting (and post-Reset) world-X gap between the player and the dummy. No move's own
    // reach + lunge closes the old default of 2.0 at all - confirmed by capturing every move
    // against a stationary dummy at that spacing and finding zero hits ever land, meaning a
    // fresh match started with nothing able to connect until the player walked in first, and
    // any hit-confirm cancel (a combo string) could never even begin. 1.6 leaves a small step-in
    // still required (not a free opening hit) while landing inside real reach once closed.
    // Eyeball-tuned - the in-engine hitbox tuning panel + collision debug-draw is the way to
    // dial this further once a move's own reach changes.
    constexpr float kDefaultFighterGapX = 1.6f;

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
