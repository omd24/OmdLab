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
    };
}

#endif // OMD_PROTOTYPE
