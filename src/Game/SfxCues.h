#pragma once

#if defined(OMD_PROTOTYPE)

#include "Engine/Audio.h"
#include "Foundation/Log.h"

#include <cmath>
#include <cstdint>
#include <string>

// Variation of the cues. 1: a random variant per play, +-6% pitch jitter and probabilistic
// extras (grunts). 0: fully deterministic - every cue plays variant SFX_FIXED_VARIANT of its set
// (clamped to the set size), at its authored pitch, with every layer present - for judging and
// tuning individual sounds.
// 0: hit sounds from the tracked CC0 set in data/audio; 1: the untracked local/audio recordings.
#ifndef HIT_SFX_LOCAL
#define HIT_SFX_LOCAL 0
#endif

#ifndef RANDOM_SFX
#define RANDOM_SFX 0
#endif
#ifndef SFX_FIXED_VARIANT
#define SFX_FIXED_VARIANT 3
#endif

namespace Game
{
    // TODO(OM): data-driven cue assignment (sfx blocks keyed on move and frame, per-character
    // voice sets) and a mixer with buses; these cues are chosen in code from game events.

    // Sound names are looked up in local/audio, then data/audio (see Engine::Audio). Any name
    // without a file is silently skipped, so the game plays with whatever content is present.
    class SfxCues
    {
    public:
        // Which families of cues are on. Defaults reflect what was judged good in play: the
        // attack sounds of the knockout slow-motion, the fireball, and the quiet run sounds;
        // the swing/voice of ordinary attacks and jump/land are off.
        struct Categories
        {
            bool attackAlways = false;  // Swing + voice on every attack.
            bool attackAfterKo = false; // Swing + voice only from the knockout to the next round.
            bool jump = false;          // Jump and landing.
            bool fireball = true;       // Cast and the in-flight roar.
            bool run = true;            // Run roar and splash loop.
            bool koCry = true;
            bool hit = true;            // The regular-hit sound.
            bool whiff = true;          // The swing sound of an attack that did not connect.
            float whiffVolume = 0.25f;   // Level of the whiff swing (before the overall SFX volume).
            float koVolume = 0.4f;       // Level of the K.O. cry relative to its original (loud) level.
            float hitVolume = 0.4f;     // Level of a regular hit (13+ damage plays it 25% louder).

            // The recorded set (hit_own_*, whiff_own_*, voice_*, ko_own). Levels are relative to
            // the file level; the files themselves are loudness-matched per family.
            bool own = true;             // Use the recorded set for hits, whiffs, attack voice and K.O.
            bool voice = true;           // The attacker's voice on attacks.
            float ownHitVolume = 0.9f;
            float ownWhiffVolume = 0.65f;
            float ownVoiceVolume = 0.5f;     // Attack voice: kept under the hits and whiffs.
            float voiceChance = 0.4f;        // Chance a light/medium attack (opener) has a voice at all...
            float heavyVoiceChance = 0.75f;  // ...and a heavy one (a string's ender).
            float ownKoVolume = 0.85f;
            // The older recordings stay as a quiet extra layer now and then (0 chance = off).
            float legacyChance = 0.35f;
            float legacyVolume = 0.3f;

            // Running: the roar when a run starts and the footstep loop.
            float runRoarVolume = 0.75f;
            float runLoopVolume = 0.22f;
        };
        Categories categories;

        void Load()
        {
            LoadSet(m_gruntLight, { "grunt_1", "grunt_2", "grunt_3", "grunt_4" });
            LoadSet(m_gruntHeavy, { "grunt_heavy_1", "grunt_heavy_2" });
            LoadSet(m_swish, { "swish_1", "swish_2", "swish_3", "swish_4" });
            LoadSet(m_whooshHeavy, { "swish_3", "swish_2" }); // dull recordings; the noise-synth whooshes sounded metallic
#if HIT_SFX_LOCAL
            LoadSet(m_hitLight, { "hit_light_1", "hit_light_2", "hit_light_3", "hit_light_4" });
#else
            LoadSet(m_hitLight, { "hit_cc0_1", "hit_cc0_2", "hit_cc0_3", "hit_cc0_4" });
#endif
            LoadSet(m_whiff, { "whiff_cc0_1", "whiff_cc0_2" });
            LoadSet(m_hitHeavy, { "hit_heavy_1", "hit_heavy_2" });
            LoadSet(m_block, { "block_1", "block_2" });
            LoadSet(m_crushLight, { "crush_light_1", "crush_light_2" });
            LoadSet(m_crushHeavy, { "crush_heavy_1", "crush_heavy_2", "crush_heavy_ai" });
            LoadSet(m_hitOwn, { "hit_own_1", "hit_own_2", "hit_own_3" });
            LoadSet(m_whiffOwn, { "whiff_own_1", "whiff_own_2" });
            LoadSet(m_voiceMedium, { "voice_medium_1" });
            LoadSet(m_voiceHeavy, { "voice_heavy_1", "voice_heavy_2" });
            m_voiceFireball = Engine::Audio::Load("voice_fireball");
            m_koOwn = Engine::Audio::Load("ko_own");
            m_fightOwn = Engine::Audio::Load("fight_own");
            // The recorded sets always vary (never the same variant twice in a row), whatever
            // the global randomisation switch says.
            for (Set* set : { &m_hitOwn, &m_whiffOwn, &m_voiceMedium, &m_voiceHeavy })
            {
                set->randomAlways = true;
            }
            m_fireballCast = Engine::Audio::Load("fireball_cast");
            m_fireballImpact = Engine::Audio::Load("fireball_impact");
            m_fireballCancel = Engine::Audio::Load("fireball_cancel");
            m_koBoom = Engine::Audio::Load("ko_boom");
            m_koCry = Engine::Audio::Load("ko_yell2");
            m_attackYell = Engine::Audio::Load("attack_yell");
            m_jump = Engine::Audio::Load("jump");
            m_land = Engine::Audio::Load("land");
            m_run = Engine::Audio::Load("run_loop");
            m_fireLoopSound = Engine::Audio::Load("fireball_loop");
            LoadSet(m_roar, { "run_roar", "run_roar2" });
            m_roar.randomAlways = true;
            m_hitLight.randomAlways = true; // hits vary between the five recordings
            // The heavy sets (used by the combo enders: the third punch / kick of a string, and any
            // hit of 13+ damage) keep their first variant whatever the global one is.
            m_whooshHeavy.pinned = 0;
            m_gruntHeavy.pinned = 0;
            m_hitHeavy.pinned = 0;
            m_crushHeavy.pinned = 0;
        }

        // `continuation`: this move was cancelled into from another attack (a link in a string).
        // Openers and middle links share a gentle level; the heavy ender sits at its normal level.
        void OnAttackStarted(const std::string& moveId, bool continuation)
        {
            const bool heavy = moveId.find("heavy") != std::string::npos;
            const bool fireball = moveId == "fireball";
            PlayAttackVoice(moveId, heavy, fireball, continuation);
            if (fireball ? !categories.fireball
                         : !(categories.attackAlways || (categories.attackAfterKo && m_afterKnockout)))
            {
                return;
            }
            // From the knockout to the next round the attack sounds play plainly: every swing
            // and voice, at its authored level, with no thinning or ducking.
            const bool plain = m_afterKnockout && !fireball;
            if (m_sparse && !fireball)
            {
                // Sparse: the middle links of a string stay silent (the opener and the heavy
                // ender carry it), and swings are at least 0.7 s apart.
                if ((continuation && !heavy) || m_time - m_lastSwing < 0.7f)
                {
                    return;
                }
            }
            m_lastSwing = m_time;
            m_recent += 1.0f;
            // The opener plays at the same (gentler) level as the second link of a string - the
            // harder level it had before sounded worse; only the heavy ender is at full level.
            m_eventGain = plain ? 1.0f : ((continuation && heavy) ? 1.0f : 0.55f);
            if (fireball)
            {
                Out(m_fireballCast, kLevelSwing * 1.25f);
                return;
            }
            // Most attacks are just the swing; the voice only comes through now and then, so
            // a fast exchange does not turn into a wall of shouting.
            if (heavy)
            {
                PlayRandom(m_whooshHeavy, kLevelSwing * 1.25f, 1.0f, kGroupSwing);
                if (Chance(0.5f) && GruntAllowed())
                {
                    // Now and then the grunt is a full yell, never twice within a few seconds.
                    if (Chance(0.2f) && m_time - m_lastYell > 6.0f)
                    {
                        m_lastYell = m_time;
                        Out(m_attackYell, kLevelVoice * 1.1f, 1.0f, kGroupVoice);
                    }
                    else
                    {
                        PlayRandom(m_gruntHeavy, kLevelVoice * 1.1f, 0.95f, kGroupVoice);
                    }
                }
            }
            else
            {
                PlayRandom(m_swish, kLevelSwing, 1.0f, kGroupSwing);
                if (Chance(0.22f) && GruntAllowed())
                {
                    PlayRandom(m_gruntLight, kLevelVoice * 0.9f, 1.0f, kGroupVoice);
                }
            }
        }

        void OnHit(int damage, bool blocked, bool fireball, bool knockout)
        {
            if (knockout)
            {
                // Lower than before, never ducked by what came just before it - and what follows
                // it (the next round's first attacks) is held down for a couple of seconds.
                // No boom: just the cry, never ducked by what came before it.
                if (categories.own && m_koOwn != Engine::Audio::kInvalidSound)
                {
                    Out(m_koOwn, categories.ownKoVolume, 1.0f, kGroupKo, false);
                }
                else if (categories.koCry)
                {
                    Out(m_koCry, kLevelVoice * 1.75f * categories.koVolume, 1.0f, kGroupVoice, false);
                }
                m_afterKnockout = true;
                return;
            }
            if (m_sparse && m_time - m_lastHit < 0.08f)
            {
                return; // Sparse: only a double trigger of one hit is dropped - every hit of a string sounds.
            }
            m_lastHit = m_time;
            BeginEvent();
            if (fireball)
            {
                Out(m_fireballImpact, blocked ? 0.45f : 0.7f);
                return;
            }
            // A regular hit: one very soft thud (nothing for a block, which just pushes back).
            if (categories.own && categories.hit && !blocked && m_hitOwn.count > 0)
            {
                PlayRandom(m_hitOwn, categories.ownHitVolume * (damage >= 13 ? 1.15f : 1.0f), 1.0f, kGroupImpact, false);
                if (Next() < categories.legacyChance)
                {
                    PlayRandom(m_hitLight, categories.legacyVolume * categories.hitVolume, 1.0f, kGroupLegacy);
                }
            }
            else if (categories.hit && !blocked)
            {
                PlayRandom(m_hitLight, categories.hitVolume * (damage >= 13 ? 1.25f : 1.0f), 1.0f, kGroupImpact);
            }
        }

        // Played a few ticks into the strike of an attack that has not connected.
        void OnWhiff()
        {
            if (!categories.whiff || m_time - m_lastWhiff < 0.5f)
            {
                return;
            }
            m_lastWhiff = m_time;
            if (categories.own && m_whiffOwn.count > 0)
            {
                PlayRandom(m_whiffOwn, categories.ownWhiffVolume, 1.0f, kGroupSwing, false);
                if (Next() < categories.legacyChance)
                {
                    PlayRandom(m_whiff, categories.legacyVolume * categories.whiffVolume, 1.0f, kGroupLegacy);
                }
                return;
            }
            BeginEvent();
            PlayRandom(m_whiff, categories.whiffVolume, 1.0f, kGroupSwing);
        }

        // The round-start announcement ("fight").
        void OnFight()
        {
            if (categories.own && m_fightOwn != Engine::Audio::kInvalidSound)
            {
                Out(m_fightOwn, 0.9f, 1.0f, kGroupVoice, false);
            }
        }

        void OnFireballCancelled()
        {
            BeginEvent();
            Out(m_fireballCancel, 0.8f);
        }
        void OnJump()
        {
            if (!categories.jump)
            {
                return;
            }
            BeginEvent();
            Out(m_jump, kLevelVoice * 0.5f);
        }
        void OnLand()
        {
            if (!categories.jump)
            {
                return;
            }
            BeginEvent();
            Out(m_land, kLevelVoice * 0.5f);
        }

        // A flickering roar while any fireball is in flight (one shared loop).
        void SetFireballInFlight(bool inFlight)
        {
            if (!categories.fireball)
            {
                inFlight = false;
            }
            if (inFlight && m_fireLoop < 0)
            {
                m_fireLoop = Engine::Audio::StartLoop(m_fireLoopSound, 0.09f * m_scale);
            }
            else if (!inFlight && m_fireLoop >= 0)
            {
                Engine::Audio::StopLoop(m_fireLoop);
                m_fireLoop = -1;
            }
        }

        // A footstep loop while a fighter runs; the handle is the caller's to hold.
        Engine::Audio::LoopId StartRun() { return categories.run ? Engine::Audio::StartLoop(m_run, categories.runLoopVolume * m_scale) : -1; }
        void StopRun(Engine::Audio::LoopId loop) { Engine::Audio::StopLoop(loop); }

        // Live switches for judging cues: random picks on/off, and which variant of every set
        // plays while random is off (wraps after the largest set, 4).
        bool Randomized() const { return m_randomize; }
        void SetRandomized(bool randomized) { m_randomize = randomized; }
        int FixedVariant() const { return m_fixedVariant; }
        void CycleVariant() { m_fixedVariant = (m_fixedVariant + 1) % 4; }

        // TODO(OM): a finisher voice for a knockout that lands in the off-limit area (see
        // data/audio/unused/finisher_yell.wav); data/audio/unused/roar_alt.wav is an alternative roar.

        // The roar when a run starts, at most once every ~1.5 s.
        void OnRunStart(float nowSeconds)
        {
            if (!categories.run || nowSeconds - m_lastRoarSeconds < 1.5f)
            {
                return;
            }
            m_lastRoarSeconds = nowSeconds;
            BeginEvent();
            PlayRandom(m_roar, categories.runRoarVolume, 1.0f, kGroupRun, false);
        }

    public:
        // Overall level for every cue (the SFX volume slider).
        void SetScale(float scale) { m_scale = scale; }

        // After a knockout the exchange is over: every effect is silent until the round restarts.
        void OnRoundReset()
        {
            Engine::Audio::StopGroup(kGroupKo, 300); // The K.O. sound ends with the round.
            m_afterKnockout = false;
            m_recent = 0.0f;
        }

        // Once per render frame (real seconds): lets the "recent sounds" level fall back.
        void Tick(float deltaSeconds)
        {
            m_recent *= std::exp(-deltaSeconds / 1.2f);
            m_time += deltaSeconds;
        }

        // Sparse mode plays far fewer sounds: no swing sound for the middle links of a string,
        // swings >= 0.7 s apart, hit sounds >= 0.35 s apart, voice >= 1.4 s apart - about the
        // density of a knockout slow-motion, where every sound is heard on its own.
        bool Sparse() const { return m_sparse; }
        void SetSparse(bool sparse) { m_sparse = sparse; }

    private:
        // Every one-shot is scaled by how much has just played: the first sound after a quiet
        // moment is at full level, the ones right behind it much lower (1 / (1 + 0.7 * recent)),
        // so a flurry or a post-knockout exchange does not pile up loudly.
        // Called once at the start of an event (a hit, a jump...): fixes the level all of that
        // event's layers share; events that follow other sounds quickly are ducked. (Attacks set
        // their own level from their position in a string, see OnAttackStarted.)
        void BeginEvent()
        {
            m_eventGain = 1.0f / (1.0f + 0.7f * m_recent);
            m_recent += 1.0f;
        }

        void Out(Engine::Audio::SoundId sound, float volume, float pitch = 1.0f, int group = 0, bool duck = true)
        {
            if (m_muteAfterKnockout && m_afterKnockout && duck)
            {
                return; // Silent between the knockout and the next round.
            }
            Engine::Audio::Play(sound, volume * m_scale * (duck ? m_eventGain : 1.0f), pitch, group);
        }

        // Reference cue levels. Every one-shot file is normalized to the same loudness (see
        // scripts/audio_build.py), so these are directly comparable: swings and impacts sit at
        // one level, the voice a little under, the heavier cues a touch above.
        static constexpr float kLevelSwing = 0.2f;
        static constexpr float kLevelVoice = 0.2f;

        // Whether everything but the knockout cry is muted from the knockout to the next round.
        // Off: the knockout slow-motion keeps its attack sounds.
        bool m_muteAfterKnockout = false;
        static constexpr int kGroupImpact = 1;
        static constexpr int kGroupVoice = 2;
        static constexpr int kGroupSwing = 3;
        static constexpr int kGroupKo = 4;
        static constexpr int kGroupLegacy = 5;
        static constexpr int kGroupRun = 6;

        struct Set
        {
            Engine::Audio::SoundId ids[8];
            const char* names[8];
            int count = 0;
            int last = -1; // Variant played last, to avoid an immediate repeat.
            int pinned = -1; // >= 0: the variant used in deterministic mode regardless of the global one.
            bool randomAlways = false; // Picks a random variant even when random SFX is off.
        };

        static void LoadSetImpl(Set& set, const char* name)
        {
            const Engine::Audio::SoundId id = Engine::Audio::Load(name);
            if (id != Engine::Audio::kInvalidSound && set.count < 8)
            {
                set.names[set.count] = name;
                set.ids[set.count++] = id;
            }
        }

        template <size_t N>
        void LoadSet(Set& set, const char* const (&names)[N])
        {
            for (const char* name : names)
            {
                LoadSetImpl(set, name);
            }
        }

        // Presentation-only randomness: not part of the simulation, so a plain LCG is fine.
        float Next()
        {
            m_rng = m_rng * 1664525u + 1013904223u;
            return static_cast<float>(m_rng >> 8) / 16777216.0f;
        }
        bool Chance(float p)
        {
            if (!m_randomize)
            {
                return true; // Deterministic mode: every layer is present.
            }
            return Next() < p;
        }

        void PlayRandom(Set& set, float volume, float basePitch, int group = 0, bool duck = true)
        {
            if (set.count == 0)
            {
                return;
            }
            int index = 0;
            float pitch = basePitch;
            if (m_randomize || set.randomAlways)
            {
                index = static_cast<int>(Next() * static_cast<float>(set.count)) % set.count;
                if (set.count > 1 && index == set.last)
                {
                    // Never the same variant twice in a row.
                    index = (index + 1 + static_cast<int>(Next() * static_cast<float>(set.count - 1))) % set.count;
                }
                set.last = index;
                pitch = basePitch * (0.94f + 0.12f * Next());
            }
            else
            {
                const int wanted = set.pinned >= 0 ? set.pinned : m_fixedVariant;
                index = wanted < set.count ? wanted : set.count - 1;
            }
            Foundation::Log::Write(Foundation::Log::Severity::Info, "Sfx", "%s vol=%.2f", set.names[index], volume);
            Out(set.ids[index], volume, pitch, group, duck);
        }

        // The attacker's voice: a medium or heavy recording by the move (the higher-pitched light
        // one, voice_light_1, is not used), the fireball throw by itself. The most repeated
        // attacks (light and medium openers, which are also the ones that miss most) only have a
        // voice now and then and never two in a row; middle links of a string stay silent and
        // voices keep some distance from each other, so a flurry is not a wall of shouting.
        void PlayAttackVoice(const std::string& moveId, bool heavy, bool fireball, bool continuation)
        {
            if (!categories.own || !categories.voice)
            {
                return;
            }
            if (fireball)
            {
                if (categories.fireball && m_voiceFireball != Engine::Audio::kInvalidSound)
                {
                    Out(m_voiceFireball, categories.ownVoiceVolume * 1.4f, 1.0f, kGroupVoice, false);
                }
                return;
            }
            if ((continuation && !heavy) || m_time - m_lastVoice < 0.4f)
            {
                return;
            }
            if (heavy)
            {
                if (Next() >= categories.heavyVoiceChance)
                {
                    return;
                }
            }
            else
            {
                const bool voiced = !m_lastOpenerVoiced && Next() < categories.voiceChance;
                m_lastOpenerVoiced = voiced;
                if (!voiced)
                {
                    return;
                }
            }
            m_lastVoice = m_time;
            PlayRandom(heavy ? m_voiceHeavy : m_voiceMedium, categories.ownVoiceVolume * (heavy ? 1.1f : 1.0f), 1.0f, kGroupVoice,
                       false);
        }

        Set m_gruntLight, m_gruntHeavy, m_swish, m_whooshHeavy, m_hitLight, m_hitHeavy, m_block, m_crushLight, m_crushHeavy, m_roar, m_whiff;
        Set m_hitOwn, m_whiffOwn, m_voiceMedium, m_voiceHeavy;
        float m_lastVoice = -10.0f;
        bool m_lastOpenerVoiced = false;
        Engine::Audio::SoundId m_voiceFireball = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_koOwn = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_fightOwn = Engine::Audio::kInvalidSound;
        float m_lastWhiff = -10.0f;
        float m_lastRoarSeconds = -100.0f;
        bool m_randomize = RANDOM_SFX != 0;
        int m_fixedVariant = SFX_FIXED_VARIANT;
        float m_scale = 1.0f;
        float m_recent = 0.0f; // Decaying count of recent events.
        float m_eventGain = 1.0f;
        float m_time = 0.0f;
        float m_lastSwing = -10.0f;
        float m_lastYell = -100.0f;
        float m_lastGrunt = -10.0f;
        float m_lastHit = -10.0f;
        bool m_sparse = true;

        bool GruntAllowed()
        {
            if (m_sparse && m_time - m_lastGrunt < 1.4f)
            {
                return false;
            }
            m_lastGrunt = m_time;
            return true;
        }
        bool m_afterKnockout = false;
        Engine::Audio::SoundId m_fireballCast = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_attackYell = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_fireballImpact = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_fireballCancel = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_koBoom = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_koCry = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_jump = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_land = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_run = Engine::Audio::kInvalidSound;
        Engine::Audio::SoundId m_fireLoopSound = Engine::Audio::kInvalidSound;
        Engine::Audio::LoopId m_fireLoop = -1;
        uint32_t m_rng = 12345u;
    };
}

#endif // OMD_PROTOTYPE
