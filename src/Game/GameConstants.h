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
}
