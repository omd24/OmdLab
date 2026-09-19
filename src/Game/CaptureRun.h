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

        // World-X the dummy is placed at before this run starts (see CaptureRun.cpp's
        // BuildPlan). 2.0 by default - matches every established single-move capture, so their
        // numbers stay comparable run to run. Combo scripts need the dummy in real hitbox range
        // instead: a cancel only ever fires on `hitConfirmed`, and no move's reach + lunge
        // closes a 2.0-unit gap, so at the default distance no capture has ever actually landed
        // a hit - a combo chain would silently never advance past its first move.
        float DummyX() const { return m_dummyX; }

        // World-X the player is placed at before this run starts - 0.0 unless a script needs a
        // specific spot (e.g. the wall-adjacent jump-over scripts).
        float PlayerX() const { return m_playerX; }

        // Non-empty for a script that tests one combo-only follow-up move in isolation (e.g.
        // "hook_light") - the caller should force the player straight into Attack playing this
        // move (Game::ForceEnterAttackMove) once, before running the plan, instead of relying
        // on input to select it (SelectMove can't reach a combo-only move - see
        // ForceEnterAttackMove's own comment). Empty for every other script.
        const std::string& ForcedMove() const { return m_forcedMove; }

    private:
        std::string m_name;
        std::vector<TickPlan> m_plan;
        Engine::InputCommand m_idle;
        float m_dummyX = 2.0f;
        float m_playerX = 0.0f;
        std::string m_forcedMove;
    };
}
