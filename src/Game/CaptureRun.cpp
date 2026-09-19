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

        // `times` one-tick taps back to back (2*times ticks). A single precisely-timed Tap()
        // into a cancel window is fragile: landing a hit against a stationary dummy holds the
        // hitbox and hurtbox overlapping for the box's whole active window, and each tick they
        // still overlap re-freezes hitstop, so the exact tick a cancel's `frame >= N` condition
        // first becomes true shifts by a few ticks run to run depending on the hit's own timing
        // - not something a combo script should have to predict exactly. Repeated presses across
        // a span comfortably wider than the cancel window catch it regardless of that drift.
        void TapRepeated(FighterButton button, unsigned times)
        {
            for (unsigned i = 0; i < times; ++i)
            {
                Tap(button);
            }
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

    // total ~= lead-in + move length + settle, in ticks (60Hz). Kept a touch longer than the
    // move so its full recovery is captured. Move lengths after the 1/1.2 load-time speed
    // scale: punch ~33, kick_light ~55. CaptureLast's count is bumped a few ticks past the
    // move's own length so it also grabs the tail of the pre-tap Wait() as genuinely idle
    // lead-in frames, not just the move itself - a capture that starts already mid-coil has
    // no quiet baseline before it, which undersells how much this state actually moves versus
    // its own rest pose.
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
            b.CaptureLast(58, "punch"); // +8 idle lead-in ticks before the tap
        }
        else if (name == "kick")
        {
            b.Wait(14);
            b.Tap(FighterButton::Kick);
            b.Wait(78);
            b.CaptureLast(88, "kick"); // +8 idle lead-in ticks before the tap
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
        // hook_light / hook_heavy / kick_low / kick_heavy are combo-only follow-ups - no input
        // button reaches them from neutral (see ForceEnterAttackMove's own comment), so these
        // scripts carry no taps at all: main.cpp force-enters the move once at tick 1 (see
        // CaptureRun::ForcedMove) and the plan is just "wait long enough, capture the tail".
        // No idle lead-in is possible here (there is no neutral entry tick to lead in to) -
        // these start already mid-move, unlike punch/kick's own scripts.
        else if (name == "hook_light" || name == "kick_low")
        {
            b.CaptureLast(40, name.c_str());
        }
        else if (name == "hook_heavy" || name == "kick_heavy")
        {
            b.CaptureLast(50, name.c_str());
        }
        // Combo scripts: press the same button again inside the previous move's own cancel
        // window (require frame >= N, checked against framesInMove) while it's hitConfirmed -
        // the dummy is placed in real hitbox range for these three only (CaptureRun::DummyX),
        // since at the default distance no move's reach + lunge connects at all and a cancel
        // would silently never fire.
        //
        // A single precisely-timed Tap() into that window turned out fragile: landing the hit
        // holds the hitbox and hurtbox overlapping for the box's whole active window, which
        // re-triggers hitstop on every one of those ticks against a stationary target, so
        // exactly when `frame >= N` first reads true while NOT frozen drifts by a few ticks
        // depending on the hit's own timing, not something worth predicting exactly. Each
        // follow-up is a wide TapRepeated instead - it only takes one press to land inside the
        // window, and pressing before it opens (ignored - the cancel's own condition isn't met
        // yet) or after the move has already ended (starts a fresh neutral attack instead,
        // visible as a second Idle->Attack entry in motion.csv rather than a same-state cancel)
        // is easy to tell apart when checking the result.
        else if (name == "combo_ppp") // punch, punch, punch -> punch -> hook_light -> hook_heavy
        {
            b.Wait(14);
            b.Tap(FighterButton::Punch);          // punch enters
            b.Wait(5);
            b.TapRepeated(FighterButton::Punch, 6); // hook_light (punch's cancel, frame >= 10)
            b.Wait(2);
            b.TapRepeated(FighterButton::Punch, 6); // hook_heavy (hook_light's cancel, frame >= 10)
            b.Wait(33);
            b.CaptureLast(110, "combo_ppp");
        }
        else if (name == "combo_kkk") // kick, kick, kick -> kick_light -> kick_low -> kick_heavy
        {
            b.Wait(14);
            b.Tap(FighterButton::Kick);          // kick_light enters
            b.Wait(7);
            b.TapRepeated(FighterButton::Kick, 7); // kick_low (kick_light's cancel, frame >= 12)
            b.Wait(2);
            b.TapRepeated(FighterButton::Kick, 6); // kick_heavy (kick_low's cancel, frame >= 10)
            b.Wait(33);
            b.CaptureLast(120, "combo_kkk");
        }
        else if (name == "combo_ppk") // punch, punch, kick -> punch -> hook_light -> kick_heavy
        {
            b.Wait(14);
            b.Tap(FighterButton::Punch);          // punch enters
            b.Wait(5);
            b.TapRepeated(FighterButton::Punch, 6); // hook_light (punch's cancel, frame >= 10)
            b.Wait(2);
            b.TapRepeated(FighterButton::Kick, 6);  // kick_heavy (hook_light's cancel, frame >= 10)
            b.Wait(33);
            b.CaptureLast(110, "combo_ppk");
        }
        else if (name == "combo_kkp") // kick, kick, punch -> kick_light -> kick_low -> hook_heavy
        {
            b.Wait(14);
            b.Tap(FighterButton::Kick);          // kick_light enters
            b.Wait(7);
            b.TapRepeated(FighterButton::Kick, 7); // kick_low (kick_light's cancel, frame >= 12)
            b.Wait(2);
            b.TapRepeated(FighterButton::Punch, 6); // hook_heavy (kick_low's cancel, frame >= 10)
            b.Wait(33);
            b.CaptureLast(110, "combo_kkp");
        }
        return b.out;
    }

    const std::array<const char*, 16> kScripts = {
        "idle", "punch", "kick", "punchkick", "walk", "walkhalf", "jump", "jumpfwd",
        "hook_light", "hook_heavy", "kick_low", "kick_heavy", "combo_ppp", "combo_kkk", "combo_ppk", "combo_kkp"
    };

    // Dummy world-X for a capture run, before its plan starts (see CaptureRun::DummyX's own
    // comment on why combo scripts need a different one).
    float DummyXForScript(const std::string& name)
    {
        return name.rfind("combo_", 0) == 0 ? 1.2f : 2.0f;
    }

    // Non-empty for a script that force-enters one combo-only follow-up move directly (see
    // CaptureRun::ForcedMove's own comment).
    std::string ForcedMoveForScript(const std::string& name)
    {
        if (name == "hook_light" || name == "hook_heavy" || name == "kick_low" || name == "kick_heavy")
        {
            return name;
        }
        return {};
    }
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

    CaptureRun::CaptureRun(const std::string& scriptName)
        : m_name(scriptName), m_plan(BuildPlan(scriptName)), m_dummyX(DummyXForScript(scriptName)),
          m_forcedMove(ForcedMoveForScript(scriptName))
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
