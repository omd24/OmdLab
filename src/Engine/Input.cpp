#include "Input.h"

#include "Foundation/Input.h"

#include <cmath>

namespace
{
    constexpr float kGamepadStickThreshold = 0.2f; // Prefer the stick over its digital fallback only once meaningfully pushed.
    constexpr float kGamepadStickDigitalThreshold = 0.5f; // Threshold for treating a stick direction as a held "button".

    bool GamepadInputHeld(const Foundation::GamepadState& gamepad, Engine::GamepadInput input)
    {
        if (!gamepad.connected || input == Engine::GamepadInput::None)
        {
            return false;
        }
        switch (input)
        {
            case Engine::GamepadInput::ButtonA: return gamepad.buttonA;
            case Engine::GamepadInput::ButtonB: return gamepad.buttonB;
            case Engine::GamepadInput::ButtonX: return gamepad.buttonX;
            case Engine::GamepadInput::ButtonY: return gamepad.buttonY;
            case Engine::GamepadInput::LeftShoulder: return gamepad.leftShoulder;
            case Engine::GamepadInput::RightShoulder: return gamepad.rightShoulder;
            case Engine::GamepadInput::DpadUp: return gamepad.dpadUp;
            case Engine::GamepadInput::DpadDown: return gamepad.dpadDown;
            case Engine::GamepadInput::DpadLeft: return gamepad.dpadLeft;
            case Engine::GamepadInput::DpadRight: return gamepad.dpadRight;
            case Engine::GamepadInput::LeftStickUp: return gamepad.leftStickY > kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::LeftStickDown: return gamepad.leftStickY < -kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::LeftStickLeft: return gamepad.leftStickX < -kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::LeftStickRight: return gamepad.leftStickX > kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::RightStickUp: return gamepad.rightStickY > kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::RightStickDown: return gamepad.rightStickY < -kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::RightStickLeft: return gamepad.rightStickX < -kGamepadStickDigitalThreshold;
            case Engine::GamepadInput::RightStickRight: return gamepad.rightStickX > kGamepadStickDigitalThreshold;
            default: return false;
        }
    }

    float GamepadAxisValue(const Foundation::GamepadState& gamepad, Engine::GamepadAxis axis)
    {
        switch (axis)
        {
            case Engine::GamepadAxis::LeftStickX: return gamepad.leftStickX;
            case Engine::GamepadAxis::LeftStickY: return gamepad.leftStickY;
            case Engine::GamepadAxis::RightStickX: return gamepad.rightStickX;
            case Engine::GamepadAxis::RightStickY: return gamepad.rightStickY;
            default: return 0.0f;
        }
    }
}

namespace Engine
{
    InputCommand AssembleInputCommand(uint32_t tick, const InputBindings& bindings, const InputCommand& previous, int gamepadIndex)
    {
        const Foundation::GamepadState gamepad = Foundation::GetGamepadState(gamepadIndex);

        InputCommand command;
        command.tick = tick;

        for (size_t i = 0; i < kMaxInputButtons; ++i)
        {
            const InputBinding& binding = bindings.buttons[i];
            const bool held = (binding.primaryKey != 0 && Foundation::IsKeyDown(binding.primaryKey)) ||
                               GamepadInputHeld(gamepad, binding.gamepadInput) || GamepadInputHeld(gamepad, binding.gamepadInputAlt);

            ButtonState& state = command.buttons[i];
            state.held = held;
            state.pressedThisTick = held && !previous.buttons[i].held;
            state.releasedThisTick = !held && previous.buttons[i].held;
        }

        float axisValue = 0.0f;
        if (gamepad.connected && bindings.axis.gamepadAxis != GamepadAxis::None)
        {
            const float stickValue = GamepadAxisValue(gamepad, bindings.axis.gamepadAxis);
            if (fabsf(stickValue) > kGamepadStickThreshold)
            {
                axisValue = stickValue;
            }
        }
        if (axisValue == 0.0f)
        {
            const bool positiveHeld = (bindings.axis.positiveKey != 0 && Foundation::IsKeyDown(bindings.axis.positiveKey)) ||
                                       GamepadInputHeld(gamepad, bindings.axis.gamepadPositive);
            const bool negativeHeld = (bindings.axis.negativeKey != 0 && Foundation::IsKeyDown(bindings.axis.negativeKey)) ||
                                       GamepadInputHeld(gamepad, bindings.axis.gamepadNegative);
            axisValue = (positiveHeld ? 1.0f : 0.0f) - (negativeHeld ? 1.0f : 0.0f);
        }
        command.axis = axisValue;

        return command;
    }
}
