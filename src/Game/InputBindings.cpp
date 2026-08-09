#include "InputBindings.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace Game
{
    Engine::InputBindings MakeDefaultFighterBindings()
    {
        Engine::InputBindings bindings;

        auto& jump = bindings.buttons[static_cast<size_t>(FighterButton::Jump)];
        jump.primaryKey = VK_SPACE;
        jump.gamepadInput = Engine::GamepadInput::ButtonA;

        auto& crouch = bindings.buttons[static_cast<size_t>(FighterButton::Crouch)];
        crouch.primaryKey = VK_LCONTROL;
        crouch.gamepadInput = Engine::GamepadInput::LeftStickDown;

        auto& punch = bindings.buttons[static_cast<size_t>(FighterButton::Punch)];
        punch.primaryKey = 'J';
        punch.gamepadInput = Engine::GamepadInput::ButtonX;

        auto& kick = bindings.buttons[static_cast<size_t>(FighterButton::Kick)];
        kick.primaryKey = 'K';
        kick.gamepadInput = Engine::GamepadInput::ButtonB;

        auto& block = bindings.buttons[static_cast<size_t>(FighterButton::Block)];
        block.primaryKey = VK_LSHIFT;
        block.gamepadInput = Engine::GamepadInput::RightShoulder;

        auto& run = bindings.buttons[static_cast<size_t>(FighterButton::Run)];
        run.primaryKey = 'L';
        run.gamepadInput = Engine::GamepadInput::LeftShoulder;

        bindings.axis.negativeKey = 'A';
        bindings.axis.positiveKey = 'D';
        bindings.axis.gamepadAxis = Engine::GamepadAxis::LeftStickX;

        return bindings;
    }

    std::optional<FighterButton> ParseFighterButtonName(const std::string& name)
    {
        if (name == "Jump") return FighterButton::Jump;
        if (name == "Crouch") return FighterButton::Crouch;
        if (name == "Punch") return FighterButton::Punch;
        if (name == "Kick") return FighterButton::Kick;
        if (name == "Block") return FighterButton::Block;
        if (name == "Run") return FighterButton::Run;
        return std::nullopt;
    }
}
