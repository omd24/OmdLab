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

    // Distinct from the free-fly debug camera's own WASD/arrows/QE. Built-in fallback, used
    // whenever data/input_bindings.txt (see LoadFighterBindingsFromFile) is missing or fails to
    // parse - always valid, never itself loaded from disk.
    Engine::InputBindings MakeDefaultFighterBindings();

    // Parses the flat "one row per button, plus one Axis row" table data/input_bindings.txt is
    // authored in (see that file's own header comment for exact column meaning). All-or-nothing,
    // matching this project's established parse convention for authored data: outBindings is
    // only ever touched once every expected row has been found well-formed - a partial/malformed
    // file leaves outBindings completely untouched and returns false, rather than applying a
    // half-updated result. Callers are expected to already hold a valid fallback (typically
    // MakeDefaultFighterBindings()) to keep using in that case.
    bool LoadFighterBindingsFromFile(const std::string& path, Engine::InputBindings& outBindings);

    // Maps a move's authored inputButton field (a bare identifier in .combat content, e.g.
    // "Punch") onto this enum - std::nullopt for an unrecognized name, so a typo'd field fails
    // to match any input rather than crashing or aliasing to button 0.
    std::optional<FighterButton> ParseFighterButtonName(const std::string& name);
}
