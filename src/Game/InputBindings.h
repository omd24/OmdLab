#pragma once

#include "Engine/Input.h"

#include <optional>
#include <string>

namespace Game
{
    // The buttons a fighter reads. A stand-in gameplay set, not yet validated against any real
    // moveset - names/count are expected to change once real move data exists. Lives here, not
    // in Engine, since button identity is game-specific; Engine only knows about anonymous
    // slots (see Engine::InputCommand).
    enum class FighterButton
    {
        Jump,
        Crouch,
        Punch,
        Kick,
        Block,
        Run,
        Count
    };

    // Placeholder dev keybinds, distinct from the free-fly camera's WASD/arrows/QE, until real
    // gameplay exists to justify a real binding scheme. Hardcoded here for now; swapping this
    // for a data file later (per the "input bindings should be tabular" design) changes only
    // this function's implementation, not any caller.
    Engine::InputBindings MakeDefaultFighterBindings();

    // Maps a move's authored inputButton field (a bare identifier in .combat content, e.g.
    // "Punch") onto this enum - std::nullopt for an unrecognized name, so a typo'd field fails
    // to match any input rather than crashing or aliasing to button 0.
    std::optional<FighterButton> ParseFighterButtonName(const std::string& name);
}
