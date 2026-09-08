#pragma once

#include "Engine/Input.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Game
{
    // A deterministic scripted capture run for the frame-capture harness. Built from a named
    // hardcoded script (see CaptureRun.cpp); drives the player fighter's per-tick input and
    // says which frames to screenshot.
    //
    // Contract on the caller's render loop while a run is active (see main.cpp):
    //   - exactly one sim tick per render frame, at FixedTimestepAccumulator::kFixedDeltaSeconds,
    //     so "tick N" is byte-reproducible run to run;
    //   - the player's InputCommand is InputForTick(tick) instead of AssembleInputCommand;
    //   - the debug UI is hidden;
    //   - fighters are reset to a neutral pose once, before tick 1.
    // Ticks are FixedTimestepAccumulator::tickCount (1-based: the first simulated tick is 1).
    class CaptureRun
    {
    public:
        // One simulated tick's plan. Public only so CaptureRun.cpp's script builder can name it.
        struct TickPlan
        {
            Engine::InputCommand input;
            bool capture = false;
            std::string caption;
        };

        static bool IsKnownScript(const std::string& scriptName);
        static std::string KnownScriptList();

        explicit CaptureRun(const std::string& scriptName); // opens a FrameCapture session

        const Engine::InputCommand& InputForTick(std::uint32_t tick) const;
        bool CaptureThisTick(std::uint32_t tick, std::string& outCaption) const;
        bool Finished(std::uint32_t tick) const;
        void Finish(); // closes the FrameCapture session (writes motion.csv)

        const std::string& Name() const { return m_name; }

    private:
        std::string m_name;
        std::vector<TickPlan> m_plan;
        Engine::InputCommand m_idle;
    };
}
