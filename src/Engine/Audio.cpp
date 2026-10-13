#include "Audio.h"

#include "Foundation/Log.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// The bundled library is large and warns at this project's warning level; it is not ours to fix.
#pragma warning(push, 0)
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio.h"
#pragma warning(pop)

namespace Engine
{
    namespace Audio
    {
        namespace
        {
            constexpr size_t kVoiceCount = 32;

            struct Voice
            {
                ma_sound sound;
                bool inUse = false;
                int group = 0;
                uint32_t startOrder = 0;
            };

            ma_engine g_engine;
            bool g_ready = false;
            std::vector<std::string> g_paths; // SoundId -> resolved file path
            Voice* g_voices = nullptr;
            ma_sound g_music;
            bool g_musicActive = false;

            constexpr size_t kLoopCount = 4;
            struct LoopSlot
            {
                ma_sound sound;
                bool inUse = false;
            };
            LoopSlot g_loops[kLoopCount];

            bool Resolve(const std::string& name, std::string& outPath)
            {
                namespace fs = std::filesystem;
                for (const char* folder : { "local/audio/", "data/audio/" })
                {
                    const std::string path = std::string(folder) + name + ".wav";
                    if (fs::exists(path))
                    {
                        outPath = path;
                        return true;
                    }
                }
                return false;
            }
        }

        bool Init()
        {
            if (g_ready)
            {
                return true;
            }
            if (ma_engine_init(nullptr, &g_engine) != MA_SUCCESS)
            {
                Foundation::Log::Write(Foundation::Log::Severity::Warning, "Audio", "no audio device - sound disabled");
                return false;
            }
            ma_engine_set_volume(&g_engine, 0.8f); // headroom for overlapping sounds
            g_voices = new Voice[kVoiceCount];
            std::memset(static_cast<void*>(g_voices), 0, sizeof(Voice) * kVoiceCount);
            g_ready = true;
            Foundation::Log::Write(Foundation::Log::Severity::Info, "Audio", "audio initialized (%u Hz)", ma_engine_get_sample_rate(&g_engine));
            return true;
        }

        void Shutdown()
        {
            if (!g_ready)
            {
                return;
            }
            StopMusic();
            for (LoopSlot& loop : g_loops)
            {
                if (loop.inUse)
                {
                    ma_sound_uninit(&loop.sound);
                    loop.inUse = false;
                }
            }
            for (size_t i = 0; i < kVoiceCount; ++i)
            {
                if (g_voices[i].inUse)
                {
                    ma_sound_uninit(&g_voices[i].sound);
                }
            }
            delete[] g_voices;
            g_voices = nullptr;
            ma_engine_uninit(&g_engine);
            g_paths.clear();
            g_ready = false;
        }

        SoundId Load(const char* name)
        {
            if (!g_ready)
            {
                return kInvalidSound;
            }
            std::string path;
            if (!Resolve(name, path))
            {
                return kInvalidSound;
            }
            for (size_t i = 0; i < g_paths.size(); ++i)
            {
                if (g_paths[i] == path)
                {
                    return static_cast<SoundId>(i);
                }
            }
            // Decoded into the resource manager once; each Play() instance shares that data.
            ma_resource_manager_register_file(ma_engine_get_resource_manager(&g_engine), path.c_str(), MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_DECODE);
            g_paths.push_back(path);
            return static_cast<SoundId>(g_paths.size() - 1);
        }

        void Play(SoundId sound, float volume, float pitch, int group)
        {
            if (!g_ready || sound < 0 || static_cast<size_t>(sound) >= g_paths.size())
            {
                return;
            }
            // Group limit: a group keeps at most kMaxPerGroup voices playing; starting another
            // fades the oldest ones out over ~45 ms (so a fast string of hits overlaps by one
            // tail instead of stacking, but no hit is cut off short).
            if (group != 0)
            {
                constexpr int kMaxPerGroup = 2;
                for (;;)
                {
                    Voice* oldest = nullptr;
                    int playing = 0;
                    for (size_t i = 0; i < kVoiceCount; ++i)
                    {
                        Voice& other = g_voices[i];
                        if (other.inUse && other.group == group && ma_sound_is_playing(&other.sound) && !ma_sound_at_end(&other.sound))
                        {
                            ++playing;
                            if (oldest == nullptr || other.startOrder < oldest->startOrder)
                            {
                                oldest = &other;
                            }
                        }
                    }
                    if (playing < kMaxPerGroup || oldest == nullptr)
                    {
                        break;
                    }
                    ma_sound_stop_with_fade_in_milliseconds(&oldest->sound, 45);
                    oldest->group = 0; // Already on its way out: no longer counted.
                }
            }
            // Reuse a finished voice, or drop the sound if all are busy.
            Voice* voice = nullptr;
            for (size_t i = 0; i < kVoiceCount; ++i)
            {
                Voice& candidate = g_voices[i];
                if (candidate.inUse && (ma_sound_at_end(&candidate.sound) || !ma_sound_is_playing(&candidate.sound)))
                {
                    ma_sound_uninit(&candidate.sound);
                    candidate.inUse = false;
                }
                if (!candidate.inUse && voice == nullptr)
                {
                    voice = &candidate;
                }
            }
            if (voice == nullptr)
            {
                return;
            }
            if (ma_sound_init_from_file(&g_engine, g_paths[static_cast<size_t>(sound)].c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                        nullptr, nullptr, &voice->sound) != MA_SUCCESS)
            {
                return;
            }
            ma_sound_set_volume(&voice->sound, volume);
            ma_sound_set_pitch(&voice->sound, pitch);
            ma_sound_start(&voice->sound);
            voice->inUse = true;
            voice->group = group;
            static uint32_t s_order = 0;
            voice->startOrder = ++s_order;
        }

        void StopGroup(int group, int fadeMilliseconds)
        {
            if (!g_ready || group == 0)
            {
                return;
            }
            for (size_t i = 0; i < kVoiceCount; ++i)
            {
                Voice& voice = g_voices[i];
                if (voice.inUse && voice.group == group && ma_sound_is_playing(&voice.sound) && !ma_sound_at_end(&voice.sound))
                {
                    ma_sound_stop_with_fade_in_milliseconds(&voice.sound, static_cast<ma_uint64>(fadeMilliseconds));
                    voice.group = 0;
                }
            }
        }

        LoopId StartLoop(SoundId sound, float volume)
        {
            if (!g_ready || sound < 0 || static_cast<size_t>(sound) >= g_paths.size())
            {
                return -1;
            }
            for (size_t i = 0; i < kLoopCount; ++i)
            {
                if (g_loops[i].inUse)
                {
                    continue;
                }
                if (ma_sound_init_from_file(&g_engine, g_paths[static_cast<size_t>(sound)].c_str(),
                                            MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr,
                                            &g_loops[i].sound) != MA_SUCCESS)
                {
                    return -1;
                }
                ma_sound_set_looping(&g_loops[i].sound, MA_TRUE);
                ma_sound_set_volume(&g_loops[i].sound, volume);
                ma_sound_start(&g_loops[i].sound);
                g_loops[i].inUse = true;
                return static_cast<LoopId>(i);
            }
            return -1;
        }

        void StopLoop(LoopId loop)
        {
            if (g_ready && loop >= 0 && static_cast<size_t>(loop) < kLoopCount && g_loops[loop].inUse)
            {
                ma_sound_uninit(&g_loops[loop].sound);
                g_loops[loop].inUse = false;
            }
        }

        void SetLoopVolume(LoopId loop, float volume)
        {
            if (g_ready && loop >= 0 && static_cast<size_t>(loop) < kLoopCount && g_loops[loop].inUse)
            {
                ma_sound_set_volume(&g_loops[loop].sound, volume);
            }
        }

        void PlayMusic(const char* name, float volume)
        {
            if (!g_ready)
            {
                return;
            }
            StopMusic();
            std::string path;
            if (!Resolve(name, path))
            {
                return;
            }
            if (ma_sound_init_from_file(&g_engine, path.c_str(), MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, nullptr, &g_music) !=
                MA_SUCCESS)
            {
                return;
            }
            ma_sound_set_looping(&g_music, MA_TRUE);
            ma_sound_set_volume(&g_music, volume);
            ma_sound_start(&g_music);
            g_musicActive = true;
        }

        void StopMusic()
        {
            if (g_musicActive)
            {
                ma_sound_uninit(&g_music);
                g_musicActive = false;
            }
        }

        void SetMusicVolume(float volume)
        {
            if (g_musicActive)
            {
                ma_sound_set_volume(&g_music, volume);
            }
        }

        void SetMasterVolume(float volume)
        {
            if (g_ready)
            {
                ma_engine_set_volume(&g_engine, volume);
            }
        }
    }
}
