#pragma once

#include <cstdint>

namespace Engine
{
    // Standard fixed-timestep accumulator - decouples simulation rate from render framerate
    // (per the networking-readiness decision: fixed timestep simulation, deterministic sim
    // state). Generic, not input-specific - kept separate from Input.h for that reason.
    //
    // Usage: call BeginFrame(deltaSeconds) once per render frame, then poll TryConsumeTick()
    // in a while loop - each true return means one fixed tick's worth of sim work should run
    // now. No sim work exists yet to actually drive with this - deliberately just the skeleton
    // ahead of a real per-tick consumer.
    struct FixedTimestepAccumulator
    {
        static constexpr float kFixedDeltaSeconds = 1.0f / 60.0f;
        static constexpr uint32_t kMaxTicksPerFrame = 8; // Spiral-of-death guard.

        float accumulatedSeconds = 0.0f;
        uint32_t tickCount = 0; // Monotonically increasing - usable directly as a tick number.

        void BeginFrame(float deltaSeconds)
        {
            accumulatedSeconds += deltaSeconds;
            ticksConsumedThisFrame = 0;
        }

        // Returns true once per fixed tick that should run this frame, false once caught up.
        // If the backlog would exceed kMaxTicksPerFrame (e.g. after a breakpoint or a window
        // drag stalls the render loop), the remaining backlog is dropped rather than spiraling
        // - the sim falls behind wall-clock time instead of trying to catch up in one frame.
        bool TryConsumeTick()
        {
            if (ticksConsumedThisFrame >= kMaxTicksPerFrame)
            {
                accumulatedSeconds = 0.0f;
                return false;
            }
            if (accumulatedSeconds < kFixedDeltaSeconds)
            {
                return false;
            }
            accumulatedSeconds -= kFixedDeltaSeconds;
            ++tickCount;
            ++ticksConsumedThisFrame;
            return true;
        }

    private:
        uint32_t ticksConsumedThisFrame = 0;
    };
}
