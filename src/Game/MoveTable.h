#pragma once

#include "CombatDsl.h"
#include "Engine/Collision.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Asset
{
    struct Model;
}

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
        // Optional bone attachment (authored as a "bone <name>" field inside hitbox { } - see
        // MoveTable.cpp's BuildHitboxShape). Empty means today's behavior unchanged: box.offset
        // is relative to the entity's own root Transform. Non-empty names an Asset::Node by its
        // real name (e.g. "RightHand_014") - resolved once at startup (ResolveHitboxJoints)
        // into resolvedJointIndex rather than searched by string every tick; FighterState.cpp
        // then reads that joint's own current animated position each tick box.offset becomes a
        // small refinement relative to, instead of the sole source of the box's position.
        std::string boneName;
        int32_t resolvedJointIndex = -1;
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
        // Corrective transform this character's specific source asset's MESH VERTEX data needs
        // applied to land in true game-world units/position - see Engine::
        // CreateSkinnedMeshDrawItems's own rootTransform parameter for the scale quirk this
        // exists to correct (a Sketchfab FBX->glTF export baking a 0.01 unit-conversion scale
        // this asset's skin data doesn't otherwise account for). Lives here (not just a
        // main.cpp-local, which is where it used to live) as a single source of truth for
        // whichever future code needs it a second place, even though FighterState.cpp's
        // bone-attached hitbox lookup turned out NOT to need it - verified empirically that this
        // asset's joint/skeleton hierarchy is already true-scale on its own, unlike its mesh
        // vertex data (see ResolveHitboxOffset's own comment for the full story). Identity by
        // default - only a real character asset with a known quirk needs to set this to anything
        // else. Stored as XMFLOAT4X4, not XMMATRIX, matching this project's convention of never
        // storing the SIMD-register type as a class member (XMLoadFloat4x4/XMStoreFloat4x4 at
        // the point of use, same as every other persisted matrix in this codebase).
        DirectX::XMFLOAT4X4 assetCorrection = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
        // A second, DELIBERATELY SEPARATE corrective rotation (radians, around world +Y) - not
        // folded into assetCorrection above, because it means something different: assetCorrection
        // fixes an accidental *import artifact* affecting only mesh vertex data (empirically not
        // the skeleton); this fixes which way the character actually faces, a real property of
        // the whole rigid character (mesh AND skeleton alike, since they must stay visually
        // consistent with each other) that this project cares about for gameplay reasons - the
        // ground plane/movement/camera all already assume a 2D side view (camera looking down
        // +Z, X is the screen-horizontal gameplay axis), but this character's own source asset
        // was authored facing along Z, showing its front/back to that camera instead of a
        // profile. Applied everywhere assetCorrection's scale is NOT (both mesh render AND
        // bone-attached hitbox lookups - see ResolveHitboxOffset), since unlike the scale quirk
        // this must move the skeleton too, or a hitbox would visibly detach from the mesh it's
        // meant to track. 0 (no correction) by default.
        float facingCorrectionRadians = 0.0f;
    };

    // Maps a parsed CombatFile's stringly-typed MoveDecls onto real MoveDefinition fields.
    // Unknown field names are logged and skipped, not a hard error - keeps authoring additive
    // (a typo'd or not-yet-consumed field doesn't block everything else from loading). Takes the
    // parsed file by rvalue reference since it consumes (moves out of) each MoveDecl's cancel
    // list rather than copying it - CombatDsl::Condition is move-only.
    bool BuildMoveTable(CombatDsl::CombatFile&& parsedFile, MoveTable& outTable);

    // One-time resolution of every hitbox's optional bone attachment (MoveHitboxDef::boneName)
    // from a name to a stable Asset::Model node index - called once at startup after both the
    // character's model and move table are loaded, not searched by string every tick. Logs (but
    // doesn't fail loudly on) any boneName that doesn't match a real node name, leaving
    // resolvedJointIndex at -1 - FighterState.cpp treats that identically to "no bone was ever
    // authored," so a typo degrades to the old root-relative behavior rather than crashing.
    void ResolveHitboxJoints(const Asset::Model& model, MoveTable& moveTable);
}
