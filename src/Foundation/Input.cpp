#include "Input.h"

#ifdef OMD_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Xinput.h>

namespace
{
    float ApplyStickDeadzone(SHORT raw, SHORT deadzone)
    {
        if (raw > deadzone)
        {
            return static_cast<float>(raw - deadzone) / static_cast<float>(32767 - deadzone);
        }
        if (raw < -deadzone)
        {
            return static_cast<float>(raw + deadzone) / static_cast<float>(32768 - deadzone);
        }
        return 0.0f;
    }

    float ApplyTriggerDeadzone(BYTE raw)
    {
        return raw > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? static_cast<float>(raw) / 255.0f : 0.0f;
    }
}

namespace Foundation
{
    bool IsKeyDown(int virtualKey)
    {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    MouseState GetMouseState()
    {
        MouseState state;
        POINT cursor;
        GetCursorPos(&cursor);
        state.x = cursor.x;
        state.y = cursor.y;
        state.leftDown = IsKeyDown(VK_LBUTTON);
        state.rightDown = IsKeyDown(VK_RBUTTON);
        state.middleDown = IsKeyDown(VK_MBUTTON);
        return state;
    }

    GamepadState GetGamepadState(int controllerIndex)
    {
        GamepadState state;

        XINPUT_STATE rawState = {};
        if (XInputGetState(static_cast<DWORD>(controllerIndex), &rawState) != ERROR_SUCCESS)
        {
            return state; // connected stays false - an empty slot, not an error.
        }
        state.connected = true;

        const XINPUT_GAMEPAD& pad = rawState.Gamepad;
        state.leftStickX = ApplyStickDeadzone(pad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        state.leftStickY = ApplyStickDeadzone(pad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        state.rightStickX = ApplyStickDeadzone(pad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        state.rightStickY = ApplyStickDeadzone(pad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        state.leftTrigger = ApplyTriggerDeadzone(pad.bLeftTrigger);
        state.rightTrigger = ApplyTriggerDeadzone(pad.bRightTrigger);
        state.buttonA = (pad.wButtons & XINPUT_GAMEPAD_A) != 0;
        state.buttonB = (pad.wButtons & XINPUT_GAMEPAD_B) != 0;
        state.buttonX = (pad.wButtons & XINPUT_GAMEPAD_X) != 0;
        state.buttonY = (pad.wButtons & XINPUT_GAMEPAD_Y) != 0;
        state.leftShoulder = (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        state.rightShoulder = (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        state.dpadUp = (pad.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
        state.dpadDown = (pad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        state.dpadLeft = (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        state.dpadRight = (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        return state;
    }
}

#endif
