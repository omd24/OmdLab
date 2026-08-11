#pragma once

#include "CombatDsl.h"
#include "Engine/Collision.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Game
{
    enum class GuardHeight : uint8_t
    {
        Mid,
        High,
        Low,
        Overhead,
        Throw,
        Unblockable,
    };

    // Absolute (from move start) frame window a hitbox is active for - defaults to
    // [startupFrames, startupFrames + activeFrames) for the common single-hitbox move, but
    // authored per-hitbox so a future multi-hit move can override it.
    struct MoveHitboxDef
    {
        Engine::CollisionBox box;
        uint32_t frameStart = 0;
        uint32_t frameEnd = 0;
    };

    // One move's full data, built from a parsed CombatDsl::MoveDecl (see BuildMoveTable) -
    // everything a real fighting game move needs to be simulated: which clip plays, its three
    // sequential frame phases, its outcome on hit/block, and its cancel-into list (still holding
    // the parsed condition trees directly, evaluated the same way state transitions are).
    struct MoveDefinition
    {
        std::string id;
        std::string displayName;
        std::string animationClip; // References an Asset::Clip by name.
        std::string inputButton;   // Matches a Game::FighterButton's name (see InputBindings.h).
        uint32_t startupFrames = 0;
        uint32_t activeFrames = 0;
        uint32_t recoveryFrames = 0;
        uint32_t onHitStunFrames = 0;
        uint32_t onBlockStunFrames = 0;
        int32_t damage = 0;
        GuardHeight guardHeight = GuardHeight::Mid;
        std::vector<MoveHitboxDef> hitboxes;
        std::vector<CombatDsl::Cancel> cancels;
    };

    // One character's full moveset. Move-only (CombatDsl::Cancel holds a unique_ptr<Condition>).
    struct MoveTable
    {
        std::vector<MoveDefinition> moves;

        const MoveDefinition* FindById(const std::string& id) const;

        // Stable position-in-table index, usable directly as Engine::Hitbox::moveId (which is
        // deliberately an opaque uint32_t as far as Engine is concerned) - -1 if id isn't found.
        int32_t IndexOf(const std::string& id) const;
    };

    struct CharacterStats
    {
        int32_t maxHealth = 100;
        float walkSpeed = 2.0f;
        float runSpeed = 4.0f;
        float jumpSpeed = 3.0f; // Initial vertical launch speed for Jump - paired with FighterState.cpp's
                                 // kGravity, which a jump's total airtime (2*jumpSpeed/kGravity) is derived from.
    };

    // A fighter entity is instantiated FROM this, never hand-assembled per-character in code.
    struct CharacterDefinition
    {
        std::string modelDirectory;
        CharacterStats stats;
        MoveTable moveTable;
    };

    // Maps a parsed CombatFile's stringly-typed MoveDecls onto real MoveDefinition fields.
    // Unknown field names are logged and skipped, not a hard error - keeps authoring additive
    // (a typo'd or not-yet-consumed field doesn't block everything else from loading). Takes the
    // parsed file by rvalue reference since it consumes (moves out of) each MoveDecl's cancel
    // list rather than copying it - CombatDsl::Condition is move-only.
    bool BuildMoveTable(CombatDsl::CombatFile&& parsedFile, MoveTable& outTable);
}
