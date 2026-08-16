#include "InputBindings.h"

#include "Foundation/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
    // A single alnum character maps directly to its own VK_* code - matches Win32's own
    // convention that VK_A..VK_Z/VK_0..VK_9 equal the ASCII letter/digit. Named keys beyond that
    // are looked up explicitly; extend this list as new named keys are actually bound, rather
    // than trying to cover the full VK_* space up front.
    int ParseVirtualKey(const std::string& token)
    {
        if (token.size() == 1 && std::isalnum(static_cast<unsigned char>(token[0])))
        {
            return std::toupper(static_cast<unsigned char>(token[0]));
        }
        if (token == "Space") return VK_SPACE;
        if (token == "LControl") return VK_LCONTROL;
        if (token == "LShift") return VK_LSHIFT;
        return 0; // "-" or unrecognized - no key bound, not a parse error (a button may
                  // legitimately have no keyboard binding, only a gamepad one).
    }

    Engine::GamepadInput ParseGamepadInput(const std::string& token)
    {
        if (token == "ButtonA") return Engine::GamepadInput::ButtonA;
        if (token == "ButtonB") return Engine::GamepadInput::ButtonB;
        if (token == "ButtonX") return Engine::GamepadInput::ButtonX;
        if (token == "ButtonY") return Engine::GamepadInput::ButtonY;
        if (token == "LeftShoulder") return Engine::GamepadInput::LeftShoulder;
        if (token == "RightShoulder") return Engine::GamepadInput::RightShoulder;
        if (token == "DpadUp") return Engine::GamepadInput::DpadUp;
        if (token == "DpadDown") return Engine::GamepadInput::DpadDown;
        if (token == "DpadLeft") return Engine::GamepadInput::DpadLeft;
        if (token == "DpadRight") return Engine::GamepadInput::DpadRight;
        if (token == "LeftStickUp") return Engine::GamepadInput::LeftStickUp;
        if (token == "LeftStickDown") return Engine::GamepadInput::LeftStickDown;
        if (token == "LeftStickLeft") return Engine::GamepadInput::LeftStickLeft;
        if (token == "LeftStickRight") return Engine::GamepadInput::LeftStickRight;
        if (token == "RightStickUp") return Engine::GamepadInput::RightStickUp;
        if (token == "RightStickDown") return Engine::GamepadInput::RightStickDown;
        if (token == "RightStickLeft") return Engine::GamepadInput::RightStickLeft;
        if (token == "RightStickRight") return Engine::GamepadInput::RightStickRight;
        return Engine::GamepadInput::None; // "-" or unrecognized.
    }

    Engine::GamepadAxis ParseGamepadAxis(const std::string& token)
    {
        if (token == "LeftStickX") return Engine::GamepadAxis::LeftStickX;
        if (token == "LeftStickY") return Engine::GamepadAxis::LeftStickY;
        if (token == "RightStickX") return Engine::GamepadAxis::RightStickX;
        if (token == "RightStickY") return Engine::GamepadAxis::RightStickY;
        return Engine::GamepadAxis::None;
    }

    std::vector<std::string> Tokenize(const std::string& line)
    {
        std::vector<std::string> tokens;
        std::istringstream stream(line);
        std::string token;
        while (stream >> token)
        {
            tokens.push_back(token);
        }
        return tokens;
    }
}

namespace Game
{
    Engine::InputBindings MakeDefaultFighterBindings()
    {
        Engine::InputBindings bindings;

        auto& jump = bindings.buttons[static_cast<size_t>(FighterButton::Jump)];
        jump.primaryKey = VK_SPACE;
        jump.gamepadInput = Engine::GamepadInput::DpadUp; // D-pad is the real fighting-game convention for movement.
        jump.gamepadInputAlt = Engine::GamepadInput::LeftStickUp; // Stick also works, not required.

        auto& crouch = bindings.buttons[static_cast<size_t>(FighterButton::Crouch)];
        crouch.primaryKey = VK_LCONTROL;
        crouch.gamepadInput = Engine::GamepadInput::DpadDown;
        crouch.gamepadInputAlt = Engine::GamepadInput::LeftStickDown;

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
        bindings.axis.gamepadNegative = Engine::GamepadInput::DpadLeft;
        bindings.axis.gamepadPositive = Engine::GamepadInput::DpadRight;
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

    bool LoadFighterBindingsFromFile(const std::string& path, Engine::InputBindings& outBindings)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            Foundation::Log::Write(Foundation::Log::Severity::Warning, "Game", "Could not open input bindings file '%s'", path.c_str());
            return false;
        }

        // Same-order names as the FighterButton enum, for readable log messages only - not used
        // for parsing (ParseFighterButtonName already owns that mapping).
        static constexpr std::array<const char*, static_cast<size_t>(FighterButton::Count)> kButtonNames = {
            "Jump", "Crouch", "Punch", "Kick", "Block", "Run" };

        // Built up in a local copy, only committed to outBindings once every expected row has
        // been found well-formed - see this function's own header comment for why.
        Engine::InputBindings parsed;
        std::array<bool, static_cast<size_t>(FighterButton::Count)> buttonFound{};
        bool axisFound = false;

        std::string line;
        int lineNumber = 0;
        while (std::getline(file, line))
        {
            ++lineNumber;
            const size_t commentPos = line.find('#');
            const std::vector<std::string> tokens = Tokenize(commentPos == std::string::npos ? line : line.substr(0, commentPos));
            if (tokens.empty())
            {
                continue;
            }

            if (tokens[0] == "Axis")
            {
                if (tokens.size() != 6)
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Error, "Game", "%s:%d: Axis row expects 5 fields, got %zu", path.c_str(), lineNumber,
                        tokens.size() - 1);
                    return false;
                }
                parsed.axis.negativeKey = ParseVirtualKey(tokens[1]);
                parsed.axis.positiveKey = ParseVirtualKey(tokens[2]);
                parsed.axis.gamepadNegative = ParseGamepadInput(tokens[3]);
                parsed.axis.gamepadPositive = ParseGamepadInput(tokens[4]);
                parsed.axis.gamepadAxis = ParseGamepadAxis(tokens[5]);
                axisFound = true;
                continue;
            }

            const std::optional<FighterButton> button = ParseFighterButtonName(tokens[0]);
            if (!button.has_value())
            {
                Foundation::Log::Write(
                    Foundation::Log::Severity::Error, "Game", "%s:%d: unrecognized binding row '%s'", path.c_str(), lineNumber,
                    tokens[0].c_str());
                return false;
            }
            if (tokens.size() != 4)
            {
                Foundation::Log::Write(
                    Foundation::Log::Severity::Error, "Game", "%s:%d: '%s' row expects 3 fields, got %zu", path.c_str(), lineNumber,
                    tokens[0].c_str(), tokens.size() - 1);
                return false;
            }
            Engine::InputBinding& binding = parsed.buttons[static_cast<size_t>(*button)];
            binding.primaryKey = ParseVirtualKey(tokens[1]);
            binding.gamepadInput = ParseGamepadInput(tokens[2]);
            binding.gamepadInputAlt = ParseGamepadInput(tokens[3]);
            buttonFound[static_cast<size_t>(*button)] = true;
        }

        for (size_t i = 0; i < buttonFound.size(); ++i)
        {
            if (!buttonFound[i])
            {
                Foundation::Log::Write(
                    Foundation::Log::Severity::Error, "Game", "%s: missing a binding row for '%s'", path.c_str(), kButtonNames[i]);
                return false;
            }
        }
        if (!axisFound)
        {
            Foundation::Log::Write(Foundation::Log::Severity::Error, "Game", "%s: missing the Axis row", path.c_str());
            return false;
        }

        outBindings = parsed;
        return true;
    }
}
