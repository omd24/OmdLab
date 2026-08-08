#pragma once

#include <cstddef>
#include <cstdint>

namespace Engine
{
    // held: true this tick, regardless of last tick. pressedThisTick/releasedThisTick: the
    // false->true / true->false edges specifically - computed once here at assembly time (see
    // AssembleInputCommand) rather than re-derived by every future combo/motion-input matcher
    // that reads history, since every such matcher would otherwise need to know how to diff
    // adjacent history slots itself, including the ring buffer's wraparound case.
    struct ButtonState
    {
        bool held = false;
        bool pressedThisTick = false;
        bool releasedThisTick = false;
    };

    // Capacity, not any specific game's button count - Engine attaches no meaning to a slot
    // index, only that this many exist. Headroom cost is cheap, matches the same reasoning
    // already used for Renderer::kMaxSkinJoints.
    constexpr size_t kMaxInputButtons = 8;

    // One tick's worth of input - anonymous slots only, no button names or game concepts here
    // (a genuinely reusable input mechanism doesn't know what "Punch" is). Does NOT serve the
    // free-fly debug camera (see Camera.h/.cpp, which reads Foundation::Input directly) - the
    // camera's continuous 3D translate/look/mouse-drag axes don't belong on a type meant to be
    // ring-buffered and, eventually, networked; forcing them in would bloat this struct with
    // fields no fighter will ever read without even covering the camera's own needs (mouse-drag
    // pan/zoom still wouldn't fit). Naming slot indices and interpreting the axis is entirely
    // the caller's job (see Game/InputBindings.h).
    struct InputCommand
    {
        uint32_t tick = 0;
        ButtonState buttons[kMaxInputButtons];
        float axis = 0.0f; // [-1, 1] - one generic analog/digital axis; the caller decides what it drives.
    };

    // Physical gamepad elements a button slot can bind to - describes the controller's own
    // layout (face buttons, shoulders, dpad, thresholded stick directions), not any game
    // concept. Stick directions are exposed as digital "held past a threshold" the same way a
    // keyboard key is, so a button slot can be bound to either uniformly.
    enum class GamepadInput
    {
        None,
        ButtonA,
        ButtonB,
        ButtonX,
        ButtonY,
        LeftShoulder,
        RightShoulder,
        DpadUp,
        DpadDown,
        DpadLeft,
        DpadRight,
        LeftStickUp,
        LeftStickDown,
        LeftStickLeft,
        LeftStickRight,
        RightStickUp,
        RightStickDown,
        RightStickLeft,
        RightStickRight,
    };

    // Which continuous gamepad stick axis (if any) drives InputCommand::axis.
    enum class GamepadAxis
    {
        None,
        LeftStickX,
        LeftStickY,
        RightStickX,
        RightStickY,
    };

    // One button slot's binding: a keyboard key (a Win32 virtual-key constant or ASCII letter/
    // digit code, Foundation::IsKeyDown's own convention; 0 = unbound) and/or a physical
    // gamepad element - a slot is held if either source reports it, the same merge behavior
    // this project has used since the input system's first version, just expressed as data now
    // instead of hardcoded per-button code.
    struct InputBinding
    {
        int primaryKey = 0;
        GamepadInput gamepadInput = GamepadInput::None;
    };

    struct AxisBinding
    {
        int negativeKey = 0; // Digital fallback, drives -1.
        int positiveKey = 0; // Digital fallback, drives +1.
        // Preferred over the digital fallback once the stick clears its own deadzone.
        GamepadAxis gamepadAxis = GamepadAxis::None;
    };

    // The full binding table for one local input source. Entirely caller-supplied - Engine
    // attaches no meaning to a slot index or to the axis, only knows how to read whatever
    // physical input each one names. A caller (e.g. Game/InputBindings.h) owns an instance of
    // this (built from a data file eventually, a hardcoded default for now) plus its own enum
    // naming each slot index for its own readability - Engine never sees that enum.
    struct InputBindings
    {
        InputBinding buttons[kMaxInputButtons];
        AxisBinding axis;
    };

    // Assembles one tick's InputCommand from current raw device state, mapped through
    // `bindings`. Pure function of (tick, bindings, previous command, current device state) -
    // no hidden internal state - so it's reproducible from explicit inputs alone, matching the
    // networking-readiness goal of keeping sim-adjacent code deterministic/replay-friendly.
    //
    // gamepadIndex: XInput slot 0-3, per Foundation::GetGamepadState.
    InputCommand AssembleInputCommand(uint32_t tick, const InputBindings& bindings, const InputCommand& previous, int gamepadIndex);

    // Fixed-size ring buffer of recent InputCommands - the raw material any future combo/
    // motion-input matcher scans back over. 64 ticks (~1.07s at FixedTimestepAccumulator's
    // 60Hz tick rate) comfortably covers both a single motion input (typically judged over a
    // ~15-20 frame window) and a multi-hit combo string with slack -
    // deliberately generous now rather than tightly fit to an unbuilt motion-input spec, since
    // that's cheap to do now and expensive to discover-too-small later. A power of two so
    // wraparound is a mask, not a modulo.
    constexpr uint32_t kInputHistoryTicks = 64;

    struct InputHistory
    {
        void Push(const InputCommand& command)
        {
            entries[nextIndex] = command;
            nextIndex = (nextIndex + 1) % kInputHistoryTicks;
            if (storedCount < kInputHistoryTicks)
            {
                ++storedCount;
            }
        }

        // ticksAgo: 0 = most recently pushed command. ticksAgo >= the number of commands
        // pushed so far returns the oldest command still stored (clamped, not undefined) -
        // callers scanning back further than history actually goes get stale-but-valid data,
        // never garbage.
        const InputCommand& Get(uint32_t ticksAgo) const
        {
            const uint32_t clamped = ticksAgo < storedCount ? ticksAgo : (storedCount > 0 ? storedCount - 1 : 0);
            const uint32_t index = (nextIndex + kInputHistoryTicks - 1 - clamped) % kInputHistoryTicks;
            return entries[index];
        }

        const InputCommand& Latest() const
        {
            return Get(0);
        }

    private:
        InputCommand entries[kInputHistoryTicks]{};
        uint32_t nextIndex = 0;
        uint32_t storedCount = 0;
    };
}
