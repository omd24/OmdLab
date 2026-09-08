#include "CaptureRun.h"

#include "InputBindings.h"
#include "Engine/FrameCapture.h"
#include "Foundation/Log.h"

#include <array>

namespace
{
    using Game::FighterButton;
    using Foundation::Log::Severity;

    // Steps are compiled into a flat per-tick plan (index 0 == tick 1). A step consumes one or
    // more ticks; the plan grows as steps are appended.
    struct Builder
    {
        std::vector<Game::CaptureRun::TickPlan> out;

        void Wait(unsigned ticks)
        {
            for (unsigned i = 0; i < ticks; ++i)
            {
                out.emplace_back();
            }
        }

        // A one-tick tap: pressedThisTick + held on this tick, releasedThisTick on the next.
        void Tap(FighterButton button)
        {
            TapAxis(button, 0.0f);
        }

        // Same one-tick tap, but with the movement axis held at `axis` on both the press and
        // release ticks - for directional inputs like a forward jump (takeoff direction is
        // latched from the axis sign at the press tick).
        void TapAxis(FighterButton button, float axis)
        {
            const size_t slot = static_cast<size_t>(button);
            Game::CaptureRun::TickPlan pressFrame;
            pressFrame.input.axis = axis;
            pressFrame.input.buttons[slot].pressedThisTick = true;
            pressFrame.input.buttons[slot].held = true;
            out.push_back(pressFrame);

            Game::CaptureRun::TickPlan releaseFrame;
            releaseFrame.input.axis = axis;
            releaseFrame.input.buttons[slot].releasedThisTick = true;
            out.push_back(releaseFrame);
        }

        // Mark the last `count` appended ticks as capture frames with the given caption; if the
        // plan is shorter it also extends it (idle ticks) up to `count`.
        void CaptureLast(unsigned count, const char* caption)
        {
            while (out.size() < count)
            {
                out.emplace_back();
            }
            const size_t from = out.size() - count;
            for (size_t i = from; i < out.size(); ++i)
            {
                out[i].capture = true;
                out[i].caption = caption;
            }
        }

        // Append `count` idle ticks and capture all of them.
        void CaptureIdle(unsigned count, const char* caption)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                Game::CaptureRun::TickPlan f;
                f.capture = true;
                f.caption = caption;
                out.push_back(f);
            }
        }

        // Append `count` ticks holding the movement axis at `axis` (drives Walk/Run), captured.
        void CaptureAxis(float axis, unsigned count, const char* caption)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                Game::CaptureRun::TickPlan f;
                f.input.axis = axis;
                f.capture = true;
                f.caption = caption;
                out.push_back(f);
            }
        }
    };

    // total ~= settle + move length in ticks (60Hz). Kept a touch longer than the move so its
    // full recovery is captured. Move lengths after the 1/1.2 load-time speed scale: punch ~33,
    // kick_light ~55.
    std::vector<Game::CaptureRun::TickPlan> BuildPlan(const std::string& name)
    {
        Builder b;
        if (name == "idle")
        {
            b.CaptureIdle(48, "idle");
        }
        else if (name == "punch")
        {
            b.Wait(14);
            b.Tap(FighterButton::Punch);
            b.Wait(48);
            b.CaptureLast(50, "punch");
        }
        else if (name == "kick")
        {
            b.Wait(14);
            b.Tap(FighterButton::Kick);
            b.Wait(78);
            b.CaptureLast(80, "kick");
        }
        else if (name == "punchkick")
        {
            b.Wait(14);
            b.Tap(FighterButton::Punch);
            b.Wait(34);
            b.CaptureLast(36, "punch");
            b.Tap(FighterButton::Kick);
            b.Wait(78);
            b.CaptureLast(80, "kick");
        }
        else if (name == "walk")
        {
            b.CaptureAxis(1.0f, 40, "walk");   // full forward walk (blend param -> walkSpeed)
            b.CaptureAxis(0.0f, 30, "stop");   // release -> Walk -> Idle, param eases to 0
        }
        else if (name == "walkhalf")
        {
            b.CaptureAxis(0.5f, 48, "walkhalf"); // half stick -> blend(idle, walk_forward, ~0.5)
        }
        else if (name == "jump")
        {
            // Neutral jump: takeoff -> full arc -> land -> a few idle ticks. 150 captured ticks
            // covers the airtime with margin. Exercises the full jump pose path for
            // regression-diffing; run it several times when chasing a rare intermittent fault,
            // since one pass won't force it.
            b.Wait(14);
            b.Tap(FighterButton::Jump);
            b.Wait(150);
            b.CaptureLast(160, "jump");
        }
        else if (name == "jumpfwd")
        {
            // Forward jump - exercises the jump_forward clip + the takeoff-direction latch and
            // the horizontal travel the neutral jump doesn't.
            b.Wait(14);
            b.TapAxis(FighterButton::Jump, 1.0f);
            b.Wait(150);
            b.CaptureLast(160, "jumpfwd");
        }
        return b.out;
    }

    const std::array<const char*, 8> kScripts = {
        "idle", "punch", "kick", "punchkick", "walk", "walkhalf", "jump", "jumpfwd"
    };
}

namespace Game
{
    bool CaptureRun::IsKnownScript(const std::string& scriptName)
    {
        for (const char* s : kScripts)
        {
            if (scriptName == s)
            {
                return true;
            }
        }
        return false;
    }

    std::string CaptureRun::KnownScriptList()
    {
        std::string list;
        for (const char* s : kScripts)
        {
            list += (list.empty() ? "" : ", ");
            list += s;
        }
        return list;
    }

    CaptureRun::CaptureRun(const std::string& scriptName) : m_name(scriptName), m_plan(BuildPlan(scriptName))
    {
        Engine::FrameCapture::BeginSession(scriptName);
        Foundation::Log::Write(Severity::Info, "CaptureRun", "script '%s': %zu ticks planned", scriptName.c_str(), m_plan.size());
    }

    const Engine::InputCommand& CaptureRun::InputForTick(std::uint32_t tick) const
    {
        const size_t i = tick == 0 ? 0 : static_cast<size_t>(tick - 1);
        return i < m_plan.size() ? m_plan[i].input : m_idle;
    }

    bool CaptureRun::CaptureThisTick(std::uint32_t tick, std::string& outCaption) const
    {
        const size_t i = tick == 0 ? 0 : static_cast<size_t>(tick - 1);
        if (i < m_plan.size() && m_plan[i].capture)
        {
            outCaption = m_plan[i].caption;
            return true;
        }
        return false;
    }

    bool CaptureRun::Finished(std::uint32_t tick) const
    {
        return tick >= m_plan.size();
    }

    void CaptureRun::Finish()
    {
        Engine::FrameCapture::EndSession();
    }
}
