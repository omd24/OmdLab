#if defined(OMD_PROTOTYPE)

#include "AiOpponent.h"

#include "GameConstants.h"
#include "InputBindings.h"
#include "Foundation/Log.h"

#include <algorithm>
#include <cmath>

namespace Game
{
    namespace
    {
        enum AiButtonBits : uint8_t
        {
            kBitPunch = 1,
            kBitKick = 2,
            kBitSpecial = 4,
            kBitBlock = 8,
            kBitRun = 16,
            kBitJump = 32,
        };

        constexpr float kStrikeRange = 1.25f;     // Close enough that a ground strike connects.
        constexpr float kThreatRange = 1.8f;      // An opponent attack inside this is worth reacting to.
        constexpr float kFireballMinRange = 2.3f;
        constexpr float kJumpInMinRange = 1.4f;
        constexpr float kJumpInMaxRange = 3.4f;
        constexpr int kSafeHealth = 22;           // Below this a non-lethal brain stops attacking.
        constexpr int kComboSafeHealth = 42;      // ...and below this it stops starting combos.
        constexpr float kWalkSpeed = 2.0f;        // Mirrors the character's walk speed, for plan lengths.

        int RandRange(uint32_t& rng, int lo, int hi)
        {
            return lo + static_cast<int>(AiRand01(rng) * static_cast<float>(hi - lo + 1));
        }

        void Push(AiBrain& brain, int ticks, float axis, uint8_t held)
        {
            brain.plan.push_back(AiStep{ std::max(1, ticks), axis, held });
        }

        void Tap(AiBrain& brain, uint8_t bit, int gapAfter, float axis = 0.0f)
        {
            Push(brain, 1, axis, bit);
            Push(brain, gapAfter, axis, 0);
        }

        void StartPlan(AiBrain& brain)
        {
            brain.planIndex = 0;
            brain.stepTicksLeft = brain.plan.empty() ? 0 : brain.plan[0].ticks;
        }

        // Combo strings reuse the existing cancel chains: the gaps keep each press inside the
        // previous move's cancel window.
        void BuildCombo(AiBrain& brain, int kind)
        {
            brain.plan.clear();
            switch (kind)
            {
            case 0: // punch, punch, punch
                Tap(brain, kBitPunch, 9);
                Tap(brain, kBitPunch, 9);
                Tap(brain, kBitPunch, 14);
                break;
            case 1: // punch, punch, kick
                Tap(brain, kBitPunch, 9);
                Tap(brain, kBitPunch, 9);
                Tap(brain, kBitKick, 18);
                break;
            case 2: // kick, kick, kick
                Tap(brain, kBitKick, 13);
                Tap(brain, kBitKick, 13);
                Tap(brain, kBitKick, 22);
                break;
            default: // kick, kick, punch
                Tap(brain, kBitKick, 13);
                Tap(brain, kBitKick, 13);
                Tap(brain, kBitPunch, 18);
                break;
            }
            StartPlan(brain);
        }

        void BuildSingle(AiBrain& brain, uint8_t bit, int recovery)
        {
            brain.plan.clear();
            Tap(brain, bit, recovery);
            if (bit == kBitSpecial)
            {
                brain.ticksSinceFireball = 0;
                brain.fireballGapTicks = RandRange(brain.rng, 420, 760);
            }
            StartPlan(brain);
        }

        void BuildMove(AiBrain& brain, float axis, int ticks, bool run = false)
        {
            brain.plan.clear();
            Push(brain, ticks, axis, run ? kBitRun : 0);
            StartPlan(brain);
        }

        void BuildBlock(AiBrain& brain, int delayTicks, int holdTicks)
        {
            brain.plan.clear();
            if (delayTicks > 0)
            {
                Push(brain, delayTicks, 0.0f, 0);
            }
            Push(brain, holdTicks, 0.0f, kBitBlock);
            StartPlan(brain);
        }

        // Jump toward the opponent and throw an aerial strike on the way down (a strike thrown
        // early passes over a standing opponent), or jump away.
        void BuildJumpIn(AiBrain& brain, float towardAxis)
        {
            brain.plan.clear();
            brain.ticksSinceJump = 0;
            Push(brain, 1, towardAxis, kBitJump);
            Push(brain, RandRange(brain.rng, 17, 21), towardAxis, 0);
            Tap(brain, (AiRand01(brain.rng) < 0.5f) ? kBitPunch : kBitKick, 26, towardAxis);
            StartPlan(brain);
        }

        // Escape from a corner: jump toward the opponent and over them, landing on the open side.
        void BuildJumpOver(AiBrain& brain, float towardAxis)
        {
            brain.plan.clear();
            brain.ticksSinceJump = 0;
            Push(brain, 1, towardAxis, kBitJump);
            Push(brain, 44, towardAxis, 0);
            StartPlan(brain);
        }

        void BuildJumpAway(AiBrain& brain, float awayAxis)
        {
            brain.plan.clear();
            brain.ticksSinceJump = 0;
            Push(brain, 1, awayAxis, kBitJump);
            Push(brain, 40, awayAxis, 0);
            StartPlan(brain);
        }

        AiIntent IntentFromStep(const AiStep& step)
        {
            AiIntent intent;
            intent.axis = step.axis;
            intent.punch = (step.held & kBitPunch) != 0;
            intent.kick = (step.held & kBitKick) != 0;
            intent.special = (step.held & kBitSpecial) != 0;
            intent.block = (step.held & kBitBlock) != 0;
            intent.run = (step.held & kBitRun) != 0;
            intent.jump = (step.held & kBitJump) != 0;
            return intent;
        }

        // The first input of the plan that was just built.
        AiIntent Begin(const AiBrain& brain)
        {
            return IntentFromStep(brain.plan[0]);
        }

        // Closing the distance: always something active - run, jump-in, fireball or walk.
        AiIntent ChooseApproach(
            AiBrain& brain, float distance, float toward, AiRole role, const AiTuning& tuning, int opponentHealth, bool targetFree)
        {
            const float roll = AiRand01(brain.rng);
            const bool fireballOk = tuning.lethal || opponentHealth > 30;
            if (distance > kFireballMinRange && fireballOk && roll < tuning.fireballChance && (targetFree || AiRand01(brain.rng) < 0.25f))
            {
                BuildSingle(brain, kBitSpecial, 46);
                return Begin(brain);
            }
            if (distance > kJumpInMinRange && distance < kJumpInMaxRange && (tuning.lethal || opponentHealth > kSafeHealth) &&
                AiRand01(brain.rng) < tuning.jumpChance)
            {
                BuildJumpIn(brain, toward);
                return Begin(brain);
            }
            if (distance > 2.2f && AiRand01(brain.rng) < tuning.runChance)
            {
                const float seconds = std::max(0.0f, distance - kStrikeRange) / 4.0f;
                BuildMove(brain, toward, std::clamp(static_cast<int>(seconds * 60.0f), 10, 60), true);
                return Begin(brain);
            }
            (void)role;
            const float walkSeconds = std::max(0.0f, distance - kStrikeRange + 0.1f) / kWalkSpeed;
            BuildMove(brain, toward, std::clamp(static_cast<int>(walkSeconds * 60.0f), 8, 50));
            return Begin(brain);
        }

        // In striking range with the initiative: a combo or a single strike.
        AiIntent ChooseStrike(AiBrain& brain, AiRole role, const AiTuning& tuning, int opponentHealth)
        {
            const bool combosOk = tuning.allowCombos && (tuning.lethal || opponentHealth > kComboSafeHealth);
            const float comboShare = combosOk ? (role == AiRole::Attacker ? 0.6f : 0.3f) : 0.0f;
            const float roll = AiRand01(brain.rng);
            if (roll < comboShare)
            {
                BuildCombo(brain, RandRange(brain.rng, 0, 3));
            }
            else if (roll < comboShare + (1.0f - comboShare) * 0.5f)
            {
                BuildSingle(brain, kBitPunch, 12);
            }
            else
            {
                BuildSingle(brain, kBitKick, 16);
            }
            return Begin(brain);
        }
    }

    float AiRand01(uint32_t& state)
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    }

    void ResetAiBrain(AiBrain& brain, uint32_t seed)
    {
        brain = AiBrain{};
        brain.rng = seed * 2654435761u + 12345u;
        brain.fireballGapTicks = RandRange(brain.rng, 420, 760);
    }

    AiTuning TuningForProfile(AiProfile profile)
    {
        AiTuning tuning;
        switch (profile)
        {
        case AiProfile::Passive:
            tuning.blockChance = 0.6f;
            tuning.punishChance = 0.5f;
            tuning.attackRate = 0.6f;
            tuning.fireballChance = 0.08f;
            tuning.jumpChance = 0.08f;
            tuning.allowCombos = false;
            break;
        case AiProfile::Reactive:
            tuning.blockChance = 0.8f;
            tuning.punishChance = 0.8f;
            tuning.attackRate = 1.0f;
            tuning.fireballChance = 0.12f;
            break;
        case AiProfile::Aggressive:
            tuning.blockChance = 0.5f;
            tuning.punishChance = 0.7f;
            tuning.attackRate = 1.8f;
            tuning.fireballChance = 0.15f;
            tuning.jumpChance = 0.22f;
            break;
        }
        return tuning;
    }

    AiIntent TickAiBrain(
        AiBrain& brain, const AiFighterView& self, const AiFighterView& opponent, AiRole role, const AiTuning& tuning)
    {
        ++brain.ticksSinceFireball;
        ++brain.ticksSinceJump;
        if (brain.fireballReactionLock > 0)
        {
            --brain.fireballReactionLock;
        }
        // Thresholds below are percentages, so they hold at any max health.
        const int opponentPercent = opponent.health * 100 / std::max(1, opponent.maxHealth);
        // Hurt, or the fight is over (the opponent is down): no input at all.
        if (self.stunned || opponent.knockedOut)
        {
            brain.plan.clear();
            brain.planIndex = 0;
            brain.stepTicksLeft = 0;
            brain.cooldownTicks = std::max(brain.cooldownTicks, 4);
            return {};
        }

        const float dx = opponent.x - self.x;
        const float distance = std::fabs(dx);
        const float toward = dx >= 0.0f ? 1.0f : -1.0f;

        // An enemy fireball is coming: decide once, with a deliberately mixed bag of answers -
        // guard, jump it (the timing can be right or off), run into it, or throw one back.
        // The cast itself is the earliest warning: the thrower is visible for ~0.4 s before the
        // ball exists, which is what gives a jump time to be timed.
        const bool castWarning = opponent.castingFireball && opponent.framesInMove < opponent.releaseFrame &&
                                 (opponent.attackInstance | 0x80000000u) != brain.lastProjectileId && distance > 1.6f;
        const bool flightWarning = self.incomingProjectile && self.incomingId != brain.lastProjectileId &&
                                   self.incomingDistance < 4.0f && brain.fireballReactionLock == 0;
        if (!self.attacking && !self.airborne && (castWarning || flightWarning))
        {
            brain.lastProjectileId = castWarning ? (opponent.attackInstance | 0x80000000u) : self.incomingId;
            brain.fireballReactionLock = 80;
            const float r = AiRand01(brain.rng);
            // Ticks until the ball reaches this fighter: the rest of the cast, then 6 units/s
            // from just in front of the thrower (the body edge is ~0.6 away).
            const float travelDistance = castWarning ? std::max(0.0f, distance - 0.7f) : self.incomingDistance;
            const int untilRelease = castWarning ? static_cast<int>(opponent.releaseFrame - opponent.framesInMove) : 0;
            const int arrival = std::max(0, untilRelease + static_cast<int>((travelDistance - 0.6f) * 10.0f));
            Foundation::Log::Write(
                Foundation::Log::Severity::Info, "AI", "fireball reaction: roll=%.2f arrival=%d ticks (%s)", r, arrival,
                castWarning ? "cast" : "in flight");
            if (r < 0.25f)
            {
                BuildBlock(brain, std::max(0, arrival - 8), 26);
                return Begin(brain);
            }
            if (r < 0.65f)
            {
                // The apex of a jump is ~21 ticks in and is the only part that clears the ball:
                // jump so the apex meets it, with a timing error that is sometimes too big.
                const int error = RandRange(brain.rng, -6, 6);
                const int wait = std::max(0, arrival - 21 + error);
                brain.plan.clear();
                if (wait > 0)
                {
                    Push(brain, wait, 0.0f, 0);
                }
                const float axis = (AiRand01(brain.rng) < 0.6f) ? toward : 0.0f;
                brain.ticksSinceJump = 0;
                Push(brain, 1, axis, kBitJump);
                Push(brain, 44, axis, 0);
                StartPlan(brain);
                return Begin(brain);
            }
            if (r < 0.8f)
            {
                // A mistake: run straight into it.
                BuildMove(brain, toward, 22, true);
                return Begin(brain);
            }
            if (r < 0.86f && travelDistance > 2.0f)
            {
                BuildSingle(brain, kBitSpecial, 46); // Trade: the two fireballs cancel.
                return Begin(brain);
            }
            BuildBlock(brain, 0, 24);
            return Begin(brain);
        }

        // React to a fresh opponent attack, once per attack: guard it (with some reaction
        // delay) or, when not guarding, sometimes walk into it.
        if (!self.attacking && !self.airborne && opponent.attacking && opponent.attackInstance != brain.lastReactedInstance &&
            distance < kThreatRange && opponent.framesInMove < opponent.strikeEndFrame)
        {
            brain.lastReactedInstance = opponent.attackInstance;
            if (AiRand01(brain.rng) < tuning.blockChance)
            {
                const int remaining = static_cast<int>(opponent.strikeEndFrame) - static_cast<int>(opponent.framesInMove);
                BuildBlock(brain, RandRange(brain.rng, 0, 3), std::clamp(remaining + 8, 12, 40));
                return Begin(brain);
            }
            if (AiRand01(brain.rng) < tuning.mistakeChance)
            {
                BuildMove(brain, toward, RandRange(brain.rng, 8, 16));
                return Begin(brain);
            }
        }

        // Punish an attack that has just run out (whiffed or blocked) while still in range.
        if (!self.attacking && !self.airborne && opponent.attacking && opponent.exposed &&
            opponent.attackInstance != brain.lastPunishedInstance && distance < kStrikeRange + 0.1f)
        {
            brain.lastPunishedInstance = opponent.attackInstance;
            if (AiRand01(brain.rng) < tuning.punishChance && (tuning.lethal || opponentPercent > kSafeHealth))
            {
                BuildSingle(brain, (AiRand01(brain.rng) < 0.5f) ? kBitPunch : kBitKick, 12);
                return Begin(brain);
            }
        }

        // Run the current plan.
        if (brain.planIndex < brain.plan.size())
        {
            const AiIntent intent = IntentFromStep(brain.plan[brain.planIndex]);
            if (--brain.stepTicksLeft <= 0)
            {
                ++brain.planIndex;
                if (brain.planIndex < brain.plan.size())
                {
                    brain.stepTicksLeft = brain.plan[brain.planIndex].ticks;
                }
                else
                {
                    brain.plan.clear();
                    brain.planIndex = 0;
                    const float rate = std::max(0.1f, tuning.attackRate);
                    brain.cooldownTicks = static_cast<int>(static_cast<float>(RandRange(brain.rng, 3, 10)) / rate);
                }
            }
            return intent;
        }

        if (self.attacking || self.airborne)
        {
            return {};
        }
        if (brain.cooldownTicks > 0)
        {
            --brain.cooldownTicks;
            return {};
        }

        // Choose something new. There is no "stand and wait" option: every choice moves the
        // fighter or attacks.

        // Cornered: the wall is behind this fighter (the opponent stands between it and the
        // open stage). Walking toward a wall to chase does not count - then the opponent is on
        // the wall side. Mostly escape by jumping over the opponent.
        constexpr float kCornerZone = 0.9f;
        const float wallSide = self.x >= 0.0f ? 1.0f : -1.0f;
        if (std::fabs(self.x) > kStageFighterLimit - kCornerZone && toward * wallSide < 0.0f && distance < 2.4f &&
            AiRand01(brain.rng) < 0.6f)
        {
            Foundation::Log::Write(Foundation::Log::Severity::Info, "AI", "corner escape at x=%.2f", self.x);
            BuildJumpOver(brain, toward);
            return Begin(brain);
        }
        const bool opponentLow = !tuning.lethal && opponentPercent <= kSafeHealth;
        // A fireball is only worth throwing when the target can answer it (not mid-attack,
        // airborne or stunned) - otherwise it is a free hit rather than an exchange.
        const bool targetFree = !opponent.attacking && !opponent.airborne && !opponent.stunned;
        const float roll = AiRand01(brain.rng);

        // Variety: an action that has not happened for a while goes first.
        const int kFireballGapTicks = brain.fireballGapTicks;
        constexpr int kJumpGapTicks = 300;
        if (!opponentLow)
        {
            if (brain.ticksSinceFireball > kFireballGapTicks && (tuning.lethal || opponentPercent > 30) &&
                !(opponent.castingFireball && AiRand01(brain.rng) < 0.75f) && (targetFree || brain.ticksSinceFireball > kFireballGapTicks + 240))
            {
                if (distance > kFireballMinRange)
                {
                    BuildSingle(brain, kBitSpecial, 46);
                }
                else
                {
                    // Too close for the throw to read: step back to range first, then throw.
                    const float needed = kFireballMinRange + 0.4f - distance;
                    brain.plan.clear();
                    Push(brain, std::clamp(static_cast<int>(needed / kWalkSpeed * 60.0f), 15, 70), -toward, 0);
                    Tap(brain, kBitSpecial, 46);
                    brain.ticksSinceFireball = 0;
                    StartPlan(brain);
                }
                return Begin(brain);
            }
            if (brain.ticksSinceJump > kJumpGapTicks && distance > 1.0f && distance < kJumpInMaxRange &&
                (tuning.lethal || opponentPercent > kSafeHealth))
            {
                BuildJumpIn(brain, toward);
                return Begin(brain);
            }
        }

        if (role == AiRole::Defender || opponentLow)
        {
            // Not the one pressing: keep moving - back off, jump away, throw a fireball from
            // range, guard for a beat, or counter when the opponent is right there.
            if (distance < kStrikeRange + 0.2f)
            {
                if (roll < 0.35f)
                {
                    BuildMove(brain, -toward, RandRange(brain.rng, 14, 28));
                }
                else if (roll < 0.62f)
                {
                    BuildJumpAway(brain, -toward);
                }
                else if (roll < 0.82f || opponentLow)
                {
                    BuildBlock(brain, 0, RandRange(brain.rng, 8, 16));
                }
                else
                {
                    return ChooseStrike(brain, role, tuning, opponentPercent);
                }
                return Begin(brain);
            }
            if (distance > kFireballMinRange && !opponentLow && roll < 0.4f && targetFree)
            {
                BuildSingle(brain, kBitSpecial, 46);
                return Begin(brain);
            }
            // A short step back only while still close - a fighter that keeps backing away from
            // the one pressing it opens the distance and stalls the fight.
            if (distance < 2.0f && roll < 0.7f)
            {
                BuildMove(brain, -toward, RandRange(brain.rng, 6, 12));
                return Begin(brain);
            }
            return ChooseApproach(brain, distance, toward, role, tuning, opponentPercent, targetFree);
        }

        if (distance > kStrikeRange)
        {
            return ChooseApproach(brain, distance, toward, role, tuning, opponentPercent, targetFree);
        }
        return ChooseStrike(brain, role, tuning, opponentPercent);
    }

    void ApplyAiIntent(const AiIntent& intent, uint32_t tick, const Engine::InputCommand& previous, Engine::InputCommand& out)
    {
        out.tick = tick;
        out.axis = intent.axis;
        const auto set = [&](FighterButton button, bool held)
        {
            const size_t slot = static_cast<size_t>(button);
            const bool wasHeld = previous.buttons[slot].held;
            out.buttons[slot].held = held;
            out.buttons[slot].pressedThisTick = held && !wasHeld;
            out.buttons[slot].releasedThisTick = !held && wasHeld;
        };
        set(FighterButton::Punch, intent.punch);
        set(FighterButton::Kick, intent.kick);
        set(FighterButton::Special, intent.special);
        set(FighterButton::Block, intent.block);
        set(FighterButton::Run, intent.run);
        set(FighterButton::Jump, intent.jump);
        set(FighterButton::Crouch, false);
    }

    void FightDirector::Reset(uint32_t newSeed, int health0, int health1)
    {
        seed = newSeed;
        m_rng = newSeed * 2246822519u + 99u;
        elapsedSeconds = 0.0f;
        finale = false;
        m_quietSeconds = 0.0f;
        m_urgentSeconds = 0.0f;
        m_lastHealth[0] = health0;
        m_lastHealth[1] = health1;
        m_maxHealth = std::max(std::max(health0, health1), 1);
        m_allIn = false;
        m_attacker = (AiRand01(m_rng) < 0.5f) ? 0 : 1;
        BeginNeutral();
    }

    void FightDirector::BeginNeutral()
    {
        m_phase = Phase::Neutral;
        m_phaseSeconds = 0.0f;
        m_neutralSeconds = 0.5f + AiRand01(m_rng) * 0.7f;
        role[0] = role[1] = AiRole::Neutral;
        ApplyTunings();
    }

    void FightDirector::BeginOffense(int attacker)
    {
        m_phase = Phase::Offense;
        m_phaseSeconds = 0.0f;
        m_attacker = attacker;
        m_hitsThisOffense = 0;
        m_swapAfterHits = RandRange(m_rng, 3, 5);
        role[attacker] = AiRole::Attacker;
        role[1 - attacker] = AiRole::Defender;
        ApplyTunings();
    }

    int FightDirector::ChooseAttacker(int health0, int health1) const
    {
        constexpr int kLastStandPercent = 22;
        // Before the lethal phase nobody is finished off: a fighter that is nearly out of health
        // takes the offense instead (a last stand) and the other one has to defend.
        const bool lethalPhase = finale || elapsedSeconds > fightLengthSeconds * 0.45f;
        if (!lethalPhase)
        {
            const int percent0 = health0 * 100 / m_maxHealth;
            const int percent1 = health1 * 100 / m_maxHealth;
            if (percent0 <= kLastStandPercent && percent1 > kLastStandPercent)
            {
                return 0;
            }
            if (percent1 <= kLastStandPercent && percent0 > kLastStandPercent)
            {
                return 1;
            }
        }
        return m_attacker;
    }

    void FightDirector::ApplyTunings()
    {
        const float urgent = m_urgentSeconds > 0.0f ? 1.8f : 1.0f;
        for (int i = 0; i < 2; ++i)
        {
            AiTuning t;
            if (m_allIn)
            {
                // Both fighters are low: no more holding back - open attacks, little guarding,
                // combos allowed, no health floor.
                t.blockChance = 0.3f;
                t.punishChance = 0.9f;
                t.attackRate = 2.4f * urgent;
                t.fireballChance = 0.25f;
                t.jumpChance = 0.35f;
                t.runChance = 0.7f;
                t.allowCombos = true;
                t.lethal = true;
                tuning[i] = t;
                continue;
            }
            switch (role[i])
            {
            case AiRole::Attacker:
                t.blockChance = 0.75f;
                t.punishChance = 0.9f;
                t.attackRate = 2.2f * urgent;
                t.fireballChance = 0.2f;
                t.jumpChance = 0.4f;
                t.runChance = 0.7f;
                // No health floor once the fight is well under way - a pressing fighter that
                // stops attacking near a win is what makes a lead feel slow.
                t.lethal = finale || elapsedSeconds > fightLengthSeconds * 0.45f;
                break;
            case AiRole::Defender:
                // Guards imperfectly and sometimes just walks into the attack - these are the
                // intentional mistakes that let the exchange land. Barely guards in the finale.
                t.blockChance = finale ? 0.12f : 0.55f;
                t.mistakeChance = 0.25f;
                t.punishChance = 0.6f;
                t.attackRate = 1.4f * urgent;
                t.fireballChance = 0.22f;
                t.jumpChance = 0.25f;
                t.allowCombos = false;
                t.lethal = false;
                break;
            case AiRole::Neutral:
                t.blockChance = 0.7f;
                t.punishChance = 0.6f;
                t.attackRate = 1.6f * urgent;
                t.fireballChance = 0.14f;
                t.jumpChance = 0.3f;
                t.lethal = false;
                break;
            }
            tuning[i] = t;
        }
    }

    void FightDirector::Update(float deltaSeconds, int health0, int health1, bool activity)
    {
        // A new round (a fighter back at higher health) restarts the choreography.
        const int health[2] = { health0, health1 };
        if (health0 > m_lastHealth[0] || health1 > m_lastHealth[1])
        {
            Reset(seed, health0, health1);
            return;
        }

        elapsedSeconds += deltaSeconds;
        m_phaseSeconds += deltaSeconds;
        m_quietSeconds = activity ? 0.0f : m_quietSeconds + deltaSeconds;
        if (m_urgentSeconds > 0.0f)
        {
            m_urgentSeconds -= deltaSeconds;
            if (m_urgentSeconds <= 0.0f)
            {
                ApplyTunings();
            }
        }

        // A health drop is a hit landed by the other fighter.
        for (int i = 0; i < 2; ++i)
        {
            if (health[i] < m_lastHealth[i] && m_phase == Phase::Offense && i != m_attacker)
            {
                ++m_hitsThisOffense;
            }
            m_lastHealth[i] = health[i];
        }

        // Both at or below half health: everybody goes for the win (a one-sided exchange is
        // fine early on, a stand-off between two hurt fighters is not).
        if (!m_allIn && health0 * 2 <= m_maxHealth && health1 * 2 <= m_maxHealth)
        {
            m_allIn = true;
            role[0] = role[1] = AiRole::Attacker;
            m_phase = Phase::Offense;
            m_phaseSeconds = 0.0f;
            ApplyTunings();
            return;
        }
        if (m_allIn)
        {
            // No more role swaps or neutral phases; only the quiet watchdog below still applies.
            if (m_phase == Phase::Neutral)
            {
                m_phase = Phase::Offense;
            }
            return;
        }

        if (!finale && elapsedSeconds >= fightLengthSeconds)
        {
            finale = true;
            // The fighter ahead takes the offense for the finish.
            BeginOffense(health[0] >= health[1] ? 0 : 1);
            return;
        }

        // Nothing has happened for a moment (nobody attacking or hurt): cut the neutral phase
        // short, put somebody on the offense, and raise the tempo for a couple of seconds.
        constexpr float kQuietLimitSeconds = 1.1f;
        if (m_quietSeconds > kQuietLimitSeconds)
        {
            m_quietSeconds = 0.0f;
            m_urgentSeconds = 2.0f;
            if (m_phase == Phase::Neutral)
            {
                BeginOffense(ChooseAttacker(health0, health1));
            }
            else
            {
                ApplyTunings();
            }
            return;
        }

        if (m_phase == Phase::Neutral)
        {
            if (m_phaseSeconds >= m_neutralSeconds)
            {
                BeginOffense(ChooseAttacker(health0, health1));
            }
        }
        else if (!finale)
        {
            const int desired = ChooseAttacker(health0, health1);
            if (desired != m_attacker && m_phaseSeconds > 0.8f)
            {
                BeginOffense(desired); // Last stand / the leader takes over the initiative.
            }
            else if (m_hitsThisOffense >= m_swapAfterHits || m_phaseSeconds > 7.0f)
            {
                m_attacker = 1 - m_attacker;
                BeginNeutral();
            }
        }
    }
}

#endif // OMD_PROTOTYPE
