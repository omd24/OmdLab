#pragma once

#if defined(OMD_PROTOTYPE)

namespace Game
{
    // TODO(OM): replace with a screen-space UI renderer (Renderer::UIPass) with real text,
    // layout and a shared menu/pause/options flow; this draws through the ImGui background draw
    // list and polls the keyboard, mouse and first gamepad directly.

    enum class MenuResult
    {
        None,
        Play,
        Quit,
    };

    struct MainMenuState
    {
        int selected = 0;
        bool optionsOpen = false; // The (stub) options page.
        float time = 0.0f;        // Seconds since the menu appeared, for the fade-in and flicker.

        // Previous-frame key states, for edge detection.
        bool upWas = false;
        bool downWas = false;
        bool confirmWas = false;
        bool backWas = false;
        bool clickWas = false;
    };

    // The screen after a knockout in the demo flow: a countdown and "rematch" / "cancel".
    struct ContinueState
    {
        static constexpr float kStartSeconds = 9.0f;

        float secondsLeft = kStartSeconds;
        int selected = 0; // 0 = rematch, 1 = cancel.
        float time = 0.0f;

        // Previous-frame states, for edge detection (start "held" so a button still down from
        // the fight does not choose anything).
        bool upWas = true;
        bool downWas = true;
        bool confirmWas = true;
        bool clickWas = true;
    };

    enum class ContinueResult
    {
        None,
        Rematch,
        Cancel, // Chosen, or the countdown ran out.
    };

    // Draws the screen and handles its input (keyboard and gamepad only while `focused`).
    ContinueResult DrawContinueScreen(ContinueState& state, float deltaSeconds, bool focused);

    // Restarts the fade-in (call when the menu is shown again).
    void ResetMainMenu(MainMenuState& state);

    // Draws the menu and handles its input. Keyboard and gamepad are ignored while `focused` is
    // false. Returns what the player chose this frame.
    MenuResult DrawMainMenu(MainMenuState& state, float deltaSeconds, bool focused);
}

#endif // OMD_PROTOTYPE
