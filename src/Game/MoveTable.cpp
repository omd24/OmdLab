#include "MoveTable.h"

#include "GameConstants.h"
#include "Asset/Model.h"
#include "Foundation/Log.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <utility>

namespace
{
    using Foundation::Log::Severity;

    std::string TrimCombatLine(const std::string& text)
    {
        const size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
        {
            return "";
        }
        const size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(start, end - start + 1);
    }

    std::string StripCombatComment(const std::string& line)
    {
        const size_t hashPos = line.find('#');
        return hashPos == std::string::npos ? line : line.substr(0, hashPos);
    }

    // Formats a float the way this project's hand-authored .combat files already do - a bare
    // integer when the value has no fractional part ("0", "8"), otherwise up to 4 decimal places
    // with trailing zeros (and a bare trailing '.') trimmed ("0.15").
    std::string FormatCombatNumber(float value)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.4f", value);
        std::string text = buffer;
        const size_t dot = text.find('.');
        if (dot != std::string::npos)
        {
            size_t lastSignificant = text.find_last_not_of('0');
            if (lastSignificant == dot)
            {
                --lastSignificant; // Nothing but zeros after the dot - drop the dot too.
            }
            text.erase(lastSignificant + 1);
        }
        return text == "-0" ? "0" : text;
    }

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

    // Everything up to (not including) the last '/' or '\' - "" if the path has no separator.
    // Used to resolve character.combat's "moves" reference and its own modelDirectory relative
    // to where the character file itself lives, so the whole character folder is relocatable.
    std::string DirectoryOf(const std::string& path)
    {
        const size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
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

            // Scales every authored frame-count field (not the spatial hitbox fields below) by
            // 1/kCombatSpeedMultiplier, before BuildHitboxShape derives each hitbox's own
            // active-frame window from startup/active - see kCombatSpeedMultiplier's own comment
            // for why this has to move in lockstep with SetClip's faster clip playback rather
            // than being retuned independently.
            {
                const float speedScale = 1.0f / kCombatSpeedMultiplier;
                move.startupFrames = static_cast<uint32_t>(std::lround(static_cast<float>(move.startupFrames) * speedScale));
                move.activeFrames = static_cast<uint32_t>(std::lround(static_cast<float>(move.activeFrames) * speedScale));
                move.recoveryFrames = static_cast<uint32_t>(std::lround(static_cast<float>(move.recoveryFrames) * speedScale));
                move.onHitStunFrames = static_cast<uint32_t>(std::lround(static_cast<float>(move.onHitStunFrames) * speedScale));
                move.onBlockStunFrames = static_cast<uint32_t>(std::lround(static_cast<float>(move.onBlockStunFrames) * speedScale));
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

    void BuildCharacterDefinition(const CombatDsl::CharacterDecl& decl, CharacterDefinition& outDefinition)
    {
        for (const auto& [name, value] : decl.fields)
        {
            if (name == "name") outDefinition.displayName = value.stringValue;
            else if (name == "model") outDefinition.modelFile = value.stringValue;
            else if (name == "moves") {} // Consumed by LoadCharacterDefinition - recognized, not an unknown field.
            else Foundation::Log::Write(Severity::Warning, "MoveTable", "unknown character field '%s'", name.c_str());
        }

        for (const auto& [name, value] : decl.stats)
        {
            if (name == "maxHealth") outDefinition.stats.maxHealth = static_cast<int32_t>(value.numberValue);
            else if (name == "walkSpeed") outDefinition.stats.walkSpeed = static_cast<float>(value.numberValue);
            else if (name == "runSpeed") outDefinition.stats.runSpeed = static_cast<float>(value.numberValue);
            else if (name == "jumpSpeed") outDefinition.stats.jumpSpeed = static_cast<float>(value.numberValue);
            else Foundation::Log::Write(Severity::Warning, "MoveTable", "unknown character stats field '%s'", name.c_str());
        }

        for (const auto& [name, value] : decl.correction)
        {
            if (name == "assetScale")
            {
                const float scale = static_cast<float>(value.numberValue);
                DirectX::XMStoreFloat4x4(&outDefinition.assetCorrection, DirectX::XMMatrixScaling(scale, scale, scale));
            }
            else if (name == "facingDegrees")
            {
                outDefinition.facingCorrectionRadians = static_cast<float>(value.numberValue) * (DirectX::XM_PI / 180.0f);
            }
            else if (name == "groundingOffsetY")
            {
                outDefinition.groundingOffsetY = static_cast<float>(value.numberValue);
            }
            else
            {
                Foundation::Log::Write(Severity::Warning, "MoveTable", "unknown character correction field '%s'", name.c_str());
            }
        }

        // Same reciprocal frame scaling BuildMoveTable applies to a move's own frame fields, and
        // for the same reason (see kCombatSpeedMultiplier's comment): the "ko" clip plays back
        // at kCombatSpeedMultiplier rate, so a window authored against the raw clip's frame
        // numbers has to be divided by the multiplier to line up with framesInState at runtime.
        const float speedScale = 1.0f / kCombatSpeedMultiplier;
        for (const auto& [name, value] : decl.ko)
        {
            if (name == "dropOffsetY")
            {
                outDefinition.koDropOffsetY = static_cast<float>(value.numberValue);
            }
            else if (name == "dropStartFrame")
            {
                outDefinition.koDropStartFrame = static_cast<uint32_t>(std::lround(static_cast<float>(value.numberValue) * speedScale));
            }
            else if (name == "dropEndFrame")
            {
                outDefinition.koDropEndFrame = static_cast<uint32_t>(std::lround(static_cast<float>(value.numberValue) * speedScale));
            }
            else
            {
                Foundation::Log::Write(Severity::Warning, "MoveTable", "unknown character ko field '%s'", name.c_str());
            }
        }
    }

    bool LoadCharacterDefinition(const std::string& characterCombatPath, CharacterDefinition& outDefinition)
    {
        CombatDsl::CombatFile file;
        if (!CombatDsl::LoadCombatFile(characterCombatPath, file))
        {
            return false; // LoadCombatFile already logged the reason.
        }
        if (!file.character.has_value())
        {
            Foundation::Log::Write(
                Severity::Error, "MoveTable", "LoadCharacterDefinition: '%s' has no 'character' block", characterCombatPath.c_str());
            return false;
        }

        BuildCharacterDefinition(*file.character, outDefinition);

        const std::string directory = DirectoryOf(characterCombatPath);
        outDefinition.modelDirectory = directory;

        std::string movesReference;
        for (const auto& [name, value] : file.character->fields)
        {
            if (name == "moves")
            {
                movesReference = value.stringValue;
            }
        }
        if (movesReference.empty())
        {
            Foundation::Log::Write(
                Severity::Error, "MoveTable", "LoadCharacterDefinition: '%s' character block has no 'moves' field",
                characterCombatPath.c_str());
            return false;
        }

        const std::string movesPath = directory.empty() ? movesReference : directory + "/" + movesReference;
        CombatDsl::CombatFile movesFile;
        if (!CombatDsl::LoadCombatFile(movesPath, movesFile))
        {
            return false; // Already logged.
        }
        if (!BuildMoveTable(std::move(movesFile), outDefinition.moveTable))
        {
            Foundation::Log::Write(
                Severity::Error, "MoveTable", "LoadCharacterDefinition: failed to build move table from '%s'", movesPath.c_str());
            return false;
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

    bool SaveHitboxToFile(
        const std::string& filePath, const std::string& moveId, int32_t hitboxIndex, const Engine::CollisionBox& box,
        uint32_t frameStart, uint32_t frameEnd)
    {
        // Binary mode on both ends, deliberately - this project's .combat files are LF-only
        // (confirmed on moves.combat itself), but the default text-mode ofstream on Windows
        // translates every '\n' this function writes into '\r\n', silently rewriting every
        // line's ending (not just the six target lines) to CRLF. Reading in binary mode instead
        // (and trimming a stray '\r' below, for a CRLF-authored file this hasn't hit yet but
        // could) plus writing in binary mode keeps the file's own existing line-ending
        // convention untouched either way.
        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open())
        {
            Foundation::Log::Write(Severity::Error, "MoveTable", "SaveHitboxToFile: failed to open '%s' for reading", filePath.c_str());
            return false;
        }
        std::vector<std::string> lines;
        {
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back(line);
            }
        }
        in.close();

        int depth = 0;
        bool inTargetMove = false;
        bool enteredMoveBody = false;
        int moveHeaderDepth = 0;
        int32_t hitboxCounter = -1;
        bool inTargetHitbox = false;
        bool enteredHitboxBody = false;
        int hitboxHeaderDepth = 0;
        // Line index of the target hitbox's own closing '}' - captured so a missing
        // frameStart/frameEnd line can be inserted right before it (see the loop's own end).
        size_t hitboxCloseLine = 0;
        // Indentation to use for any newly INSERTED frameStart/frameEnd line - copied from
        // whichever existing field line is matched first below, so an inserted line looks
        // exactly like its siblings rather than guessing a hardcoded indent width.
        std::string capturedIndent;
        const std::string moveHeaderText = "move " + moveId;

        struct FieldTarget
        {
            const char* name;
            float value;
            bool written;
            // true: missing this line is a hard failure (existing offset/half-extent
            // behavior). false: missing is expected/common (frameStart/frameEnd usually
            // aren't authored at all, defaulting to [startup, startup+active) instead - see
            // MoveHitboxDef's own comment) - a new line gets INSERTED for it instead of
            // failing.
            bool required;
        };
        FieldTarget targets[8] = {
            { "offsetX", box.offset.x, false, true }, { "offsetY", box.offset.y, false, true },
            { "offsetZ", box.offset.z, false, true }, { "halfX", box.halfExtents.x, false, true },
            { "halfY", box.halfExtents.y, false, true }, { "halfZ", box.halfExtents.z, false, true },
            { "frameStart", static_cast<float>(frameStart), false, false }, { "frameEnd", static_cast<float>(frameEnd), false, false },
        };

        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string stripped = StripCombatComment(lines[i]);
            const std::string trimmed = TrimCombatLine(stripped);
            const int depthBefore = depth;

            if (!inTargetMove && depthBefore == 0 && trimmed == moveHeaderText)
            {
                inTargetMove = true;
                moveHeaderDepth = depthBefore;
            }
            else if (inTargetMove && !inTargetHitbox && depthBefore == moveHeaderDepth + 1 && trimmed == "hitbox")
            {
                ++hitboxCounter;
                if (hitboxCounter == hitboxIndex)
                {
                    inTargetHitbox = true;
                    hitboxHeaderDepth = depthBefore;
                }
            }
            else if (inTargetHitbox && depthBefore == hitboxHeaderDepth + 1)
            {
                const size_t firstNonSpace = lines[i].find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos)
                {
                    const size_t tokenEnd = trimmed.find_first_of(" \t");
                    const std::string fieldName = tokenEnd == std::string::npos ? trimmed : trimmed.substr(0, tokenEnd);
                    for (FieldTarget& target : targets)
                    {
                        if (!target.written && fieldName == target.name)
                        {
                            const std::string indent = lines[i].substr(0, firstNonSpace);
                            lines[i] = indent + target.name + " " + FormatCombatNumber(target.value);
                            target.written = true;
                            if (capturedIndent.empty())
                            {
                                capturedIndent = indent;
                            }
                            break;
                        }
                    }
                }
            }

            for (const char c : stripped)
            {
                if (c == '{') ++depth;
                else if (c == '}') --depth;
            }

            if (inTargetMove)
            {
                if (depth > moveHeaderDepth)
                {
                    enteredMoveBody = true;
                }
                else if (enteredMoveBody && depth == moveHeaderDepth)
                {
                    inTargetMove = false;
                }
            }
            if (inTargetHitbox)
            {
                if (depth > hitboxHeaderDepth)
                {
                    enteredHitboxBody = true;
                }
                else if (enteredHitboxBody && depth == hitboxHeaderDepth)
                {
                    hitboxCloseLine = i;
                    break; // Target hitbox's own body is closed - nothing left to find.
                }
            }
        }

        if (hitboxCounter < hitboxIndex)
        {
            Foundation::Log::Write(
                Severity::Error, "MoveTable", "SaveHitboxToFile: move '%s' has no hitbox #%d (found %d) in '%s'", moveId.c_str(),
                hitboxIndex, hitboxCounter + 1, filePath.c_str());
            return false;
        }
        bool allRequiredWritten = true;
        for (const FieldTarget& target : targets)
        {
            if (target.required && !target.written)
            {
                Foundation::Log::Write(
                    Severity::Error, "MoveTable", "SaveHitboxToFile: move '%s' hitbox #%d has no '%s' field line to rewrite in '%s'",
                    moveId.c_str(), hitboxIndex, target.name, filePath.c_str());
                allRequiredWritten = false;
            }
        }
        if (!allRequiredWritten)
        {
            return false;
        }

        // Any non-required target still missing (frameStart/frameEnd, typically - see
        // FieldTarget::required's own comment) gets a brand-new line inserted right before the
        // hitbox block's own closing '}', rather than failing - this is the expected common
        // case for a hitbox that's never had its active-frame window explicitly authored
        // before.
        std::vector<std::string> linesToInsert;
        for (const FieldTarget& target : targets)
        {
            if (!target.required && !target.written)
            {
                linesToInsert.push_back(capturedIndent + target.name + " " + FormatCombatNumber(target.value));
            }
        }
        if (!linesToInsert.empty())
        {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(hitboxCloseLine), linesToInsert.begin(), linesToInsert.end());
        }

        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Foundation::Log::Write(Severity::Error, "MoveTable", "SaveHitboxToFile: failed to open '%s' for writing", filePath.c_str());
            return false;
        }
        // Every line gets its own trailing '\n', including the last - matches moves.combat's own
        // existing trailing-newline-at-EOF convention rather than dropping it.
        for (const std::string& outLine : lines)
        {
            out << outLine << "\n";
        }
        out.close();

        Foundation::Log::Write(
            Severity::Info, "MoveTable", "SaveHitboxToFile: saved hitbox #%d of move '%s' to '%s'", hitboxIndex, moveId.c_str(),
            filePath.c_str());
        return true;
    }

    bool SaveCharacterToFile(
        const std::string& filePath, float groundingOffsetY, float koDropOffsetY, uint32_t koDropStartFrameRaw,
        uint32_t koDropEndFrameRaw)
    {
        // Same targeted line-rewrite approach as SaveHitboxToFile (see its own comment on binary
        // mode / CRLF), but simpler: character.combat always authors all four of these field
        // lines, so there is no insert path - a missing target line is a hard failure. Frame
        // values are written back as RAW clip frames (the caller undoes the 1/kCombatSpeedMultiplier
        // load-time scaling before calling this), so the file round-trips to the same numbers a
        // person authored.
        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open())
        {
            Foundation::Log::Write(Severity::Error, "MoveTable", "SaveCharacterToFile: failed to open '%s' for reading", filePath.c_str());
            return false;
        }
        std::vector<std::string> lines;
        {
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                lines.push_back(line);
            }
        }
        in.close();

        int depth = 0;
        bool foundCharacter = false;
        bool inCharacterBlock = false;
        bool enteredCharacterBody = false;
        int characterHeaderDepth = 0;

        struct FieldTarget
        {
            const char* name;
            float value;
            bool written;
        };
        FieldTarget targets[4] = {
            { "groundingOffsetY", groundingOffsetY, false },
            { "dropOffsetY", koDropOffsetY, false },
            { "dropStartFrame", static_cast<float>(koDropStartFrameRaw), false },
            { "dropEndFrame", static_cast<float>(koDropEndFrameRaw), false },
        };

        for (size_t i = 0; i < lines.size(); ++i)
        {
            const std::string stripped = StripCombatComment(lines[i]);
            const std::string trimmed = TrimCombatLine(stripped);
            const int depthBefore = depth;

            if (!inCharacterBlock && depthBefore == 0 && trimmed == "character")
            {
                foundCharacter = true;
                inCharacterBlock = true;
                characterHeaderDepth = depthBefore;
            }
            else if (inCharacterBlock && depthBefore >= characterHeaderDepth + 1)
            {
                // Any depth inside the character block - the four field names are distinct
                // across the whole file, so no need to also track which sub-block (correction /
                // ko) a given line sits in.
                const size_t firstNonSpace = lines[i].find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos)
                {
                    const size_t tokenEnd = trimmed.find_first_of(" \t");
                    const std::string fieldName = tokenEnd == std::string::npos ? trimmed : trimmed.substr(0, tokenEnd);
                    for (FieldTarget& target : targets)
                    {
                        if (!target.written && fieldName == target.name)
                        {
                            const std::string indent = lines[i].substr(0, firstNonSpace);
                            lines[i] = indent + target.name + " " + FormatCombatNumber(target.value);
                            target.written = true;
                            break;
                        }
                    }
                }
            }

            for (const char c : stripped)
            {
                if (c == '{') ++depth;
                else if (c == '}') --depth;
            }

            if (inCharacterBlock)
            {
                if (depth > characterHeaderDepth)
                {
                    enteredCharacterBody = true;
                }
                else if (enteredCharacterBody && depth == characterHeaderDepth)
                {
                    break; // Character block closed - nothing left to find.
                }
            }
        }

        if (!foundCharacter)
        {
            Foundation::Log::Write(Severity::Error, "MoveTable", "SaveCharacterToFile: no 'character' block in '%s'", filePath.c_str());
            return false;
        }
        bool allWritten = true;
        for (const FieldTarget& target : targets)
        {
            if (!target.written)
            {
                Foundation::Log::Write(
                    Severity::Error, "MoveTable", "SaveCharacterToFile: no '%s' field line to rewrite in '%s'", target.name,
                    filePath.c_str());
                allWritten = false;
            }
        }
        if (!allWritten)
        {
            return false;
        }

        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Foundation::Log::Write(Severity::Error, "MoveTable", "SaveCharacterToFile: failed to open '%s' for writing", filePath.c_str());
            return false;
        }
        for (const std::string& outLine : lines)
        {
            out << outLine << "\n";
        }
        out.close();

        Foundation::Log::Write(Severity::Info, "MoveTable", "SaveCharacterToFile: saved to '%s'", filePath.c_str());
        return true;
    }
}
