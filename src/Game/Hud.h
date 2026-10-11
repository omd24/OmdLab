#pragma once

#if defined(OMD_PROTOTYPE)

namespace Game
{
    // TODO(OM): replace with a screen-space UI renderer (Renderer::UIPass) with real text,
    // layout and per-fighter portraits; this draws through the ImGui background draw list.

    struct HudInput
    {
        int health[2] = { 0, 0 }; // Left / right fighter.
        int maxHealth[2] = { 1, 1 };
        const char* name[2] = { "", "" };
        bool bannerVisible = false;
        float bannerAlpha = 0.0f;
        const char* bannerTitle = "K.O.";
        const char* bannerSubtitle = "";
    };

    // Animation state that persists between frames (the lagging damage segment).
    struct HudState
    {
        float trail[2] = { 1.0f, 1.0f };
        float holdSeconds[2] = { 0.0f, 0.0f };
        float lastFraction[2] = { 1.0f, 1.0f };
    };

    // Draws the overlay onto ImGui's background draw list: it is not a window, takes no input,
    // and is unaffected by the debug-window toggle.
    void DrawHud(HudState& state, const HudInput& input, float deltaSeconds);
}

#endif // OMD_PROTOTYPE
