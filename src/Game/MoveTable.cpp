#include "MoveTable.h"

#include "Asset/Model.h"
#include "Foundation/Log.h"

#include <utility>

namespace
{
    using Foundation::Log::Severity;

    Game::GuardHeight ParseGuardHeight(const std::string& text)
    {
        if (text == "high") return Game::GuardHeight::High;
        if (text == "low") return Game::GuardHeight::Low;
        if (text == "overhead") return Game::GuardHeight::Overhead;
        if (text == "throw") return Game::GuardHeight::Throw;
        if (text == "unblockable") return Game::GuardHeight::Unblockable;
        return Game::GuardHeight::Mid;
    }

    // frameStart/frameEnd default to the move's own [startup, startup+active) window - the
    // common case for a single-hitbox move - overridable below for a future multi-hit move.
    Engine::CollisionBox BuildHitboxShape(
        const Game::CombatDsl::FieldBlock& block, uint32_t& outFrameStart, uint32_t& outFrameEnd, uint32_t moveStartupFrames,
        uint32_t moveActiveFrames, std::string& outBoneName)
    {
        Engine::CollisionBox box;
        outFrameStart = moveStartupFrames;
        outFrameEnd = moveStartupFrames + moveActiveFrames;
        for (const auto& [name, value] : block)
        {
            const float numberValue = static_cast<float>(value.numberValue);
            if (name == "offsetX") box.offset.x = numberValue;
            else if (name == "offsetY") box.offset.y = numberValue;
            else if (name == "offsetZ") box.offset.z = numberValue;
            else if (name == "halfX") box.halfExtents.x = numberValue;
            else if (name == "halfY") box.halfExtents.y = numberValue;
            else if (name == "halfZ") box.halfExtents.z = numberValue;
            else if (name == "frameStart") outFrameStart = static_cast<uint32_t>(value.numberValue);
            else if (name == "frameEnd") outFrameEnd = static_cast<uint32_t>(value.numberValue);
            else if (name == "bone") outBoneName = value.stringValue;
            else Foundation::Log::Write(Severity::Warning, "MoveTable", "unknown hitbox field '%s'", name.c_str());
        }
        return box;
    }
}

namespace Game
{
    const MoveDefinition* MoveTable::FindById(const std::string& id) const
    {
        for (const MoveDefinition& move : moves)
        {
            if (move.id == id)
            {
                return &move;
            }
        }
        return nullptr;
    }

    int32_t MoveTable::IndexOf(const std::string& id) const
    {
        for (size_t i = 0; i < moves.size(); ++i)
        {
            if (moves[i].id == id)
            {
                return static_cast<int32_t>(i);
            }
        }
        return -1;
    }

    bool BuildMoveTable(CombatDsl::CombatFile&& parsedFile, MoveTable& outTable)
    {
        outTable.moves.clear();
        for (CombatDsl::MoveDecl& moveDecl : parsedFile.moves)
        {
            if (moveDecl.id.empty())
            {
                Foundation::Log::Write(Severity::Error, "MoveTable", "a move declaration has no id");
                return false;
            }

            MoveDefinition move;
            move.id = moveDecl.id;
            move.displayName = moveDecl.id;

            for (const auto& [name, value] : moveDecl.fields)
            {
                if (name == "animationClip") move.animationClip = value.stringValue;
                else if (name == "inputButton") move.inputButton = value.stringValue;
                else if (name == "displayName") move.displayName = value.stringValue;
                else if (name == "startup") move.startupFrames = static_cast<uint32_t>(value.numberValue);
                else if (name == "active") move.activeFrames = static_cast<uint32_t>(value.numberValue);
                else if (name == "recovery") move.recoveryFrames = static_cast<uint32_t>(value.numberValue);
                else if (name == "onHitStun") move.onHitStunFrames = static_cast<uint32_t>(value.numberValue);
                else if (name == "onBlockStun") move.onBlockStunFrames = static_cast<uint32_t>(value.numberValue);
                else if (name == "damage") move.damage = static_cast<int32_t>(value.numberValue);
                else if (name == "guardHeight") move.guardHeight = ParseGuardHeight(value.stringValue);
                else Foundation::Log::Write(Severity::Warning, "MoveTable", "unknown field '%s' on move '%s'", name.c_str(), move.id.c_str());
            }

            for (const CombatDsl::FieldBlock& hitboxBlock : moveDecl.hitboxes)
            {
                MoveHitboxDef hitboxDef;
                hitboxDef.box = BuildHitboxShape(
                    hitboxBlock, hitboxDef.frameStart, hitboxDef.frameEnd, move.startupFrames, move.activeFrames, hitboxDef.boneName);
                move.hitboxes.push_back(hitboxDef);
            }

            move.cancels = std::move(moveDecl.cancels);
            outTable.moves.push_back(std::move(move));
        }
        return true;
    }

    void ResolveHitboxJoints(const Asset::Model& model, MoveTable& moveTable)
    {
        for (MoveDefinition& move : moveTable.moves)
        {
            for (MoveHitboxDef& hitboxDef : move.hitboxes)
            {
                if (hitboxDef.boneName.empty())
                {
                    continue;
                }
                hitboxDef.resolvedJointIndex = -1;
                for (size_t i = 0; i < model.nodes.size(); ++i)
                {
                    if (model.nodes[i].name == hitboxDef.boneName)
                    {
                        hitboxDef.resolvedJointIndex = static_cast<int32_t>(i);
                        break;
                    }
                }
                if (hitboxDef.resolvedJointIndex < 0)
                {
                    Foundation::Log::Write(
                        Severity::Error, "MoveTable", "move '%s' hitbox references unknown bone '%s' - falling back to root-relative",
                        move.id.c_str(), hitboxDef.boneName.c_str());
                }
            }
        }
    }
}
