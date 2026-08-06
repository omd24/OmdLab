#pragma once

#include <cmath>
#include <cstdint>

namespace Engine
{
    // Minimal "which clip, how far into it" playback state - not a general animation state
    // machine or a blending/crossfade system (that's a later step's job, once a fighter state
    // machine exists to trigger different clips). Just enough to drive one clip on a loop.
    struct ClipPlayback
    {
        int32_t clipIndex = 0;
        float playbackTimeSeconds = 0.0f;
        bool playing = true;

        // No-op while paused. clipDurationSeconds wraps playbackTimeSeconds back into
        // [0, clipDurationSeconds) so it doesn't grow unbounded over a long play session -
        // Engine::EvaluateNodeWorldTransforms would wrap it again regardless when sampling, but
        // doing it here too keeps this struct's own state bounded and inspectable in the debug
        // UI.
        void Advance(float deltaSeconds, float clipDurationSeconds)
        {
            if (!playing)
            {
                return;
            }
            playbackTimeSeconds += deltaSeconds;
            if (clipDurationSeconds > 0.0f)
            {
                playbackTimeSeconds = fmodf(playbackTimeSeconds, clipDurationSeconds);
            }
        }
    };
}
