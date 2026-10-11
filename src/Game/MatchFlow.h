#pragma once

#if defined(OMD_PROTOTYPE)

namespace Game
{
    // TODO(OM): replace with real round/match flow (round counter, win conditions, timer,
    // match-over screen).

    // Drives what happens around a knockout: a short slow-motion beat, a banner hold, then a
    // reset of both fighters. Advanced once per render frame with real elapsed seconds.
    class MatchFlow
    {
    public:
        enum class Phase
        {
            Fighting,
            SlowMotion,
            Hold,
        };

        // loserSide: 0 = left/first fighter is down, 1 = right/second, -1 = nobody. Returns true
        // on the frame both fighters should be reset.
        bool Update(float deltaSeconds, int loserSide)
        {
            if (m_phase == Phase::Fighting)
            {
                if (loserSide >= 0)
                {
                    m_phase = Phase::SlowMotion;
                    m_loser = loserSide;
                    m_elapsed = 0.0f;
                }
                return false;
            }
            m_elapsed += deltaSeconds;
            if (m_phase == Phase::SlowMotion && m_elapsed >= kSlowMotionSeconds)
            {
                m_phase = Phase::Hold;
                m_elapsed = 0.0f;
            }
            else if (m_phase == Phase::Hold && m_elapsed >= kHoldSeconds)
            {
                Reset();
                return true;
            }
            return false;
        }

        void Reset()
        {
            m_phase = Phase::Fighting;
            m_elapsed = 0.0f;
            m_loser = -1;
        }

        // Multiplier for the simulation's frame delta.
        float TimeScale() const { return m_phase == Phase::SlowMotion ? kSlowMotionScale : 1.0f; }

        bool BannerVisible() const { return m_phase != Phase::Fighting; }

        // 0..1, fading in over the first part of the slow-motion beat.
        float BannerAlpha() const
        {
            if (m_phase != Phase::SlowMotion)
            {
                return m_phase == Phase::Hold ? 1.0f : 0.0f;
            }
            const float a = m_elapsed / 0.25f;
            return a > 1.0f ? 1.0f : a;
        }

        // Which side won (the other one from the loser); -1 while fighting.
        int WinnerSide() const { return m_loser < 0 ? -1 : 1 - m_loser; }

    private:
        static constexpr float kSlowMotionSeconds = 1.2f;
        static constexpr float kSlowMotionScale = 0.3f;
        static constexpr float kHoldSeconds = 1.8f;

        Phase m_phase = Phase::Fighting;
        float m_elapsed = 0.0f;
        int m_loser = -1;
    };
}

#endif // OMD_PROTOTYPE
