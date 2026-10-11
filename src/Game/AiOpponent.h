#pragma once

#if defined(OMD_PROTOTYPE)

#include "Engine/Input.h"

#include <cstdint>
#include <vector>

namespace Game
{
    // TODO(OM): replace with a real decision system (spacing model, frame-data-aware option
    // selection, per-character profiles) and a real match-flow director.
    // TODO(OM): explore learned/adversarial agents: two agents that model each other and
    // develop strategies for winning (zoning, baiting, punishing habits) from self-play, instead
    // of the scripted, director-shaped behaviour here.

    // What a fighter's brain is currently trying to do, as chosen by the director (or Neutral
    // when a brain runs on its own).
    enum class AiRole
    {
        Neutral,
        Attacker,
        Defender,
    };

    // The knobs of a brain. A profile or a director role fills these in.
    struct AiTuning
    {
        float blockChance = 0.8f;    // Per opponent attack: chance to guard it.
        float punishChance = 0.8f;   // Chance to punish an opponent attack that has just ended.
        float mistakeChance = 0.0f;  // When not guarding: chance to walk into the attack instead of standing.
        float attackRate = 1.0f;     // Scales how soon the next action is chosen (higher = busier).
        float fireballChance = 0.12f;
        float jumpChance = 0.15f;    // Share of approaches that are jump-ins.
        float runChance = 0.5f;      // Share of long approaches that are runs.
        bool allowCombos = true;
        bool lethal = true;          // False: never starts an attack on an opponent that is nearly out of health.
    };

    // What a brain may observe about one fighter, filled in by the caller from game state.
    struct AiFighterView
    {
        float x = 0.0f;
        bool facingRight = true;
        bool airborne = false;
        bool stunned = false;  // HitStun or KO.
        bool knockedOut = false;
        bool attacking = false;
        uint32_t attackInstance = 0;
        uint32_t framesInMove = 0;
        uint32_t strikeEndFrame = 0; // startup + active of the current move.
        int health = 100;
        int maxHealth = 100;

        // Casting a fireball (until the move ends), and the move frame it is released on.
        bool castingFireball = false;
        uint32_t releaseFrame = 0;
        // Attacking and past the point where it could still hit (a spent strike, or a fireball
        // that has already left the hand): a punish opportunity.
        bool exposed = false;
        // The nearest enemy fireball heading for this fighter.
        bool incomingProjectile = false;
        float incomingDistance = 0.0f;
        uint32_t incomingId = 0;
    };

    // The inputs a brain wants held this tick.
    struct AiIntent
    {
        float axis = 0.0f;
        bool punch = false;
        bool kick = false;
        bool special = false;
        bool block = false;
        bool run = false;
        bool jump = false;
    };

    // One scripted step of a plan: hold these inputs for this many ticks.
    struct AiStep
    {
        int ticks = 1;
        float axis = 0.0f;
        uint8_t held = 0; // Bit mask, see the button bits in the .cpp.
    };

    struct AiBrain
    {
        uint32_t rng = 1;
        std::vector<AiStep> plan;
        size_t planIndex = 0;
        int stepTicksLeft = 0;
        int cooldownTicks = 0;
        uint32_t lastReactedInstance = 0;
        uint32_t lastPunishedInstance = 0;
        // Ticks since the last fireball / jump - a long gap makes that action the preferred
        // next choice, so a fight shows variety instead of only ground strikes.
        int ticksSinceFireball = 0;
        int ticksSinceJump = 0;
        // Gap before the next fireball is preferred - re-rolled after each throw so two brains
        // do not fall into step.
        int fireballGapTicks = 420;
        uint32_t lastProjectileId = 0;
        // After answering a fireball (from its cast) the same ball is not answered again.
        int fireballReactionLock = 0;
    };

    // Seeded generator shared by brains and the director: deterministic for a given seed.
    float AiRand01(uint32_t& state);

    void ResetAiBrain(AiBrain& brain, uint32_t seed);

    // Standard tunings for the preset opponents.
    enum class AiProfile
    {
        Passive,
        Reactive,
        Aggressive,
    };
    AiTuning TuningForProfile(AiProfile profile);

    // Advances one fixed tick and returns the inputs to hold.
    AiIntent TickAiBrain(
        AiBrain& brain, const AiFighterView& self, const AiFighterView& opponent, AiRole role, const AiTuning& tuning);

    // Converts an intent into a command, deriving pressed/released edges from `previous`.
    void ApplyAiIntent(const AiIntent& intent, uint32_t tick, const Engine::InputCommand& previous, Engine::InputCommand& out);

    // Steers two brains into a back-and-forth fight: alternates which fighter is on the
    // offensive, makes the other guard imperfectly, forces action whenever nothing has happened
    // for a moment, and lets the round end in a knockout after a set time. Only the brains'
    // choices are shaped - hits, blocks and damage are real results.
    struct FightDirector
    {
        uint32_t seed = 1;
        float fightLengthSeconds = 70.0f;

        AiRole role[2] = { AiRole::Neutral, AiRole::Neutral };
        AiTuning tuning[2];
        float elapsedSeconds = 0.0f;
        bool finale = false;

        void Reset(uint32_t newSeed, int health0, int health1);

        // Once per fixed tick. `activity`: either fighter is attacking or stunned this tick.
        void Update(float deltaSeconds, int health0, int health1, bool activity);

    private:
        enum class Phase
        {
            Neutral,
            Offense,
        };
        Phase m_phase = Phase::Neutral;
        float m_phaseSeconds = 0.0f;
        float m_neutralSeconds = 0.8f;
        float m_quietSeconds = 0.0f;
        int m_maxHealth = 100;
        bool m_allIn = false;
        float m_urgentSeconds = 0.0f;
        int m_attacker = 0;
        int m_hitsThisOffense = 0;
        int m_swapAfterHits = 3;
        int m_lastHealth[2] = { 100, 100 };
        uint32_t m_rng = 1;

        void BeginNeutral();
        void BeginOffense(int attacker);
        // Who should be pressing: the offense alternates between the fighters, except that a
        // fighter that is nearly out of health takes it (a last stand) before the lethal phase.
        int ChooseAttacker(int health0, int health1) const;
        void ApplyTunings();
    };
}

#endif // OMD_PROTOTYPE
