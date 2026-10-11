#pragma once

namespace Engine
{
    // Minimal sound playback: named one-shot effects plus one looping music track. A sound
    // name resolves to <name>.wav, searched in local/audio first and then data/audio; a name
    // with neither file is a silent no-op (the game runs without any audio content).
    //
    // TODO(OM): a real audio system - mixer buses, voice budget and priorities, spatialisation,
    // data-driven cue assignment, and a backend of our own instead of the bundled library.
    namespace Audio
    {
        using SoundId = int;
        constexpr SoundId kInvalidSound = -1;

        // Opens the output device. Returns false (and every later call is a no-op) if no device
        // is available.
        bool Init();
        void Shutdown();

        // Registers a sound by name (decoded into memory once). kInvalidSound if the file is missing.
        SoundId Load(const char* name);

        // Fire-and-forget. volume 1.0 = as authored; pitch 1.0 = unchanged. A non-zero `group`
        // chokes: sounds already playing in the same group are faded out (~12 ms) as the new one
        // starts, so rapid repeats (a combo of hits) replace each other instead of piling up.
        void Play(SoundId sound, float volume = 1.0f, float pitch = 1.0f, int group = 0);

        // Loops until stopped; replaces any music already playing.
        void PlayMusic(const char* name, float volume);
        void StopMusic();
        void SetMusicVolume(float volume);

        void SetMasterVolume(float volume);

        // A sound repeated until stopped (movement loops). Returns a handle for StopLoop /
        // SetLoopVolume, or -1 if the sound is invalid or no loop slot is free.
        using LoopId = int;
        LoopId StartLoop(SoundId sound, float volume);
        void StopLoop(LoopId loop);
        void SetLoopVolume(LoopId loop, float volume);
    }
}
