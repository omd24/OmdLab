#pragma once

#if defined(OMD_PROTOTYPE)

namespace Game
{
    // Runtime switches for the provisional features, adjustable from the debug window. A
    // scripted capture run turns everything off (see main.cpp) so its output stays comparable.
    struct PrototypeConfig
    {
        bool hud = true;       // Health bars and the K.O. banner.
        bool matchFlow = true; // K.O. slow-motion, banner hold and automatic round reset.
        bool juice = true;     // Hit flash, camera shake, extra K.O. hitstop.
        bool backdrop = true;  // Picture of a dungeon wall behind the stage, with torch fire.
        bool vfx = true;       // Hit sparks, dust puffs, fireball glow and trail.
        bool sfx = true;       // Attack, fireball, movement and K.O. sounds (per-family switches in SfxCues).
        bool music = true;     // Looping music track.
        float sfxVolume = 1.05f;
        float musicVolume = 0.10f;
    };
}

#endif // OMD_PROTOTYPE
