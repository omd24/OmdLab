#pragma once

#ifdef OMD_WINDOWS

namespace Foundation
{
    // Thin platform wrapper over raw keyboard/mouse/gamepad polling - no game concept here
    // (no "button", no "player"), same "OS abstraction, even though it feels Engine-y" role
    // Window.h already plays for window/console creation. Engine::Input assembles these into
    // a game-shaped per-tick command.
    //
    // Polling, not message-based, for every source here - matches the free-fly camera's own
    // established reasoning (see Engine/Camera.h): GetAsyncKeyState/XInputGetState work
    // regardless of which window has focus, unlike WM_* messages (confirmed unreliable for
    // this app's mouse wheel specifically).

    // virtualKey: a Win32 VK_* constant or ASCII letter/digit code (GetAsyncKeyState's own
    // convention, e.g. 'W', VK_LEFT, VK_LBUTTON).
    bool IsKeyDown(int virtualKey);

    struct MouseState
    {
        long x = 0;
        long y = 0; // Screen coordinates, same as GetCursorPos - caller computes deltas.
        bool leftDown = false;
        bool rightDown = false;
        bool middleDown = false;
    };
    MouseState GetMouseState();

    struct GamepadState
    {
        bool connected = false; // false if this slot has no gamepad attached - not an error.
        float leftStickX = 0.0f, leftStickY = 0.0f;   // [-1, 1] each axis, deadzone applied.
        float rightStickX = 0.0f, rightStickY = 0.0f; // [-1, 1] each axis, deadzone applied.
        float leftTrigger = 0.0f, rightTrigger = 0.0f; // [0, 1], deadzone applied.
        bool buttonA = false, buttonB = false, buttonX = false, buttonY = false;
        bool leftShoulder = false, rightShoulder = false;
        bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
    };
    // controllerIndex: XInput slot 0-3.
    GamepadState GetGamepadState(int controllerIndex);
}

#endif
