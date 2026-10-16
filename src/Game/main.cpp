#include "Asset/GltfImporter.h"
#include "Engine/Animation.h"
#include "Engine/Animator.h"
#include "Engine/Audio.h"
#include "Engine/Camera.h"
#include "Engine/Collision.h"
#include "Engine/Components.h"
#include "Engine/Engine.h"
#include "Engine/FixedTimestep.h"
#include "Engine/FrameCapture.h"
#include "Engine/Input.h"
#include "Engine/ModelResources.h"
#include "Foundation/Debug.h"
#include "Foundation/Input.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"
#include "Renderer/Buffer.h"
#include "Renderer/DebugDrawPass.h"
#include "Renderer/RenderTasks.h"
#include "CaptureRun.h"
#include "CombatDsl.h"
#include "FighterShadow.h"
#include "FighterState.h"
#include "GameConstants.h"
#include "GroundPlane.h"
#include "InputBindings.h"
#include "LocalTestScene.h"
#include "AiOpponent.h"
#include "Hud.h"
#include "SfxCues.h"
#include "Juice.h"
#include "MatchFlow.h"
#include "MoveTable.h"
#include "PrototypeConfig.h"
#include "Projectile.h"
#include "Vfx.h"
#include "Backdrop.h"
#include "MainMenu.h"
#include "Splash.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <entt.hpp>
#include <filesystem>
#include <functional>
#include <imgui.h>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// DirectX Agility SDK activation. Must live in the exe (not a static lib) -
// the D3D12 loader only looks for these exports in the main module. Keep
// D3D12SDKVersion in sync with the NuGet package version in
// build/main.sharpmake.cs ("1.619.5" -> 619) and D3D12SDKPath with
// AgilitySdk.SdkPath there.
extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    Foundation::CreateDebugConsole("OmdLab - Debug Console");

    Foundation::Log::Init("logs/omdlab.log");
    Foundation::Log::Write(Foundation::Log::Severity::Info, "Game", "OmdLab starting up");

    // "--capture <script>": run a deterministic scripted capture (see CaptureRun.h), then exit.
    // Parsed here; acted on in the render loop. Script names are ASCII.
    std::string captureScriptName;
    {
        const std::wstring cmd = commandLine != nullptr ? commandLine : L"";
        const size_t flag = cmd.find(L"--capture");
        if (flag != std::wstring::npos)
        {
            const size_t start = cmd.find_first_not_of(L" \t", flag + 9);
            if (start != std::wstring::npos)
            {
                const size_t end = cmd.find_first_of(L" \t", start);
                const std::wstring token = cmd.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
                for (const wchar_t wc : token)
                {
                    // Script names are ASCII - narrow one char at a time, dropping anything else.
                    if (wc > 0 && wc < 128)
                    {
                        captureScriptName += static_cast<char>(wc);
                    }
                }
            }
        }
    }

#if defined(OMD_PROTOTYPE)
    // "--seed N": the AI simulation seed (also used by the headless ai_sim capture).
    uint32_t initialAiSeed = 1;
    if (commandLine != nullptr)
    {
        const std::wstring cmd(commandLine);
        const size_t seedFlag = cmd.find(L"--seed ");
        if (seedFlag != std::wstring::npos)
        {
            initialAiSeed = static_cast<uint32_t>(_wtoi(cmd.c_str() + seedFlag + 7));
        }
    }
#endif
#if defined(OMD_PROTOTYPE)
    // "--hud" alongside --capture keeps the overlay in the captured frames (off by default so
    // scripted output stays comparable between runs).
    const bool captureKeepsHud = commandLine != nullptr && std::wstring(commandLine).find(L"--hud") != std::wstring::npos;
    const bool captureKeepsJuice = commandLine != nullptr && std::wstring(commandLine).find(L"--juice") != std::wstring::npos;
    // Launch modes. Default: straight into the game with the debug tooling, as usual. --menu:
    // the main menu first, then the usual game. --demo: boot statement, main menu, and "Play"
    // starts the AI simulation with the debug UI hidden (for recording footage).
    bool demoMode = commandLine != nullptr && std::wstring(commandLine).find(L"--demo") != std::wstring::npos;
    const bool menuRequested = demoMode || (commandLine != nullptr && std::wstring(commandLine).find(L"--menu") != std::wstring::npos);
    const bool scriptedRun = commandLine != nullptr && std::wstring(commandLine).find(L"--capture") != std::wstring::npos;
    const bool captureKeepsBackdrop = commandLine != nullptr && std::wstring(commandLine).find(L"--backdrop") != std::wstring::npos;
    const bool captureKeepsVfx = commandLine != nullptr && std::wstring(commandLine).find(L"--vfx") != std::wstring::npos;
#endif

    Engine::PrintDependencyChain();

    OMD_DEBUG_PRINT("Debug print smoke test, value = %d", 42);
    OMD_ASSERT(1 + 1 == 2, "Sanity check failed: math is broken");
    Game::CombatDsl::RunSelfTest();

    constexpr unsigned int windowWidth = 1280;
    constexpr unsigned int windowHeight = 720;

    HWND window = Foundation::CreateGameWindow("Fighter Lab", windowWidth, windowHeight);
    if (window == nullptr)
    {
        Foundation::Log::Shutdown();
        return -1;
    }

    Renderer::RenderTasks::Init(window, windowWidth, windowHeight);

    // Shared fighter state topology (one file, whole roster) - loaded once, referenced by every
    // fighter entity's per-tick state-machine evaluation (not wired to gameplay until a later
    // step in this same pass). Asserting its expected shape here, at load time, since this file
    // and the state-name-specific meaning elsewhere are two independently-editable places that
    // must agree - a typo in either should fail loudly at startup, not silently misbehave later.
    Game::CombatDsl::CombatFile sharedStates;
    const bool sharedStatesLoaded = Game::CombatDsl::LoadCombatFile("data/combat_shared/states.combat", sharedStates);
    OMD_ASSERT(sharedStatesLoaded, "Failed to load data/combat_shared/states.combat");
    OMD_ASSERT(sharedStates.states.size() == 7, "Expected 7 states in states.combat, got %zu", sharedStates.states.size());
    for (const char* expectedState : { "Idle", "Walk", "Run", "Jump", "Attack", "HitStun", "KO" })
    {
        bool found = false;
        for (const Game::CombatDsl::StateDecl& state : sharedStates.states)
        {
            if (state.name == expectedState)
            {
                found = true;
                break;
            }
        }
        OMD_ASSERT(found, "states.combat is missing expected state '%s'", expectedState);
    }

    // This character - identity, stats, per-asset render corrections, and (via the "character"
    // block's own "moves" field) its moveset - all from one file. modelDirectory is set to that
    // file's own containing directory, so the whole character folder is relocatable.
    constexpr const char* kCharacterFilePath = "data/characters/polyone_stick_man/character.combat";
    // Still named separately: the hitbox-tuning debug tool's "Save" button writes back to this
    // exact file, and character.combat only references it by a directory-relative name.
    constexpr const char* kMovesFilePath = "data/characters/polyone_stick_man/moves.combat";
    Game::CharacterDefinition characterDefinition;
    {
        const bool loaded = Game::LoadCharacterDefinition(kCharacterFilePath, characterDefinition);
        OMD_ASSERT(loaded, "Failed to load character.combat");
        // character.combat and the code reading these fields are independently editable - a
        // missing/renamed field should fail loudly here, not silently zero out later.
        OMD_ASSERT(!characterDefinition.displayName.empty(), "character.combat has no 'name'");
        OMD_ASSERT(!characterDefinition.modelFile.empty(), "character.combat has no 'model'");
        OMD_ASSERT(characterDefinition.stats.maxHealth > 0, "character.combat stats.maxHealth must be > 0");
    }
    Foundation::Log::Write(
        Foundation::Log::Severity::Info, "Game",
        "Loaded character '%s': model=%s/%s health=%d walk=%.2f run=%.2f jump=%.2f groundingOffsetY=%.3f koDrop=%.2f frames[%u..%u]",
        characterDefinition.displayName.c_str(), characterDefinition.modelDirectory.c_str(), characterDefinition.modelFile.c_str(),
        characterDefinition.stats.maxHealth, characterDefinition.stats.walkSpeed, characterDefinition.stats.runSpeed,
        characterDefinition.stats.jumpSpeed, characterDefinition.groundingOffsetY, characterDefinition.koDropOffsetY,
        characterDefinition.koDropStartFrame, characterDefinition.koDropEndFrame);
    for (const Game::MoveDefinition& move : characterDefinition.moveTable.moves)
    {
        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Game", "Loaded move '%s': clip=%s damage=%d startup=%u active=%u recovery=%u cancels=%zu",
            move.id.c_str(), move.animationClip.c_str(), move.damage, move.startupFrames, move.activeFrames, move.recoveryFrames,
            move.cancels.size());
    }

    // The one entity in the scene so far. FighterState/Health/input-association aren't
    // components yet - they wait for a state machine to actually need them.
    entt::registry registry;
    const entt::entity characterEntity = registry.create();
    registry.emplace<Engine::Transform>(characterEntity);

    // Character asset, currently rendered in bind pose. Engine's connective resource layer
    // does the Asset-CPU-data-to-Renderer-GPU-resource translation, the same generic path the
    // local test scene below also goes through. Directory + file both come from character.combat
    // now (modelDirectory is its own folder, modelFile the "model" field).
    const std::string characterModelPath = characterDefinition.modelDirectory + "/" + characterDefinition.modelFile;
    Asset::Model characterModel;
    std::vector<Renderer::SkinnedMeshDrawItem> characterDrawItems;
    // The dummy fighter's own definition - a separate CharacterDefinition, not aliased to the
    // player's, since MoveSource::moveTable is a raw pointer into a CharacterDefinition's own
    // moveTable address; sharing the player's would point the dummy's MoveSource at the
    // player's real move table. Loaded from the same character.combat as the player (it IS the
    // same character - same model, stats, and render corrections), then its moveTable.moves is
    // cleared: an empty move table is the entire mechanism behind "no AI, never attacks"
    // (SelectMove over an empty table always returns nullopt, so Attack can never fire), with
    // zero new branching anywhere else. The one hardcoded "punish" move below is the sole,
    // deliberate exception.
    Game::CharacterDefinition dummyDefinition;
    {
        const bool dummyLoaded = Game::LoadCharacterDefinition(kCharacterFilePath, dummyDefinition);
        OMD_ASSERT(dummyLoaded, "Failed to load character.combat for the dummy fighter");
#if !defined(OMD_PROTOTYPE)
        dummyDefinition.moveTable.moves.clear();
#endif
    }
    std::vector<Renderer::SkinnedMeshDrawItem> dummyDrawItems;
    if (Asset::ImportGltf(characterModelPath.c_str(), characterModel))
    {
        // assetCorrection (the 0.01-unit-scale undo) and facingCorrectionRadians (the +Z-facing
        // -> side-profile turn) were both filled by LoadCharacterDefinition from character.combat's
        // "correction" block - see that file for why this asset needs each. Only assetCorrection
        // bakes into the vertex data here; the entity's own Transform is still {0,0,0}/identity
        // at this point (nothing has moved yet), and facingCorrection is deliberately applied
        // per-frame instead (see the render loop's own comment on why).
        const DirectX::XMMATRIX assetCorrection = DirectX::XMLoadFloat4x4(&characterDefinition.assetCorrection);
        characterDrawItems = Engine::CreateSkinnedMeshDrawItems(characterModel, characterDefinition.modelDirectory, assetCorrection);
        if (!characterDrawItems.empty())
        {
            registry.emplace<Engine::SkinnedRenderable>(characterEntity, Engine::SkinnedRenderable{ &characterDrawItems[0] });
        }

        // One-time name->index resolution for every hitbox's optional bone attachment - needs
        // the real imported model, so this is the earliest point it can run.
        Game::ResolveHitboxJoints(characterModel, characterDefinition.moveTable);
        // Game-feel layer: auto-detect each move's strike/coil clip fractions from its clip +
        // hitbox bone, then resolve `impact` (and any explicit override) into concrete
        // timing/hitstop/lunge values. Order matters - Analyze needs the resolved joint index
        // above, Finalize needs Analyze's detected fractions.
        Game::AnalyzeMoveTiming(characterModel, characterModel.clips, characterDefinition.moveTable);
        Game::FinalizeMoveTiming(characterDefinition.moveTable);
#if defined(OMD_PROTOTYPE)
        // The opponent fights with the full moveset (it is loaded from the same file), prepared
        // exactly like the player's.
        Game::ResolveHitboxJoints(characterModel, dummyDefinition.moveTable);
        Game::AnalyzeMoveTiming(characterModel, characterModel.clips, dummyDefinition.moveTable);
        Game::FinalizeMoveTiming(dummyDefinition.moveTable);
#endif
        // Locomotion blend space (idle/walk/run by signed movement speed) - the animation
        // source for the Idle/Walk/Run states. Built per CharacterDefinition (both already
        // loaded from the same character.combat, so same clips + stats).
        Game::BuildLocomotionBlendSpace(characterModel.clips, characterDefinition);
        Game::BuildLocomotionBlendSpace(characterModel.clips, dummyDefinition);
        // Per-node blend weights for the in-place jump's held-calm-pose overlay.
        Game::BuildJumpFloatMask(characterModel, characterDefinition);
        Game::BuildJumpFloatMask(characterModel, dummyDefinition);

        // Dummy already carries its own copy of every scalar field (loaded from the same
        // character.combat above), but needs its own GPU-backed draw item instance -
        // CreateSkinnedMeshDrawItems's own persistent per-instance world/bone-palette buffers
        // are exactly why a second real fighter needs a second call here, not a shared draw item
        // (reusing one buffer across items was a real historical bug - see StaticMeshDrawItem's
        // own comment).
        dummyDrawItems = Engine::CreateSkinnedMeshDrawItems(characterModel, dummyDefinition.modelDirectory, assetCorrection);

        // One hardcoded "punish" move for the "Block then punish" AI preset (debugSectionUI/
        // tick loop below) - not authored in a .combat file, same "construct a MoveDefinition
        // directly in code" precedent the old dev-test-hitbox rig used. Frame data/damage/
        // hitboxes are copied from the player's own already-tuned/already-resolved "punch"
        // (MoveDefinition itself is move-only - Cancel holds a unique_ptr - so only the
        // copyable fields are taken, not the whole struct; hitboxes are separately copyable and
        // already carry a resolved bone-joint index from ResolveHitboxJoints just above, so this
        // doesn't need its own resolve pass). This is a deliberate, narrow exception to
        // dummyDefinition.moveTable being otherwise empty ("no AI, never attacks" - see its own
        // declaration) - the dummy still can never Attack on its own; only this file's explicit
        // AI-preset logic ever presses the button that selects this move.
        if (const Game::MoveDefinition* realPunch = characterDefinition.moveTable.FindById("punch"))
        {
            Game::MoveDefinition punishMove;
            punishMove.id = "dummy_punish";
            punishMove.displayName = "dummy_punish";
            punishMove.animationClip = realPunch->animationClip;
            punishMove.inputButton = realPunch->inputButton;
            punishMove.startupFrames = realPunch->startupFrames;
            punishMove.activeFrames = realPunch->activeFrames;
            punishMove.recoveryFrames = realPunch->recoveryFrames;
            punishMove.onHitStunFrames = realPunch->onHitStunFrames;
            punishMove.onBlockStunFrames = realPunch->onBlockStunFrames;
            punishMove.damage = realPunch->damage;
            punishMove.guardHeight = realPunch->guardHeight;
            punishMove.hitboxes = realPunch->hitboxes;
            // Copy the already-resolved game-feel layer too, so the dummy's punish jab reads with
            // the same weight/snap/hitstop the player's punch does (FinalizeMoveTiming ran above).
            punishMove.impact = realPunch->impact;
            punishMove.timingContactFraction = realPunch->timingContactFraction;
            punishMove.timingWindupFraction = realPunch->timingWindupFraction;
            punishMove.timingSnapFrames = realPunch->timingSnapFrames;
            punishMove.hitStopFrames = realPunch->hitStopFrames;
            punishMove.lungeDistance = realPunch->lungeDistance;
            punishMove.timingCurveActive = realPunch->timingCurveActive;
            dummyDefinition.moveTable.moves.push_back(std::move(punishMove));
        }
#if defined(OMD_PROTOTYPE)
        Game::CopyFireballMoves(characterDefinition.moveTable, dummyDefinition.moveTable);
#endif
    }

    // Resolved once, reused every frame below - the skinned mesh node's own skin (this asset
    // has exactly one). Null when absent (e.g. a future non-animated skinned asset), in which
    // case the pose update below is just skipped and the character stays in the identity bind
    // pose CreateSkinnedMeshDrawItems already left it in.
    const Asset::Skin* characterSkin = nullptr;
    for (const Asset::Node& node : characterModel.nodes)
    {
        if (node.skinIndex != Asset::kInvalidIndex)
        {
            characterSkin = &characterModel.skins[node.skinIndex];
            break;
        }
    }
    registry.emplace<Engine::Animator>(characterEntity);
#ifdef OMD_DEV_TOOLS
    // Debug-UI clip combo contents - built once since Asset::Clip::name (and characterModel
    // itself) don't change after import. Only ever read by primaryContentUI below, so gated
    // alongside it rather than built and left unused.
    std::vector<const char*> characterClipNames;
    for (const Asset::Clip& clip : characterModel.clips)
    {
        characterClipNames.push_back(clip.name.c_str());
    }
    // Hitbox tuning debug-UI move combo contents - "None" first so index 0 means
    // "tuning off" without needing a separate bool alongside hitboxTuningMoveIndex. Built once,
    // same reasoning as characterClipNames above - MoveDefinition::displayName (defaults to the
    // move's own id, see BuildMoveTable) doesn't change after load.
    std::vector<const char*> hitboxTuningMoveNames{ "None" };
    for (const Game::MoveDefinition& move : characterDefinition.moveTable.moves)
    {
        hitboxTuningMoveNames.push_back(move.displayName.c_str());
    }
#endif
    // Default to "idle" by name, not clip index 0 - the export pipeline's clip order isn't
    // something this code controls (observed to land alphabetically), so relying on index 0
    // to mean anything in particular would be a silent, easy-to-break assumption now that the
    // character has dozens of clips instead of one.
    for (size_t i = 0; i < characterModel.clips.size(); ++i)
    {
        if (characterModel.clips[i].name == "idle")
        {
            registry.get<Engine::Animator>(characterEntity).clipIndex = static_cast<int32_t>(i);
            break;
        }
    }

    // A first estimate of the character's body extents (feet at Transform.position, ~1.8-unit
    // standing height) - no per-state hurtbox profile exists yet, so this one box is always
    // active regardless of pose. Checked visually against the "Collision debug draw" toggle
    // below, not just guessed and left unverified. offset.y carries groundingOffsetY too, so the
    // body volume drops with the rendered mesh (see CharacterDefinition::groundingOffsetY) -
    // baked in here at startup, so a live groundingOffsetY tweak needs a restart to show up in
    // this box (the mesh render and bone-attached hitboxes both read it live every frame/tick).
    registry.emplace<Engine::Hurtbox>(
        characterEntity,
        Engine::Hurtbox{ { Engine::CollisionBox{ { 0.0f, 0.9f + characterDefinition.groundingOffsetY, 0.0f }, { 0.35f, 0.9f, 0.25f } } } });

    registry.emplace<Game::FighterState>(characterEntity);
    registry.emplace<Game::Health>(
        characterEntity, Game::Health{ characterDefinition.stats.maxHealth, characterDefinition.stats.maxHealth });
    registry.emplace<Game::MoveSource>(characterEntity, Game::MoveSource{ &characterDefinition.moveTable });
#if defined(OMD_PROTOTYPE)
    registry.emplace<Game::ProjectileCaster>(characterEntity);
#endif

    // The dummy fighter - a real second entity (no AI, stands still, blocks only via a debug
    // checkbox below) to test the player's real moveset against a real Hurtbox, replacing the
    // old dev-only test-hitbox rig this same rig used to stand in for one. Real content, not
    // dev-only (see rebuildDrawItems below), same "always included" precedent groundPlaneDrawItems
    // already sets - so unlike the removed test rig, this is unconditional, not OMD_DEV_TOOLS-gated.
    const entt::entity dummyEntity = registry.create();
    registry.emplace<Engine::Transform>(dummyEntity, Engine::Transform{ { Game::kDefaultFighterGapX, 0.0f, 0.0f } });
    if (!dummyDrawItems.empty())
    {
        registry.emplace<Engine::SkinnedRenderable>(dummyEntity, Engine::SkinnedRenderable{ &dummyDrawItems[0] });
    }
    registry.emplace<Engine::Animator>(dummyEntity);
    for (size_t i = 0; i < characterModel.clips.size(); ++i)
    {
        if (characterModel.clips[i].name == "idle")
        {
            registry.get<Engine::Animator>(dummyEntity).clipIndex = static_cast<int32_t>(i);
            break;
        }
    }
    registry.emplace<Engine::Hurtbox>(
        dummyEntity,
        Engine::Hurtbox{ { Engine::CollisionBox{ { 0.0f, 0.9f + dummyDefinition.groundingOffsetY, 0.0f }, { 0.35f, 0.9f, 0.25f } } } });
    registry.emplace<Game::FighterState>(dummyEntity);
    registry.emplace<Game::Health>(dummyEntity, Game::Health{ dummyDefinition.stats.maxHealth, dummyDefinition.stats.maxHealth });
    // Resolves a HitEvent's move when the dummy is the attacker (its table holds only the
    // scripted punish swing and the fireball entries).
    registry.emplace<Game::MoveSource>(dummyEntity, Game::MoveSource{ &dummyDefinition.moveTable });
#if defined(OMD_PROTOTYPE)
    registry.emplace<Game::ProjectileCaster>(dummyEntity);
#endif

#ifdef OMD_DEV_TOOLS
    // Collision module test rig - the hitbox half (a draggable ImGui-slider "attacker") is
    // superseded by the dummy above and removed; this trigger volume stays, since nothing else
    // in the game exercises TriggerVolume/trigger events yet. Dev-only by construction (there's
    // no non-debug reason for this entity to exist), so gated entirely rather than just hidden
    // from a UI that also wouldn't exist.
    const entt::entity testTriggerEntity = registry.create();
    registry.emplace<Engine::Transform>(testTriggerEntity, Engine::Transform{ { -2.0f, 0.9f, 0.0f } });
    registry.emplace<Engine::TriggerVolume>(
        testTriggerEntity, Engine::TriggerVolume{ Engine::CollisionBox{ {}, { 0.5f, 1.0f, 0.5f } }, /*triggerId*/ 1 });
#endif

#ifdef OMD_DEV_TOOLS
    // Dev-only stress-test content (see the "Bulk external test content" working convention) -
    // empty (and a no-op) when local/ isn't present. No non-dev-tools reason for this content
    // to ever exist, so its loading is gated entirely, not just its debug checkbox.
    std::vector<Renderer::StaticMeshDrawItem> localTestSceneDrawItems = LocalTestScene::LoadIfAvailable();
    // Off by default - dev-only content (see the "Bulk external test content" working
    // convention), not something the default view should depend on.
    bool enableLocalTestScene = false;
#endif

    // The stage fighters stand/fight on - real shipped content, not dev-only, so always
    // included below rather than gated behind a checkbox the way the local test scene is.
#if defined(OMD_PROTOTYPE)
    const Game::StageDrawItems stageDrawItems = Game::CreateStageDrawItems();
    std::vector<Renderer::StaticMeshDrawItem> groundPlaneDrawItems = stageDrawItems.floor;
    groundPlaneDrawItems.insert(groundPlaneDrawItems.end(), stageDrawItems.walls.begin(), stageDrawItems.walls.end());
    float floorScrollSeconds = 0.0f;
#else
    std::vector<Renderer::StaticMeshDrawItem> groundPlaneDrawItems{ Game::CreateGroundPlaneDrawItem() };
#endif
    // One small "grounding" shadow quad per fighter - see FighterShadow.h's own comment for why
    // this is a flat opaque patch, not a real soft shadow. Repositioned every render frame
    // (below) to track each fighter's current X/Z - kept as separate persistent draw items
    // (own world buffers), same "each instance owns its own buffer" reasoning every other
    // per-instance draw item in this project already follows.
    Renderer::StaticMeshDrawItem playerShadowDrawItem = Game::CreateFighterShadowDrawItem();
    Renderer::StaticMeshDrawItem dummyShadowDrawItem = Game::CreateFighterShadowDrawItem();
#if defined(OMD_PROTOTYPE)
    Game::ProjectileSystem projectileSystem = Game::CreateProjectileSystem(4);
    Game::VfxSystem vfxSystem = Game::CreateVfxSystem(384);
    Game::Backdrop backdrop = Game::CreateBackdrop("data/images/Seamless Low-Poly Dungeon Tile.png");
    // The main menu: shown at start (not in scripted runs), over the dark backdrop with only the
    // torch fire; the fighters, floor and HUD are left out of the draw lists while it is up.
    bool menuAllowed = menuRequested && !scriptedRun && backdrop.loaded;
    bool menuActive = menuAllowed;
    bool restartDemoRequested = false; // Set by the debug window's button; handled at the top of the next frame.
    Game::Splash splash = Game::CreateSplash();
    bool splashActive = demoMode && menuAllowed;
    float splashTime = 0.0f;
    Game::MainMenuState menuState;
    bool menuEscWas = true; // Starts "held" so a key held from before does not count as an edge.
#endif

    // The character (GPU-skinned) and local test scene/ground plane (rigid) are different draw
    // item types feeding different passes - StaticMeshPass/SkinnedMeshPass never learn a
    // "character"/"local test scene"/"ground plane" exists (the Renderer/Asset dependency rule),
    // each only ever sees its own flat draw item list. Which categories are actually included is
    // a Game-owned decision, re-applied whenever the debug checkboxes below change - cheap,
    // since the underlying GPU buffers/textures were already uploaded once above and this only
    // rebuilds the item lists.
    bool enableCharacter = true;
#if defined(OMD_PROTOTYPE)
    // TODO(OM): replace with real shadow rendering (shadow maps or a contact-shadow pass).
    // Planar projected shadows: each fighter's skinned mesh drawn a second time, flattened onto
    // the floor (in the vertex shader) along the hardcoded light direction (0.4, -1, 0.3), as a flat
    // dark. The copies share the originals' vertex data and bone palette, so they follow the
    // live pose with no extra skinning work.
    std::vector<Renderer::SkinnedMeshDrawItem> characterShadowItems = characterDrawItems;
    std::vector<Renderer::SkinnedMeshDrawItem> dummyShadowItems = dummyDrawItems;
    {
        // A negative alpha selects SkinnedMesh.hlsl's shadow projection; the rgb is the factor
        // the floor colour under the shadow is multiplied by (lower = darker shadow).
        const DirectX::XMFLOAT4 shadowTint = { 0.62f, 0.62f, 0.62f, -1.0f };
        const Renderer::BufferHandle shadowTintBuffer = Renderer::Buffer::Create(&shadowTint, sizeof(shadowTint));
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        for (std::vector<Renderer::SkinnedMeshDrawItem>* items : { &characterShadowItems, &dummyShadowItems })
        {
            for (Renderer::SkinnedMeshDrawItem& item : *items)
            {
                item.worldBuffer = Renderer::Buffer::Create(&identity, sizeof(identity));
                item.tintBuffer = shadowTintBuffer;
                item.shadow = true;
            }
        }
    }
    // TODO(OM): replace per-fighter tints with real palettes/costumes.
    const DirectX::XMFLOAT4 fighterBaseTint[2] = { { 0.72f, 0.86f, 1.0f, 1.0f }, { 1.0f, 0.74f, 0.68f, 1.0f } };
    Renderer::BufferHandle fighterTintBuffer[2];
    {
        const Renderer::BufferHandle firstTintBuffer = Renderer::Buffer::Create(&fighterBaseTint[0], sizeof(DirectX::XMFLOAT4));
        const Renderer::BufferHandle secondTintBuffer = Renderer::Buffer::Create(&fighterBaseTint[1], sizeof(DirectX::XMFLOAT4));
        fighterTintBuffer[0] = firstTintBuffer;
        fighterTintBuffer[1] = secondTintBuffer;
        for (Renderer::SkinnedMeshDrawItem& item : characterDrawItems)
        {
            item.tintBuffer = firstTintBuffer;
        }
        for (Renderer::SkinnedMeshDrawItem& item : dummyDrawItems)
        {
            item.tintBuffer = secondTintBuffer;
        }
    }
#endif
    auto rebuildDrawItems = [&]()
    {
        // Dummy's own draw items are concatenated in unconditionally - real content, like the
        // ground plane below, not gated behind enableCharacter (which stays player-only).
        std::vector<Renderer::SkinnedMeshDrawItem> skinnedItems;
        if (menuActive)
        {
            // Menu: nothing but the dark backdrop and the effects (torch fire).
            Renderer::SkinnedMeshPass::SetDrawItems(skinnedItems);
            std::vector<Renderer::StaticMeshDrawItem> menuStatic;
            menuStatic.insert(menuStatic.end(), backdrop.menuItems.begin(), backdrop.menuItems.end());
            menuStatic.insert(menuStatic.end(), backdrop.capItems.begin(), backdrop.capItems.end());
            menuStatic.insert(menuStatic.end(), vfxSystem.drawItems.begin(), vfxSystem.drawItems.end());
            menuStatic.insert(menuStatic.end(), splash.items.begin(), splash.items.end());
            Renderer::StaticMeshPass::SetDrawItems(menuStatic);
            return;
        }
        if (enableCharacter)
        {
            skinnedItems.insert(skinnedItems.end(), characterDrawItems.begin(), characterDrawItems.end());
        }
        skinnedItems.insert(skinnedItems.end(), dummyDrawItems.begin(), dummyDrawItems.end());
#if defined(OMD_PROTOTYPE)
        if (enableCharacter)
        {
            skinnedItems.insert(skinnedItems.end(), characterShadowItems.begin(), characterShadowItems.end());
        }
        skinnedItems.insert(skinnedItems.end(), dummyShadowItems.begin(), dummyShadowItems.end());
#endif
        Renderer::SkinnedMeshPass::SetDrawItems(skinnedItems);
        std::vector<Renderer::StaticMeshDrawItem> staticItems = groundPlaneDrawItems;
        // Same enableCharacter/unconditional split as the skinned items above - player's shadow
        // follows whether the player mesh itself is shown, dummy's shadow is always there.
#if !defined(OMD_PROTOTYPE)
        if (enableCharacter)
        {
            staticItems.push_back(playerShadowDrawItem);
        }
#endif
#if !defined(OMD_PROTOTYPE)
        staticItems.push_back(dummyShadowDrawItem);
#endif
#if defined(OMD_PROTOTYPE)
        staticItems.insert(staticItems.end(), projectileSystem.drawItems.begin(), projectileSystem.drawItems.end());
        staticItems.insert(staticItems.end(), backdrop.items.begin(), backdrop.items.end());
        staticItems.insert(staticItems.end(), backdrop.capItems.begin(), backdrop.capItems.end());
        // Last: blended items draw in list order, so effects land over the other translucents.
        staticItems.insert(staticItems.end(), vfxSystem.poolItems.begin(), vfxSystem.poolItems.end());
        staticItems.insert(staticItems.end(), vfxSystem.drawItems.begin(), vfxSystem.drawItems.end());
#endif
#ifdef OMD_DEV_TOOLS
        if (enableLocalTestScene)
        {
            staticItems.insert(staticItems.end(), localTestSceneDrawItems.begin(), localTestSceneDrawItems.end());
        }
#endif
        Renderer::StaticMeshPass::SetDrawItems(staticItems);
    };
    rebuildDrawItems();

    Engine::Camera camera;
    // Camera follow toggle - unconditional (not just under OMD_DEV_TOOLS), since this free-fly
    // camera is the only camera that exists at all right now (see Engine::Camera's own header
    // comment - a real locked 2D-follow mode isn't built yet), so non-dev builds need this too,
    // not just a debug convenience. Only its on/off checkbox (debugSectionUI below) is
    // dev-tools-gated - same "unconditional bool, gated checkbox" pattern forceShowAllHitboxes/
    // manualClipOverride/dummyBlocksHeld already use.
    bool enableCameraFollow = true;
#ifdef OMD_DEV_TOOLS
    // The free-fly camera is itself dev-only tooling (see Engine::Camera's own header comment -
    // no real fixed 2D game camera exists yet), so this toggle lives entirely under dev tools
    // too. On by default, matching existing behavior. Combined with ImGui's own
    // io.WantCaptureMouse below - the toggle covers "I want the camera locked regardless of
    // what's under the cursor," WantCaptureMouse covers the common case (dragging a slider or
    // clicking a checkbox) automatically, without needing to remember to flip this first.
    bool enableCameraMouseControl = true;
    // Off by default, unlike the mouse toggle above - this camera's own W/S/A/D/Q/E and arrow
    // keys collide with ImGui keyboard interaction (typing a number, using Left/Right to move
    // the cursor inside a field, Ctrl+click-to-type on a slider) far more often than the mouse
    // toggle's own click/drag case, and typing into a debug field is the more common workflow
    // than flying the camera with the keyboard now that mouse-drag navigation exists (see
    // UpdateFreeFlyCamera's own header comment). Combined with ImGui's own io.WantCaptureKeyboard
    // below, the same "manual override + automatic ImGui-focus gate" pattern
    // enableCameraMouseControl/io.WantCaptureMouse already established.
    bool enableCameraKeyboardControl = false;
    // The free-fly camera's own gamepad move axis and the fighter's movement axis both read the
    // same physical left stick (Engine::GamepadAxis::LeftStickX) - moving the camera with a
    // gamepad inevitably also walks the character, which can walk it straight into the dev
    // test-hitbox rig (or off the stage edge) while someone's just trying to look around.
    // Deliberately scoped to the analog stick only (Engine::AssembleInputCommand's
    // ignoreGamepadAnalogAxis) - keyboard A/D and the D-pad (digital, not analog) still move the
    // character while this is on, since the stick is the only source that actually conflicts
    // with the camera. On by default - unlike a full input freeze, this doesn't cost any real
    // gameplay input (D-pad/keyboard movement both still work), so there's no reason to make
    // testing opt into it: stick drives the camera, D-pad drives the character, out of the box.
    bool disableCharacterAnalogStickMovement = true;
#endif
    // Fixed-timestep sim loop skeleton - no sim state exists yet to actually drive with this;
    // it proves ticks run at a fixed rate decoupled from render rate, and that a per-tick
    // InputCommand is produced and stored, ahead of a real fighter state machine becoming the
    // first real tick consumer.
    Engine::FixedTimestepAccumulator simClock;
    Engine::InputHistory playerInputHistory;
    Engine::InputCommand lastInputCommand;
    // The dummy's synthetic per-tick input - axis and every button stay at their default
    // (false/0) every tick except Block, driven by the "Dummy blocks" checkbox below.
    // dummyBlocksHeld itself is declared unconditionally (not just under OMD_DEV_TOOLS) since
    // the always-compiled tick loop below reads it every tick to build dummyInputCommand - same
    // existing pattern forceShowAllHitboxes/manualClipOverride already use (only their checkbox
    // is dev-tools-gated, not the underlying bool).
    Engine::InputHistory dummyInputHistory;
    Engine::InputCommand dummyInputCommand;
    bool dummyBlocksHeld = false;
    // AI behavior preset for the dummy - a testing convenience, not real AI (a few hardcoded
    // scripted reactions, no evaluation/decision tree): 0 = Manual (dummyBlocksHeld above drives
    // Block directly, today's original behavior), 1 = Always block, 2 = Block then punish (see
    // the tick loop below). Declared unconditionally, same reasoning as dummyBlocksHeld.
    int dummyAiPreset = 0;
    // Counts down to a scripted punish swing once the dummy's been hit (preset 2 only) - a
    // fixed delay, not real recovery-frame tracking of whichever move actually hit it (that
    // would need reading the attacker's own FighterState, more than this testing tool needs).
    // 0 means "not currently winding up to punish."
    int dummyPunishCountdown = 0;
    // Tick counter for the "Fireball" preset (3): the dummy casts every kDummyFireballPeriodTicks.
    int dummyFireballTimer = 0;
    // Auto-revives the dummy a beat after it's KO'd (full health, back to Idle) instead of
    // leaving it lying there until the Reset button is pressed - states.combat's own KO state
    // has no transition out by design (no round-reset flow exists yet), so without this every
    // combo-tuning session needing many repeated hits meant a manual Reset after every knockout.
    // Defaults on since that repeated-testing case is the common one this exists for; a real
    // "let it actually stay down" check is one click away in the panel below. Declared
    // unconditionally, same reasoning as dummyBlocksHeld above.
    bool dummyUndying = true;
    // Latest tick's collision resolution - read by the debug draw list below between ticks, the
    // same "store the final answer" shape lastInputCommand already uses.
    Engine::CollisionEvents lastCollisionEvents;
    // SelectMove()'s result this tick - debug readout only for now (Phase 3 of the state-machine
    // work), not yet load-bearing; a later phase feeds this into the Attack-state transition.
    std::optional<std::string> lastSelectedMove;
    // When true, the "Clip" dropdown below drives the character's ClipPlayback directly (the
    // pre-state-machine behavior, wall-clock Advance()'d, for eyeballing one clip in isolation)
    // and UpdateFighterState is skipped entirely for this entity so it can't fight back over the
    // manual selection. Off by default - real gameplay is game-driven, this is a testing aid.
    bool manualClipOverride = false;
    bool manualClipPlaying = true; // "Playing" checkbox for the manual-override preview (the Animator has no play flag).
    // Debug aid: shows the active move's hitbox for the whole Attack state instead of gating it
    // to the move's own authored active-frame window (often well under 200ms real-time) - lets a
    // bone-attached hitbox's tracking be watched continuously via "Collision debug draw" rather
    // than racing a blink-and-miss-it window. Never changes hit resolution itself. Off by
    // default - see FighterUpdateInput's own comment.
    bool forceShowAllHitboxes = false;
    // Hitbox authoring/adjustment tooling. -1 means tuning is off (real gameplay owns
    // the character normally, via UpdateFighterState below); otherwise indexes
    // characterDefinition.moveTable.moves, and the tick loop calls Game::PreviewMoveHitbox
    // instead of UpdateFighterState for this entity so the selected move's clip+hitbox loop
    // continuously (via hitboxTuningFrameCounter) without needing real input or a target to hit.
    // Takes priority over manualClipOverride if both are somehow on at once - simpler than
    // cross-disabling the two checkboxes for a debug-only tool. The sliders in debugSectionUI
    // below edit CharacterDefinition::moveTable in place (live, in memory) - a "Save" button next
    // to them calls Game::SaveHitboxToFile to persist the currently-tuned box into moves.combat
    // (a targeted text patch, not a full round-trip serializer - see that function's own comment)
    // once satisfied; until clicked, edits are memory-only and lost on restart, same as before
    // this button existed.
    int hitboxTuningMoveIndex = -1;
    int hitboxTuningBoxIndex = 0;
    uint32_t hitboxTuningFrameCounter = 0;
    // Feedback for the "Save" button below - set right after each click, cleared whenever the
    // selected move/hitbox changes (a stale "Saved" from a previous box would be misleading).
    std::string hitboxTuningSaveMessage;
    // When true, the tick loop below stops auto-advancing hitboxTuningFrameCounter, leaving it
    // exactly where the "Frame" slider (debugSectionUI) last set it - lets a specific keyframe be
    // held still and the hitbox's offset/half-extent tuned against it, instead of fighting a
    // continuously-looping preview. PreviewMoveHitbox itself is unaffected either way (it always
    // just renders whatever hitboxTuningFrameCounter currently holds).
    bool hitboxTuningPaused = false;

    // "Clip timing" panel state (Animation Editor window, animationEditorUI below) - shares the
    // move selection / Pause / Frame scrub state above (same previewMove, same live preview via
    // PreviewMoveHitbox), just its own Save button/message so a hitbox edit and a timing edit
    // save independently, same precedent as "Character"'s own separate save just below. Raw
    // authored ticks, same round-trip reasoning as koDropStartFrameRawUI (MoveDefinition::
    // timingOutFrames is rescaled by kCombatSpeedMultiplier at load - see MoveTable.cpp's own
    // comment on why it's treated as a game-feel tick count, not a hitbox-style frame offset) -
    // reseeded from the newly-selected move wherever hitboxTuningMoveIndex itself changes, since
    // each move's own timingOutFrames differs.
    int timingOutRawUI = 0;
    std::string timingSaveMessage;

    // "Character" tuning panel state (debugSectionUI below). groundingOffsetY / koDropOffsetY
    // sliders bind straight to characterDefinition (1:1 with character.combat). The two KO frame
    // sliders present RAW clip frames - characterDefinition stores them already divided by
    // kCombatSpeedMultiplier (see BuildCharacterDefinition), so these UI ints are seeded by
    // multiplying that back up, edited raw, then scaled back down into both definitions on
    // change, and passed raw to SaveCharacterToFile. Edits are memory-only until "Save" is
    // clicked, same contract as the hitbox tool.
    int koDropStartFrameRawUI =
        static_cast<int>(std::lround(static_cast<float>(characterDefinition.koDropStartFrame) * Game::kCombatSpeedMultiplier));
    int koDropEndFrameRawUI =
        static_cast<int>(std::lround(static_cast<float>(characterDefinition.koDropEndFrame) * Game::kCombatSpeedMultiplier));
    std::string characterTuningSaveMessage;

    // Data-driven, hot-reloaded (see the polling below) - MakeDefaultFighterBindings() is only
    // the fallback for a missing/malformed file, never itself edited to rebind anything anymore.
    constexpr const char* kInputBindingsPath = "data/input_bindings.txt";
    Engine::InputBindings fighterBindings = Game::MakeDefaultFighterBindings();
    if (!Game::LoadFighterBindingsFromFile(kInputBindingsPath, fighterBindings))
    {
        Foundation::Log::Write(
            Foundation::Log::Severity::Warning, "Game", "Falling back to built-in default input bindings (failed to load %s)",
            kInputBindingsPath);
    }
    std::filesystem::file_time_type lastBindingsWriteTime{};
    {
        std::error_code writeTimeError;
        lastBindingsWriteTime = std::filesystem::last_write_time(kInputBindingsPath, writeTimeError);
    }
    // Checked at most once a second (see the timer below) - editing a bindings file and tabbing
    // back to see it take effect is not a sub-second-latency workflow, and this avoids stat()-ing
    // the file 60+ times a second for no reason.
    float bindingsReloadTimer = 0.0f;

    auto lastFrameTime = std::chrono::steady_clock::now();

#ifdef OMD_DEV_TOOLS
    // Capture-harness hotkeys (dev tools only). F9: screenshot the frame about to be rendered.
    // Ctrl+R: hide/show the debug UI so it isn't in the shot or eating the frame. Polled via
    // GetAsyncKeyState (Foundation::IsKeyDown), same focus-independent approach the free-fly
    // camera uses; edge-detected against the previous frame so a held key fires once.
    bool captureDebugUIVisible = !demoMode;
    bool f9WasDown = false;
    bool ctrlRWasDown = false;

    // Whether the "Animation Editor" window (clip preview/scrub + hitbox tuning - see
    // extraWindowsUI below) is open. Independent of captureDebugUIVisible: Ctrl+R hides every
    // debug window at once for a clean screenshot, this just controls whether this one
    // less-often-needed window is among them. Also flips false via its own titlebar close
    // button (ImGui::Begin's p_open) - toggling this checkbox and closing the window are the
    // same action either way.
    bool showAnimationEditor = false;

    // Combo debug (dev tools only): a rolling log of the player's own move-to-move transitions
    // (fresh Attack entries and same-state cancels alike), for judging whether a combo chain
    // actually fired or a follow-up input just missed its window - see the detection site
    // below (right after the player's own UpdateFighterState call) for how a transition is
    // told apart from an ordinary tick. viaCancel is kept as its own field (not baked into the
    // formatted text) so the ImGui readout below can color a real hit-confirmed link green
    // without re-parsing a string.
    struct ComboHistoryEntry
    {
        uint32_t tick = 0;
        bool viaCancel = false;
        std::string moveId;
    };
    std::string comboPrevState = "Idle";
    // Last-logged attackInstanceId (see FighterState.h's own comment) rather than
    // framesInMove == 1 - hitstop freezes framesInMove pinned at 1 for a couple of ticks right
    // after a cancel (the freeze from the PREVIOUS hit landing one tick late, same lag every
    // other reader of lastCollisionEvents already has), and framesInMove alone can't tell "still
    // frozen at 1" apart from "just entered" - confirmed live as the same cancel logging 2-3
    // times in a row once hit reactions stopped silently dropping. attackInstanceId only changes
    // on a genuine new EnterState call, immune to the freeze.
    uint32_t comboLastLoggedInstanceId = 0;
    std::vector<ComboHistoryEntry> comboHistory;

    // Capture harness: the skeleton-space "effector" logged to motion.csv defaults to the
    // right hand (an attack overrides it per tick with its own hitbox bone). Resolved once by
    // node name; -1 if the rig has no obvious hand node, which just leaves the effector
    // columns empty until an attack supplies a bone. Plus the previous tick's effector / COM
    // world positions, kept across loop iterations so per-frame speed is a clean difference.
    int captureEffectorNodeDefault = -1;
    for (size_t i = 0; i < characterModel.nodes.size(); ++i)
    {
        std::string n = characterModel.nodes[i].name;
        for (char& c : n)
        {
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        }
        if (n.find("righthand") != std::string::npos || n.find("right_hand") != std::string::npos ||
            n.find("hand_r") != std::string::npos)
        {
            captureEffectorNodeDefault = static_cast<int>(i);
            break;
        }
        if (captureEffectorNodeDefault < 0 && n.find("hand") != std::string::npos)
        {
            captureEffectorNodeDefault = static_cast<int>(i);
        }
    }
    if (captureEffectorNodeDefault >= 0)
    {
        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Game", "capture effector default node: %d '%s'", captureEffectorNodeDefault,
            characterModel.nodes[static_cast<size_t>(captureEffectorNodeDefault)].name.c_str());
    }
    DirectX::XMVECTOR capturePrevEffector = DirectX::XMVectorZero();
    DirectX::XMVECTOR capturePrevCom = DirectX::XMVectorZero();
    bool captureSkeletonHasPrev = false;

    // "--capture <script>": a deterministic scripted capture run - one sim tick per render
    // frame at the fixed timestep, scripted player input, debug UI off, exit when done.
    std::optional<Game::CaptureRun> captureRun;
    if (!captureScriptName.empty())
    {
        if (Game::CaptureRun::IsKnownScript(captureScriptName))
        {
            captureRun.emplace(captureScriptName);
            registry.get<Engine::Transform>(characterEntity) = Engine::Transform{ { captureRun->PlayerX(), 0.0f, 0.0f } };
            registry.replace<Game::FighterState>(characterEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                characterEntity, Game::Health{ characterDefinition.stats.maxHealth, characterDefinition.stats.maxHealth });
            // Most scripts want the established default (2.0 - out of every move's own reach,
            // so a solo capture never accidentally connects); a few combo scripts need the
            // dummy in real hitbox range instead, since a cancel only ever fires on
            // hitConfirmed (see CaptureRun::DummyX's own comment).
            registry.get<Engine::Transform>(dummyEntity) = Engine::Transform{ { captureRun->DummyX(), 0.0f, 0.0f } };
            registry.replace<Game::FighterState>(dummyEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                dummyEntity, Game::Health{ dummyDefinition.stats.maxHealth, dummyDefinition.stats.maxHealth });
            lastCollisionEvents = Engine::CollisionEvents{};
            simClock.accumulatedSeconds = 0.0f;
            // A combo-only follow-up move (e.g. "hook_light") has no input button reachable
            // from neutral by design - force the player straight into it once, here, rather
            // than relying on SelectMove (see CaptureRun::ForcedMove / ForceEnterAttackMove's
            // own comments).
            if (!captureRun->ForcedMove().empty())
            {
                Game::ForceEnterAttackMove(registry, characterEntity, characterDefinition, captureRun->ForcedMove());
            }
            // Camera pre-settled on the fighters' midpoint (player x=0, dummy x=2), follow ON.
            // Pre-positioning kills the first-~15-frames ease-in transient that otherwise
            // dominates motion.csv; follow then keeps a moving fighter (Walk/Run scripts) in
            // frame, and its motion is deterministic under the fixed capture timestep.
            camera = Engine::Camera{};
            camera.position.x = 1.0f;
            enableCameraFollow = true;
            captureDebugUIVisible = false;
        }
        else
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Error, "Game", "--capture: unknown script '%s' (known: %s)", captureScriptName.c_str(),
                Game::CaptureRun::KnownScriptList().c_str());
        }
    }
#endif

#if defined(OMD_PROTOTYPE)
    // TODO(OM): replace with real match flow and a screen-space HUD (see Hud.h / MatchFlow.h).
    Game::PrototypeConfig prototype;
    Game::MatchFlow matchFlow;
    Game::HudState hudState;
    float hudDeltaSeconds = 0.0f;
    Game::JuiceState juice;
    Game::SfxCues sfxCues;
    Engine::Audio::LoopId runLoop[2] = { -1, -1 };
    uint32_t whiffHandledInstance[2] = { 0, 0 };
    uint32_t sfxSeenCancelled = 0;
    uint32_t vfxSeenCancelled = 0;
    uint32_t vfxStepTicks[2] = { 0, 0 };
    bool audioReady = false;
    std::string currentMusic; // The track now playing ("" = none).
    float musicFade = 1.0f;
#ifdef OMD_DEV_TOOLS
    if (captureRun)
    {
        prototype.hud = captureKeepsHud;
        prototype.matchFlow = false;
        prototype.juice = captureKeepsJuice;
        prototype.vfx = captureKeepsVfx;
        prototype.backdrop = captureKeepsBackdrop;
        prototype.sfx = false;
        prototype.music = false;
    }
#endif
    // Puts both fighters back to a fresh round - same recovery the debug Reset button performs
    // for the fighters, without touching camera or tooling state.
    // Multiplier on both fighters' maximum health, applied when a round (re)starts. Adjustable in
    // the debug window.
    float healthScale = 1.0f;
    const auto resetRound = [&]()
    {
        const int scaledFirst = static_cast<int>(characterDefinition.stats.maxHealth * healthScale + 0.5f);
        const int scaledSecond = static_cast<int>(dummyDefinition.stats.maxHealth * healthScale + 0.5f);
        const int firstMaxHealth = scaledFirst > 1 ? scaledFirst : 1;
        const int secondMaxHealth = scaledSecond > 1 ? scaledSecond : 1;
        registry.get<Engine::Transform>(characterEntity) = Engine::Transform{};
        registry.replace<Game::FighterState>(characterEntity, Game::FighterState{});
        registry.replace<Game::Health>(characterEntity, Game::Health{ firstMaxHealth, firstMaxHealth });
        registry.get<Engine::Transform>(dummyEntity) = Engine::Transform{ { Game::kDefaultFighterGapX, 0.0f, 0.0f } };
        registry.replace<Game::FighterState>(dummyEntity, Game::FighterState{});
        registry.replace<Game::Health>(dummyEntity, Game::Health{ secondMaxHealth, secondMaxHealth });
        dummyPunishCountdown = 0;
        Game::ClearProjectiles(registry, projectileSystem);
        Game::ClearVfx(vfxSystem);
        lastCollisionEvents = Engine::CollisionEvents{};
        sfxCues.OnRoundReset();
    };
    // AI opponent / simulation state (see AiOpponent.h). dummyAiPreset 4..6 drive the dummy with
    // a brain against the human; "AI simulation" drives BOTH fighters, steered by the director.
    Game::AiBrain playerBrain;
    Game::AiBrain dummyBrain;
    Game::FightDirector fightDirector;
    bool aiSimulation = false;
    uint32_t aiSeed = initialAiSeed;
    // Round announcement and the post-knockout choice (demo flow). While either is up, and from
    // the knockout until the next round, both fighters get neutral input.
    constexpr float kFightIntroSeconds = 1.5f;
    float introSeconds = 0.0f;
    bool continueActive = false;
    Game::ContinueState continueState;
    // Multiplier on the simulation rate (animation included); AI simulation raises it a little
    // by default so the fight reads with more pace. Adjustable in the debug window.
    float gameSpeed = 1.0f;
    // Fast-forward: multiplies the simulation rate on top of gameSpeed (the same pace the
    // headless capture runs at). Limited by the clock's per-frame catch-up cap (8 ticks).
    bool fastForward = false;
    float fastForwardFactor = 4.0f;
    bool aiHotkeyF4Was = false;
    bool sfxHotkeyF5Was = false;
    bool sfxHotkeyF6Was = false;
    bool sfxHotkeyF7Was = false;

    bool aiHotkeyF2Was = false;
    bool aiHotkeyF3Was = false;
    Game::AiFighterView playerView;
    Game::AiFighterView dummyView;
    const auto buildView = [&](entt::entity entity, const Game::MoveTable& table)
    {
        const Game::FighterState& state = registry.get<Game::FighterState>(entity);
        const Game::Health& health = registry.get<Game::Health>(entity);
        Game::AiFighterView view;
        view.x = registry.get<Engine::Transform>(entity).position.x;
        view.facingRight = state.facingRight;
        view.airborne = state.IsAirborne();
        view.stunned = state.currentState == "HitStun" || state.currentState == "KO";
        view.knockedOut = state.currentState == "KO";
        view.attacking = state.currentState == "Attack";
        view.attackInstance = state.attackInstanceId;
        view.framesInMove = state.framesInMove;
        if (view.attacking)
        {
            if (const Game::MoveDefinition* move = table.FindById(state.activeMoveId))
            {
                view.strikeEndFrame = move->startupFrames + move->activeFrames;
            }
        }
        view.health = health.current;
        view.maxHealth = health.max;
        if (view.attacking)
        {
            const bool fireball = state.activeMoveId == "fireball";
            view.castingFireball = fireball;
            const Game::MoveDefinition* move = table.FindById(state.activeMoveId);
            const uint32_t startup = move != nullptr ? move->startupFrames : 0u;
            if (fireball && move != nullptr)
            {
                view.releaseFrame = startup + (move->timingCurveActive ? move->timingSnapFrames : 0u);
            }
            view.exposed = state.framesInMove >= view.strikeEndFrame || (fireball && state.framesInMove > startup);
        }
        // The nearest enemy fireball moving toward this fighter.
        float nearest = 1.0e9f;
        for (const entt::entity projectileEntity : registry.view<Game::Projectile, Engine::Transform>())
        {
            const Game::Projectile& projectile = registry.get<Game::Projectile>(projectileEntity);
            if (projectile.owner == entity)
            {
                continue;
            }
            const float delta = registry.get<Engine::Transform>(projectileEntity).position.x - view.x;
            if (projectile.velocityX * -delta <= 0.0f)
            {
                continue; // Moving away.
            }
            const float distance = delta >= 0.0f ? delta : -delta;
            if (distance < nearest)
            {
                nearest = distance;
                view.incomingProjectile = true;
                view.incomingDistance = distance;
                view.incomingId = static_cast<uint32_t>(entt::to_integral(projectileEntity));
            }
        }
        return view;
    };
    const auto startAiSimulation = [&]()
    {
        aiSimulation = true;
        if (gameSpeed < 1.2f)
        {
            gameSpeed = 1.25f;
        }
        resetRound();
        Game::ResetAiBrain(playerBrain, aiSeed);
        Game::ResetAiBrain(dummyBrain, aiSeed + 7919u);
        fightDirector.Reset(aiSeed, registry.get<Game::Health>(characterEntity).max, registry.get<Game::Health>(dummyEntity).max);
        continueActive = false;
        if (demoMode)
        {
            introSeconds = kFightIntroSeconds;
            sfxCues.OnFight();
        }
    };
    if (prototype.sfx || prototype.music)
    {
        audioReady = Engine::Audio::Init();
        if (audioReady)
        {
            sfxCues.Load();
            if (prototype.music)
            {
                Engine::Audio::PlayMusic("music_loop", prototype.musicVolume);
                currentMusic = "music_loop";
            }
        }
    }
#ifdef OMD_DEV_TOOLS
    if (captureRun && captureScriptName == "ai_sim")
    {
        fightDirector.fightLengthSeconds = 30.0f;
        startAiSimulation();
    }
#endif
    const std::function<void()> hudUI = [&]()
    {
        if (menuActive)
        {
            if (splashActive)
            {
                if (splashTime >= Game::Splash::kDuration)
                {
                    splashActive = false;
                    Game::ResetMainMenu(menuState);
                }
                else
                {
                    splashTime = Game::DrawSplashText(splash, splashTime, GetForegroundWindow() == window);
                    return;
                }
            }
            const bool focused = GetForegroundWindow() == window;
            switch (Game::DrawMainMenu(menuState, hudDeltaSeconds, focused))
            {
            case Game::MenuResult::Play:
                menuActive = false;
                resetRound();
                if (demoMode)
                {
                    startAiSimulation();
                }
                rebuildDrawItems();
                break;
            case Game::MenuResult::Quit:
                PostQuitMessage(0);
                break;
            default:
                break;
            }
            return;
        }
        if (continueActive)
        {
            switch (Game::DrawContinueScreen(continueState, hudDeltaSeconds, GetForegroundWindow() == window))
            {
            case Game::ContinueResult::Rematch:
                ++aiSeed; // A different fight than the last one.
                startAiSimulation();
                break;
            case Game::ContinueResult::Cancel:
                continueActive = false;
                menuActive = true;
                aiSimulation = false;
                introSeconds = 0.0f;
                Game::ResetMainMenu(menuState);
                rebuildDrawItems();
                break;
            default:
                break;
            }
        }
        if (!prototype.hud)
        {
            return;
        }
        Game::HudInput input;
        const Game::Health& first = registry.get<Game::Health>(characterEntity);
        const Game::Health& second = registry.get<Game::Health>(dummyEntity);
        input.health[0] = first.current;
        input.maxHealth[0] = first.max;
        input.health[1] = second.current;
        input.maxHealth[1] = second.max;
        input.name[0] = "PLAYER 1";
        input.name[1] = "PLAYER 2";
        if (prototype.matchFlow && matchFlow.BannerVisible())
        {
            input.bannerVisible = true;
            input.bannerAlpha = matchFlow.BannerAlpha();
            input.bannerSubtitle = matchFlow.WinnerSide() == 0 ? "PLAYER 1 WINS" : "PLAYER 2 WINS";
        }
        if (!input.bannerVisible && introSeconds > 0.0f)
        {
            // "FIGHT!": quick fade in, a short fade out at the end.
            const float elapsed = kFightIntroSeconds - introSeconds;
            input.bannerVisible = true;
            input.bannerTitle = "FIGHT!";
            input.bannerSubtitle = "";
            input.bannerAlpha = std::clamp((std::min)(elapsed / 0.12f, introSeconds / 0.3f), 0.0f, 1.0f);
        }
        Game::DrawHud(hudState, input, hudDeltaSeconds);
    };
#else
    const std::function<void()> hudUI;
#endif

    while (Foundation::PumpMessages())
    {
        const auto now = std::chrono::steady_clock::now();
        float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

#ifdef OMD_DEV_TOOLS
        // A capture run is lockstep: pin the frame delta to one fixed tick, so "tick N" is the
        // same image every run (camera-follow and clip playback both key off this delta).
        if (captureRun)
        {
            deltaSeconds = Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
            captureDebugUIVisible = false;
        }
#endif

#ifdef OMD_DEV_TOOLS
        {
            const bool f9Down = Foundation::IsKeyDown(VK_F9);
            if (f9Down && !f9WasDown)
            {
                Engine::FrameCapture::Grab("manual");
            }
            f9WasDown = f9Down;

            const bool ctrlRDown = Foundation::IsKeyDown(VK_CONTROL) && Foundation::IsKeyDown('R');
            if (ctrlRDown && !ctrlRWasDown)
            {
                captureDebugUIVisible = !captureDebugUIVisible;
            }
            ctrlRWasDown = ctrlRDown;
        }
#endif

        bindingsReloadTimer += deltaSeconds;
        if (bindingsReloadTimer >= 1.0f)
        {
            bindingsReloadTimer = 0.0f;
            std::error_code writeTimeError;
            const std::filesystem::file_time_type currentWriteTime = std::filesystem::last_write_time(kInputBindingsPath, writeTimeError);
            if (!writeTimeError && currentWriteTime != lastBindingsWriteTime)
            {
                lastBindingsWriteTime = currentWriteTime;
                Engine::InputBindings reloaded;
                if (Game::LoadFighterBindingsFromFile(kInputBindingsPath, reloaded))
                {
                    fighterBindings = reloaded;
                    Foundation::Log::Write(Foundation::Log::Severity::Info, "Game", "Reloaded input bindings from %s", kInputBindingsPath);
                }
                // On failure, LoadFighterBindingsFromFile has already logged why and left
                // fighterBindings untouched - still using whatever last loaded successfully,
                // not silently falling back to the built-in default mid-session.
            }
        }

#ifdef OMD_DEV_TOOLS
        // io.WantCaptureMouse reflects the previous frame's ImGui state (this frame's NewFrame()
        // hasn't run yet - see RenderTasks::DoFrame) - a harmless one-frame lag, same as any
        // other engine using this exact pattern to stop UI clicks/drags from also driving camera
        // controls underneath the window.
        const bool allowCameraMouse = enableCameraMouseControl && !ImGui::GetIO().WantCaptureMouse;
        // Same pattern, keyboard half - see enableCameraKeyboardControl's own declaration.
        const bool allowCameraKeyboard = enableCameraKeyboardControl && !ImGui::GetIO().WantCaptureKeyboard;
#else
        const bool allowCameraMouse = true;
        const bool allowCameraKeyboard = true;
#endif
        // Not in the menu or the demo flow: the mouse buttons and the gamepad sticks (used there
        // for choosing) would otherwise also fly the camera.
        if (!menuActive && !demoMode)
        {
            Engine::UpdateFreeFlyCamera(camera, window, deltaSeconds, allowCameraMouse, allowCameraKeyboard);
        }
        if (enableCameraFollow)
        {
            // A simple "invisible point at the midpoint of both fighters" follow - only X eases
            // toward it each frame (this is a 2D-camera game; Z is purely the stage's visual
            // depth - see GameConstants.h - and Jump's own Y arc is modest enough not to need
            // vertical follow too). Height/depth/yaw/pitch are left untouched, so this coexists
            // with manual free-fly adjustments (zoom/angle) instead of fighting them - only X
            // gets overridden while this toggle is on.
            const float playerX = registry.get<Engine::Transform>(characterEntity).position.x;
            const float dummyX = registry.get<Engine::Transform>(dummyEntity).position.x;
            const float midpointX = (playerX + dummyX) * 0.5f;
            // Eyeball-tuned starting point (see FighterState.cpp's own precedent for this
            // phrasing) - higher catches up to the midpoint faster. Not true frame-rate-
            // independent exponential decay (would need std::exp, not worth pulling in <cmath>
            // for), just a per-frame fraction of the remaining distance, clamped so a long
            // frame hitch can't overshoot past the target. std::clamp, not std::min - Windows.h's
            // own min/max macros (NOMINMAX isn't defined in this project) break a bare std::min.
            constexpr float kCameraFollowSpeed = 5.0f;
            const float followT = std::clamp(kCameraFollowSpeed * deltaSeconds, 0.0f, 1.0f);
            camera.position.x += (midpointX - camera.position.x) * followT;
        }
        const float aspectRatio = static_cast<float>(Renderer::Device::GetWidth()) / static_cast<float>(Renderer::Device::GetHeight());
#if defined(OMD_PROTOTYPE)
        // Presentation only: the shake displaces a copy of the camera, never the camera itself.
        Engine::Camera shakenCamera = camera;
        if (prototype.juice)
        {
            juice.Update(deltaSeconds);
            const DirectX::XMFLOAT2 shake = juice.ShakeOffset();
            shakenCamera.position.x += shake.x;
            shakenCamera.position.y += shake.y;
        }
        else
        {
            juice = Game::JuiceState{};
        }
        for (int side = 0; side < 2; ++side)
        {
            const DirectX::XMFLOAT4 tint = juice.TintFor(side, fighterBaseTint[side]);
            Renderer::Buffer::Update(fighterTintBuffer[side], &tint, sizeof(tint));
        }
        if (splashActive)
        {
            splashTime += deltaSeconds;
        }
        if (menuActive)
        {
            // A fixed, slowly drifting camera close to the skull pile.
            const DirectX::XMFLOAT3 focus = Game::BackdropMenuFocus(backdrop);
            shakenCamera = Engine::Camera{};
            shakenCamera.position = { focus.x + 0.3f * std::sin(menuState.time * 0.23f), focus.y + 0.08f * std::sin(menuState.time * 0.17f),
                                      focus.z - 3.0f };
        }
        Game::UpdateSplash(splash, shakenCamera, aspectRatio, splashTime, menuActive && splashActive);
        const DirectX::XMFLOAT4X4 viewProjection = Engine::ComputeViewProjection(shakenCamera, aspectRatio);
#else
        const DirectX::XMFLOAT4X4 viewProjection = Engine::ComputeViewProjection(camera, aspectRatio);
#endif

#if defined(OMD_PROTOTYPE)
        if (audioReady)
        {
            // Which track belongs to the screen: none over the two opening text pages of the demo
            // launch, the slow one behind the menu, the fight track otherwise. A newly started
            // track fades in.
            const char* wantedMusic = (menuActive && splashActive) ? "" : (menuActive ? "music_menu" : "music_loop");
            if (!prototype.music)
            {
                wantedMusic = "";
            }
            if (currentMusic != wantedMusic)
            {
                currentMusic = wantedMusic;
                musicFade = 0.0f;
                if (wantedMusic[0] == '\0')
                {
                    Engine::Audio::StopMusic();
                }
                else
                {
                    Engine::Audio::PlayMusic(wantedMusic, 0.0f);
                }
            }
            musicFade = (std::min)(1.0f, musicFade + deltaSeconds / 1.5f);
            Engine::Audio::SetMusicVolume(currentMusic.empty() ? 0.0f : prototype.musicVolume * musicFade);
        }
        sfxCues.SetScale(prototype.sfxVolume);
        sfxCues.Tick(deltaSeconds);
        float simDeltaSeconds = deltaSeconds * gameSpeed * (fastForward ? fastForwardFactor : 1.0f);
        hudDeltaSeconds = deltaSeconds;
        introSeconds = (std::max)(0.0f, introSeconds - deltaSeconds);
        if (prototype.matchFlow)
        {
            int loserSide = -1;
            if (registry.get<Game::FighterState>(characterEntity).currentState == "KO")
            {
                loserSide = 0;
            }
            else if (registry.get<Game::FighterState>(dummyEntity).currentState == "KO")
            {
                loserSide = 1;
            }
            if (continueActive)
            {
                // Waiting on the choice; the knocked-out fighter stays down.
            }
            else if (matchFlow.Update(deltaSeconds, loserSide))
            {
                if (demoMode && aiSimulation)
                {
                    continueActive = true;
                    continueState = Game::ContinueState{};
                }
                else
                {
                    resetRound();
                }
            }
            simDeltaSeconds *= matchFlow.TimeScale();
        }
        else
        {
            matchFlow.Reset();
        }
        if (menuActive)
        {
            simDeltaSeconds = 0.0f; // The fight waits behind the menu.
        }
        {
            // F2 toggles AI simulation, F3 restarts it with the next seed - usable with the
            // debug windows hidden.
            const bool f2Down = Foundation::IsKeyDown(VK_F2);
            if (f2Down && !aiHotkeyF2Was)
            {
                if (aiSimulation)
                {
                    aiSimulation = false;
                }
                else
                {
                    startAiSimulation();
                }
            }
            aiHotkeyF2Was = f2Down;
            const bool f3Down = Foundation::IsKeyDown(VK_F3);
            if (f3Down && !aiHotkeyF3Was)
            {
                ++aiSeed;
                startAiSimulation();
            }
            aiHotkeyF3Was = f3Down;
            const bool f4Down = Foundation::IsKeyDown(VK_F4);
            if (f4Down && !aiHotkeyF4Was)
            {
                fastForward = !fastForward;
            }
            aiHotkeyF4Was = f4Down;
            // F5: next fixed SFX variant (with random off). F6: random picks on/off.
            const bool f5Down = Foundation::IsKeyDown(VK_F5);
            if (f5Down && !sfxHotkeyF5Was)
            {
                sfxCues.CycleVariant();
                Foundation::Log::Write(Foundation::Log::Severity::Info, "Sfx", "fixed variant -> %d", sfxCues.FixedVariant() + 1);
            }
            sfxHotkeyF5Was = f5Down;
            const bool f6Down = Foundation::IsKeyDown(VK_F6);
            if (f6Down && !sfxHotkeyF6Was)
            {
                sfxCues.SetRandomized(!sfxCues.Randomized());
                Foundation::Log::Write(Foundation::Log::Severity::Info, "Sfx", "random picks %s", sfxCues.Randomized() ? "ON" : "OFF");
            }
            sfxHotkeyF6Was = f6Down;
            // F7: sparse / full SFX density.
            const bool f7Down = Foundation::IsKeyDown(VK_F7);
            if (f7Down && !sfxHotkeyF7Was)
            {
                sfxCues.SetSparse(!sfxCues.Sparse());
                Foundation::Log::Write(Foundation::Log::Severity::Info, "Sfx", "sparse SFX %s", sfxCues.Sparse() ? "ON" : "OFF");
            }
            sfxHotkeyF7Was = f7Down;
        }
        if (restartDemoRequested && !scriptedRun && backdrop.loaded)
        {
            // The --demo launch, without restarting the process: boot pages, menu, then Play
            // runs the AI simulation with the debug UI hidden.
            restartDemoRequested = false;
            demoMode = true;
            menuAllowed = true;
            menuActive = true;
            splashActive = true;
            splashTime = 0.0f;
            splash.skipWas = true;
            camera = Engine::Camera{};
            aiSimulation = false;
            continueActive = false;
            introSeconds = 0.0f;
            Game::ResetMainMenu(menuState);
#ifdef OMD_DEV_TOOLS
            captureDebugUIVisible = false;
#endif
            resetRound();
            rebuildDrawItems();
        }
        restartDemoRequested = false;
        if (menuAllowed)
        {
            const bool escDown = GetForegroundWindow() == window && Foundation::IsKeyDown(VK_ESCAPE);
            if (!menuActive && escDown && !menuEscWas)
            {
                menuActive = true;
                aiSimulation = false;
                continueActive = false;
                introSeconds = 0.0f;
                Game::ResetMainMenu(menuState);
                rebuildDrawItems();
            }
            menuEscWas = escDown;
        }
        floorScrollSeconds += deltaSeconds;
        Game::UpdateStageScroll(stageDrawItems, floorScrollSeconds);
        simClock.BeginFrame(simDeltaSeconds);
#else
        simClock.BeginFrame(deltaSeconds);
#endif
        while (simClock.TryConsumeTick())
        {
            // Facing - snap (not interpolated), computed once per tick from last tick's settled
            // positions, before either fighter's state machine runs below, so both the render
            // (updateFighterRender further down) and the bone-attached hitbox lookup
            // (FighterState.cpp's ResolveHitboxOffset) read this same tick's value. Whichever
            // fighter is more to the left faces right (toward the other) and vice versa - always
            // facing the opponent regardless of which way either is currently walking/attacking,
            // so crossups/jumping past an opponent don't leave anyone facing backward.
            //
            // One side decision per tick, for the pair: the player's facing is the persistent
            // "am I the left one", updated only when the two are clearly ordered (see
            // ResolveIsLeftOf - level fighters, e.g. after a jump-over clamped at a wall, keep
            // their sides), and the dummy is simply its opposite. Deriving both from the same bool
            // is what makes "both facing the same way" or "facing away" unrepresentable, with no
            // special case near the walls.
            {
                const float playerX = registry.get<Engine::Transform>(characterEntity).position.x;
                const float dummyX = registry.get<Engine::Transform>(dummyEntity).position.x;
                Game::FighterState& playerFacingState = registry.get<Game::FighterState>(characterEntity);
                const bool playerOnLeft = Game::ResolveIsLeftOf(playerFacingState.facingRight, playerX, dummyX);
                playerFacingState.facingRight = playerOnLeft;
                registry.get<Game::FighterState>(dummyEntity).facingRight = !playerOnLeft;
            }

            // See disableCharacterAnalogStickMovement's own declaration - unconditionally false
            // outside dev tools, so this always compiles to the existing behavior there.
#ifdef OMD_DEV_TOOLS
            const bool ignoreGamepadAnalogAxis = disableCharacterAnalogStickMovement;
#else
            const bool ignoreGamepadAnalogAxis = false;
#endif
#if defined(OMD_PROTOTYPE)
            const Engine::InputCommand previousPlayerCommand = lastInputCommand;
            struct HitSnapshot
            {
                entt::entity lastHitBy;
                uint32_t lastHitInstance;
                int health;
                uint32_t attackInstance;
                bool airborne;
                bool attacking;
            };
            const auto snapshotFor = [&](entt::entity entity)
            {
                const Game::FighterState& state = registry.get<Game::FighterState>(entity);
                return HitSnapshot{ state.lastHitByAttacker, state.lastHitByAttackerInstanceId, registry.get<Game::Health>(entity).current,
                                    state.attackInstanceId, state.IsAirborne(), state.currentState == "Attack" };
            };
            const HitSnapshot beforeFighters[2] = { snapshotFor(characterEntity), snapshotFor(dummyEntity) };
#endif
#ifdef OMD_DEV_TOOLS
            if (captureRun)
            {
                // Scripted player input for this tick - bypasses the real device path entirely.
                lastInputCommand = captureRun->InputForTick(simClock.tickCount);
                lastInputCommand.tick = simClock.tickCount;
            }
            else
#endif
            {
                lastInputCommand = Engine::AssembleInputCommand(
                    simClock.tickCount, fighterBindings, lastInputCommand, /*gamepadIndex*/ 0, ignoreGamepadAnalogAxis);
            }
#if defined(OMD_PROTOTYPE)
            playerView = buildView(characterEntity, characterDefinition.moveTable);
            dummyView = buildView(dummyEntity, dummyDefinition.moveTable);
            if (aiSimulation)
            {
                const bool activity = playerView.attacking || dummyView.attacking || playerView.stunned || dummyView.stunned;
                fightDirector.Update(
                    Engine::FixedTimestepAccumulator::kFixedDeltaSeconds, playerView.health, dummyView.health, activity);
                const Game::AiIntent intent =
                    Game::TickAiBrain(playerBrain, playerView, dummyView, fightDirector.role[0], fightDirector.tuning[0]);
                Game::ApplyAiIntent(intent, simClock.tickCount, previousPlayerCommand, lastInputCommand);
            }
#endif
            const bool fightersHeld = introSeconds > 0.0f || continueActive || matchFlow.BannerVisible();
            if (fightersHeld)
            {
                const uint32_t heldTick = lastInputCommand.tick;
                lastInputCommand = Engine::InputCommand{};
                lastInputCommand.tick = heldTick;
            }
            playerInputHistory.Push(lastInputCommand);

            // Debug readout only for now (Phase 3) - only overwritten on an actual selection
            // (not cleared back to empty every tick that isn't one) so it shows "the last real
            // selection" long enough to see, rather than a value only ever true for one ~16ms
            // tick. UpdateFighterState below calls SelectMove again itself once move selection
            // actually becomes load-bearing (a later phase of this step) - harmless duplicate
            // work at this entity count, not worth threading the result through as a parameter.
            if (const std::optional<std::string> selected = Game::SelectMove(
                    characterDefinition, playerInputHistory, registry.get<Game::FighterState>(characterEntity).IsAirborne());
                selected.has_value())
            {
                lastSelectedMove = selected;
            }

            // Reacts to LAST tick's collision events (still held in lastCollisionEvents from the
            // previous iteration at this point) and decides this tick's Transform/ClipPlayback
            // (and, from a later phase of this step, Hitbox attach/detach) - must run before
            // ResolveCollisions below so this tick's freshly-updated boxes are what gets tested,
            // not stale ones. A hit is therefore detected and applied one tick after a hitbox
            // actually became active - imperceptible at 60Hz, the same kind of deliberate lag the
            // debug-slider-to-collision-color path already has.
            // Skipped entirely while manualClipOverride or hitbox tuning (see below) is on, so
            // the state machine can't fight either debug override - it would otherwise overwrite
            // ClipPlayback/Hitbox right back to whatever Idle/Walk/etc. currently means every
            // tick, which is exactly the "dropdown doesn't do anything" symptom these overrides
            // exist to fix.
            const bool hitboxTuningActive =
                hitboxTuningMoveIndex >= 0 && hitboxTuningMoveIndex < static_cast<int>(characterDefinition.moveTable.moves.size());
            if (hitboxTuningActive)
            {
                // See hitboxTuningMoveIndex's own declaration - drives clip+hitbox from a
                // continuously-looping frame counter instead of real input/collision, purely so
                // the selected move's hitbox can be watched (and tuned) against its own animated
                // pose. Loops back to frame 0 once the move's own authored duration elapses,
                // matching how a real Attack's stateDurationFrames is computed (EnterState) -
                // startup, active, and recovery all replay continuously rather than freezing at
                // the end or racing off into fmodf's clip-only looping alone (which would desync
                // the hitbox's own frameStart/frameEnd window from a clip that kept looping past
                // it).
                const Game::MoveDefinition& previewMove = characterDefinition.moveTable.moves[static_cast<size_t>(hitboxTuningMoveIndex)];
                Game::PreviewMoveHitbox(
                    registry, characterEntity, characterDefinition, previewMove.id, hitboxTuningFrameCounter, characterModel.clips,
                    characterModel, forceShowAllHitboxes);
                // Paused (see hitboxTuningPaused's own declaration) freezes the counter exactly
                // where the "Frame" slider left it - the render above still uses whatever value
                // that is, so the pose/hitbox visibly hold still at that keyframe.
                if (!hitboxTuningPaused)
                {
                    const uint32_t previewTotalFrames = previewMove.startupFrames + previewMove.activeFrames + previewMove.recoveryFrames;
                    ++hitboxTuningFrameCounter;
                    if (previewTotalFrames > 0 && hitboxTuningFrameCounter >= previewTotalFrames)
                    {
                        hitboxTuningFrameCounter = 0;
                    }
                }
            }
            else if (!manualClipOverride)
            {
                Game::UpdateFighterState(
                    registry, characterEntity,
                    Game::FighterUpdateInput{
                        lastInputCommand, playerInputHistory, lastCollisionEvents, sharedStates, characterDefinition,
                        characterModel.clips, characterModel, forceShowAllHitboxes });

                // Combo debug: log + record every time the player receives a NEW Attack move.
                // attackInstanceId bumps exactly once per EnterState-with-a-move-id call (fresh
                // SelectMove-from-Idle or a same-state cancel alike - see FighterState.h's own
                // comment) and, unlike framesInMove, stays put across a hitstop freeze - so
                // comparing it against the last value logged fires exactly once per entry even
                // when the fighter is frozen at framesInMove==1 for a couple of ticks right after
                // (the previous hit's own reaction arriving one tick late, same lag every other
                // reader of lastCollisionEvents already has). Telling a cancel apart from a fresh
                // entry only needs whether the PREVIOUS tick was also "Attack" - a cancel never
                // passes through Idle in between, a fresh entry always does.
                const Game::FighterState& playerCombo = registry.get<Game::FighterState>(characterEntity);
                if (playerCombo.currentState == "Attack" && playerCombo.attackInstanceId != comboLastLoggedInstanceId)
                {
                    comboLastLoggedInstanceId = playerCombo.attackInstanceId;
                    const bool viaCancel = comboPrevState == "Attack";
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Info, "Combo", "t%u %s %s", simClock.tickCount, viaCancel ? "cancel ->" : "enter",
                        playerCombo.activeMoveId.c_str());
                    comboHistory.push_back({ simClock.tickCount, viaCancel, playerCombo.activeMoveId });
                    if (comboHistory.size() > 8)
                    {
                        comboHistory.erase(comboHistory.begin());
                    }
                }
                comboPrevState = playerCombo.currentState;
            }

            // AI preset decision - see dummyAiPreset's own declaration for why this is a testing
            // convenience (a few hardcoded scripted reactions), not real AI. lastCollisionEvents
            // is still last tick's result at this point (ResolveCollisions runs after both
            // fighters' UpdateFighterState calls below) - the same one-tick-lag every other
            // reader of it in this loop already accepts.
            bool dummyBlockThisTick = false;
            bool dummyPunchThisTick = false;
            bool dummySpecialThisTick = false;
            if (dummyAiPreset == 0) // Manual
            {
                dummyBlockThisTick = dummyBlocksHeld;
            }
            else if (dummyAiPreset == 1) // Always block
            {
                dummyBlockThisTick = true;
            }
            else if (dummyAiPreset == 3) // Fireball
            {
                constexpr int kDummyFireballPeriodTicks = 150;
                dummySpecialThisTick = (++dummyFireballTimer % kDummyFireballPeriodTicks) == 0;
            }
            else if (dummyAiPreset == 2) // Block then punish
            {
                dummyBlockThisTick = true;
                for (const Engine::HitEvent& hit : lastCollisionEvents.hits)
                {
                    if (hit.defender == dummyEntity)
                    {
                        constexpr int kDummyPunishDelayTicks = 40; // ~0.67s - a rough "wait for their recovery" heuristic, not frame-exact.
                        dummyPunishCountdown = kDummyPunishDelayTicks;
                        break;
                    }
                }
                if (dummyPunishCountdown > 0)
                {
                    --dummyPunishCountdown;
                    if (dummyPunishCountdown == 0)
                    {
                        dummyPunchThisTick = true;
                        dummyBlockThisTick = false; // Swinging, not blocking, on the punish tick itself.
                    }
                }
            }

            // The dummy has neither a manual-override nor a hitbox-tuning mode, so it always
            // runs through the real state machine, unconditionally, every tick - reacting to
            // last tick's collision events (from real hurtbox overlap with the player's own
            // Attack hitboxes) exactly like the player's own call above.
#if defined(OMD_PROTOTYPE)
            const Engine::InputCommand previousDummyCommand = dummyInputCommand;
#endif
            dummyInputCommand.tick = simClock.tickCount;
            {
                Engine::ButtonState& block = dummyInputCommand.buttons[static_cast<size_t>(Game::FighterButton::Block)];
                block.held = dummyBlockThisTick;
                // This synthetic command is built by hand rather than through
                // Engine::AssembleInputCommand, so pressed/released edges (SelectMove only
                // reads pressedThisTick, never held) have to be derived here the same way that
                // function derives them - from the previous tick's held state.
                Engine::ButtonState& punch = dummyInputCommand.buttons[static_cast<size_t>(Game::FighterButton::Punch)];
                const bool punchWasHeld = punch.held;
                punch.held = dummyPunchThisTick;
                punch.pressedThisTick = dummyPunchThisTick && !punchWasHeld;
                punch.releasedThisTick = !dummyPunchThisTick && punchWasHeld;

                Engine::ButtonState& special = dummyInputCommand.buttons[static_cast<size_t>(Game::FighterButton::Special)];
                const bool specialWasHeld = special.held;
                special.held = dummySpecialThisTick;
                special.pressedThisTick = dummySpecialThisTick && !specialWasHeld;
                special.releasedThisTick = !dummySpecialThisTick && specialWasHeld;
            }
#if defined(OMD_PROTOTYPE)
            if (aiSimulation || dummyAiPreset >= 4)
            {
                const Game::AiRole dummyRole = aiSimulation ? fightDirector.role[1] : Game::AiRole::Neutral;
                const Game::AiTuning dummyTuning =
                    aiSimulation ? fightDirector.tuning[1]
                                 : Game::TuningForProfile(static_cast<Game::AiProfile>(std::clamp(dummyAiPreset - 4, 0, 2)));
                const Game::AiIntent intent = Game::TickAiBrain(dummyBrain, dummyView, playerView, dummyRole, dummyTuning);
                Game::ApplyAiIntent(intent, simClock.tickCount, previousDummyCommand, dummyInputCommand);
            }
#endif
            if (fightersHeld)
            {
                const uint32_t heldTick = dummyInputCommand.tick;
                dummyInputCommand = Engine::InputCommand{};
                dummyInputCommand.tick = heldTick;
            }
            dummyInputHistory.Push(dummyInputCommand);
            Game::UpdateFighterState(
                registry, dummyEntity,
                Game::FighterUpdateInput{
                    dummyInputCommand, dummyInputHistory, lastCollisionEvents, sharedStates, dummyDefinition, characterModel.clips,
                    characterModel, /*forceShowAllHitboxes*/ false });

            // Undying dummy: once the KO drop has had time to actually read on screen (past its
            // own settle window, plus a short hold so the pose is clearly visible, not skipped
            // the instant health hits 0), snap straight back to full health and Idle - bypassing
            // states.combat entirely, the same way the Reset button already does, since KO
            // itself has no authored way out. Re-checked every tick while still KO'd rather than
            // latched once, which is fine - reset is idempotent and this naturally stops firing
            // the moment currentState leaves "KO". Verified via a scripted repeated-punch capture
            // that this fires exactly once per knockout and resumes normal hit processing right
            // after (health ticking back down from 100 on the next landed hit, not stuck).
            if (dummyUndying
#if defined(OMD_PROTOTYPE)
                && !prototype.matchFlow
#endif
            )
            {
                Game::FighterState& dummyState = registry.get<Game::FighterState>(dummyEntity);
                constexpr uint32_t kUndyingReviveHoldFrames = 30; // ~0.5s past the drop settling - a beat to read the KO, eyeball-tuned.
                if (dummyState.currentState == "KO" &&
                    dummyState.framesInState >= dummyDefinition.koDropEndFrame + kUndyingReviveHoldFrames)
                {
                    dummyState = Game::FighterState{};
                    sfxCues.OnRoundReset();
                    registry.replace<Game::Health>(
                        dummyEntity, Game::Health{ dummyDefinition.stats.maxHealth, dummyDefinition.stats.maxHealth });
                }
            }

            // Fighter-vs-fighter body separation - keeps the two from walking through each
            // other. A simple 1D check (this is a 2D-camera game; only X is a real gameplay
            // axis - see GameConstants.h), not a real physics solve: if they're closer together
            // than both bodies' combined half-width, each is pushed half the overlap back out,
            // symmetric regardless of which one moved into the other. Deliberately separate
            // from Hurtbox/Hitbox (offense/defense volumes, not solid colliders) - see
            // kFighterBodyHalfWidth's own comment. Runs after both fighters' UpdateFighterState
            // calls (using this tick's just-settled positions) and before ResolveCollisions, so
            // hit resolution tests the already-separated positions, not a still-overlapping one.
            {
                Engine::Transform& playerTransform = registry.get<Engine::Transform>(characterEntity);
                Engine::Transform& dummyTransform = registry.get<Engine::Transform>(dummyEntity);
                // Skip separation entirely while either fighter is off the ground - a jump then
                // carries a fighter over/past the opponent (crossup), the standard fighting-game
                // behavior, instead of an airborne fighter shoving the grounded one around. A
                // grounded fighter's position.y is exactly 0 (only the KO drop goes negative);
                // Jump drives it positive. Once both land overlapping, this runs again next tick
                // and separates them onto opposite sides, which is the intended result.
                const bool eitherAirborne = playerTransform.position.y > 0.01f || dummyTransform.position.y > 0.01f;
                const float minSeparation = Game::kFighterBodyHalfWidth * 2.0f;
                // Sides come from the same rule facing uses (the player's stored facing is last
                // tick's "am I the left one"), not from the raw sign of the X difference - two
                // fighters at exactly the same X (a jump-over clamped at a wall) would otherwise
                // resolve to whichever side the tie-break happened to pick, not the side they were
                // actually on. Pushing along the side ordering also means the push can never carry
                // one fighter past the other, so it cannot flip facing itself.
                const bool playerOnLeft = Game::ResolveIsLeftOf(
                    registry.get<Game::FighterState>(characterEntity).facingRight, playerTransform.position.x,
                    dummyTransform.position.x);
                Engine::Transform& leftTransform = playerOnLeft ? playerTransform : dummyTransform;
                Engine::Transform& rightTransform = playerOnLeft ? dummyTransform : playerTransform;
                const float overlap = minSeparation - (rightTransform.position.x - leftTransform.position.x);
                if (!eitherAirborne && overlap > 0.0f)
                {
                    // Half the overlap each - but a fighter already hugging the wall can't
                    // give ground, so whatever it could not absorb is handed to the other one,
                    // rather than leaving a residual overlap that would take several more ticks
                    // to work out (and keep the pair nearly level in the meantime).
                    constexpr float kWall = Game::kStageFighterLimit;
                    leftTransform.position.x -= overlap * 0.5f;
                    rightTransform.position.x += overlap * 0.5f;
                    if (leftTransform.position.x < -kWall)
                    {
                        rightTransform.position.x += -kWall - leftTransform.position.x;
                        leftTransform.position.x = -kWall;
                    }
                    if (rightTransform.position.x > kWall)
                    {
                        leftTransform.position.x -= rightTransform.position.x - kWall;
                        rightTransform.position.x = kWall;
                    }
                    leftTransform.position.x = std::clamp(leftTransform.position.x, -kWall, kWall);
                }
            }

            // Deterministic, fixed-tick per the networking-readiness design - resolved here, not
            // once per render frame, even though nothing yet moves entities per tick (the debug
            // sliders below move them at render rate instead, so overlap color can lag a tick
            // behind a dragged slider - intentional, an honest side effect of ticks/frames being
            // decoupled, not a bug).
#if defined(OMD_PROTOTYPE)
            if (prototype.vfx)
            {
                // Dust as a fighter leaves / returns to the ground.
                const entt::entity vfxFighters[2] = { characterEntity, dummyEntity };
                for (int side = 0; side < 2; ++side)
                {
                    const Game::FighterState& state = registry.get<Game::FighterState>(vfxFighters[side]);
                    const bool airborne = state.IsAirborne();
                    // A small splash for each foot-fall while running.
                    if (state.currentState == "Run")
                    {
                        if (++vfxStepTicks[side] >= 12)
                        {
                            vfxStepTicks[side] = 0;
                            Game::VfxEvent step;
                            step.type = Game::VfxType::Step;
                            const DirectX::XMFLOAT3 foot = registry.get<Engine::Transform>(vfxFighters[side]).position;
                            step.position = { foot.x, Game::kFloorSurfaceY + 0.03f, foot.z };
                            step.direction = { state.facingRight ? 1.0f : -1.0f, 0.0f, 0.0f };
                            Game::PushVfxEvent(vfxSystem, step);
                        }
                    }
                    else
                    {
                        vfxStepTicks[side] = 6;
                    }
                    if (airborne == beforeFighters[side].airborne)
                    {
                        continue;
                    }
                    Game::VfxEvent event;
                    event.type = airborne ? Game::VfxType::JumpOff : Game::VfxType::Landing;
                    const DirectX::XMFLOAT3 feet = registry.get<Engine::Transform>(vfxFighters[side]).position;
                    event.position = { feet.x, Game::kFloorSurfaceY + 0.03f, feet.z };
                    Game::PushVfxEvent(vfxSystem, event);
                }
                if (projectileSystem.cancelledPairs != vfxSeenCancelled)
                {
                    vfxSeenCancelled = projectileSystem.cancelledPairs;
                    Game::VfxEvent event;
                    event.type = Game::VfxType::FireballClash;
                    event.position = projectileSystem.lastClashPosition;
                    Game::PushVfxEvent(vfxSystem, event);
                }
            }
            if (prototype.juice || prototype.vfx || (prototype.sfx && audioReady))
            {
                const entt::entity fighters[2] = { characterEntity, dummyEntity };
                for (int side = 0; side < 2; ++side)
                {
                    const Game::FighterState& state = registry.get<Game::FighterState>(fighters[side]);
                    if (prototype.sfx && audioReady)
                    {
                        if (state.currentState == "Attack" && state.attackInstanceId != beforeFighters[side].attackInstance)
                        {
                            // Attacking at the start of the tick = cancelled into: a link in a string.
                            sfxCues.OnAttackStarted(state.activeMoveId, beforeFighters[side].attacking);
                        }
                        // An attack that has not connected a few ticks into its strike whiffs.
                        if (state.currentState == "Attack" && state.attackInstanceId != whiffHandledInstance[side])
                        {
                            const Game::MoveTable& moveTable = side == 0 ? characterDefinition.moveTable : dummyDefinition.moveTable;
                            const Game::MoveDefinition* move = moveTable.FindById(state.activeMoveId);
                            if (move != nullptr && !move->hitboxes.empty() && state.framesInMove >= move->startupFrames + 3u)
                            {
                                whiffHandledInstance[side] = state.attackInstanceId;
                                if (!state.moveHitConfirmed)
                                {
                                    sfxCues.OnWhiff();
                                }
                            }
                        }
                        // The footstep loop follows the Run state.
                        const bool running = state.currentState == "Run";
                        if (running && runLoop[side] < 0)
                        {
                            sfxCues.OnRunStart(static_cast<float>(simClock.tickCount) / 60.0f);
                            runLoop[side] = sfxCues.StartRun();
                        }
                        else if (!running && runLoop[side] >= 0)
                        {
                            sfxCues.StopRun(runLoop[side]);
                            runLoop[side] = -1;
                        }
                        const bool airborne = state.IsAirborne();
                        if (airborne && !beforeFighters[side].airborne && state.currentState == "Jump")
                        {
                            sfxCues.OnJump();
                        }
                        else if (!airborne && beforeFighters[side].airborne)
                        {
                            sfxCues.OnLand();
                        }
                    }
                }
                if (prototype.sfx && audioReady)
                {
                    bool fireballLive = false;
                    for (const entt::entity projectileEntity : registry.view<Game::Projectile>())
                    {
                        (void)projectileEntity;
                        fireballLive = true;
                        break;
                    }
                    sfxCues.SetFireballInFlight(fireballLive);
                }
                if (prototype.sfx && audioReady && projectileSystem.cancelledPairs != sfxSeenCancelled)
                {
                    sfxSeenCancelled = projectileSystem.cancelledPairs;
                    sfxCues.OnFireballCancelled();
                }
                // A hit registered this tick: the fighter's last-hit bookkeeping changed.
                for (int side = 0; side < 2; ++side)
                {
                    const Game::FighterState& state = registry.get<Game::FighterState>(fighters[side]);
                    if (state.lastHitByAttacker == beforeFighters[side].lastHitBy &&
                        state.lastHitByAttackerInstanceId == beforeFighters[side].lastHitInstance)
                    {
                        continue;
                    }
                    const int health = registry.get<Game::Health>(fighters[side]).current;
                    const bool knockout = health <= 0;
                    const bool blocked = state.hitReactionClip == "block_hit";
                    const bool byFireball =
                        state.lastHitByAttacker != entt::null && registry.valid(state.lastHitByAttacker) &&
                        registry.all_of<Game::Projectile>(state.lastHitByAttacker);
                    if (prototype.sfx && audioReady)
                    {
                        sfxCues.OnHit(beforeFighters[side].health - health, blocked, byFireball, knockout);
                    }
                    if (prototype.vfx)
                    {
                        Game::VfxEvent event;
                        event.type = byFireball ? Game::VfxType::FireballImpact
                                                : (knockout ? Game::VfxType::Knockout
                                                            : (blocked ? Game::VfxType::Block : Game::VfxType::Hit));
                        const entt::entity attacker = state.lastHitByAttacker;
                        event.position = Game::ComputeContactPoint(registry, attacker, fighters[side]);
                        const bool attackerKnown = attacker != entt::null && registry.valid(attacker) && registry.all_of<Engine::Transform>(attacker);
                        const float attackerX = attackerKnown ? registry.get<Engine::Transform>(attacker).position.x
                                                              : registry.get<Engine::Transform>(fighters[1 - side]).position.x;
                        event.direction = { registry.get<Engine::Transform>(fighters[side]).position.x >= attackerX ? 1.0f : -1.0f, 0.0f, 0.0f };
                        event.intensity = std::clamp(static_cast<float>(beforeFighters[side].health - health) / 25.0f, 0.15f, 1.0f);
                        Game::PushVfxEvent(vfxSystem, event);
                        if (knockout)
                        {
                            Game::VfxEvent pool = event;
                            pool.type = Game::VfxType::BloodPool;
                            pool.position = registry.get<Engine::Transform>(fighters[side]).position;
                            pool.subject = fighters[side];
                            pool.landedFrame = (side == 0 ? characterDefinition : dummyDefinition).koDropEndFrame;
                            Game::PushVfxEvent(vfxSystem, pool);
                        }
                    }
                    if (!prototype.juice)
                    {
                        continue;
                    }
                    juice.OnHit(side, beforeFighters[side].health - health, blocked, knockout);
                    if (knockout)
                    {
                        // A longer freeze on the finishing blow.
                        for (const entt::entity entity : fighters)
                        {
                            Game::FighterState& frozen = registry.get<Game::FighterState>(entity);
                            if (frozen.hitStopRemaining < 10u)
                            {
                                frozen.hitStopRemaining = 10u;
                            }
                        }
                    }
                }
            }
            Game::UpdateProjectiles(registry, projectileSystem, lastCollisionEvents);
#endif
            lastCollisionEvents = Engine::ResolveCollisions(registry);

#ifdef OMD_DEV_TOOLS
            if (captureRun)
            {
                // Where each fighter stands and which way it faces, once per tick - lets a
                // capture that moves the fighters around (jump-overs, wall scripts) be checked
                // for "always facing each other" and for a sensible landing without eyeballing
                // frames.
                const Engine::Transform& sidePlayer = registry.get<Engine::Transform>(characterEntity);
                const Engine::Transform& sideDummy = registry.get<Engine::Transform>(dummyEntity);
                const Game::FighterState& sidePlayerState = registry.get<Game::FighterState>(characterEntity);
                const Game::FighterState& sideDummyState = registry.get<Game::FighterState>(dummyEntity);
                Foundation::Log::Write(
                    Foundation::Log::Severity::Info, "Side",
                    "t%u player x=%.3f y=%.2f faceR=%d %s %s php=%d | dummy x=%.3f faceR=%d %s hp=%d", simClock.tickCount, sidePlayer.position.x,
                    sidePlayer.position.y, sidePlayerState.facingRight ? 1 : 0, sidePlayerState.currentState.c_str(),
                    sidePlayerState.activeMoveId.c_str(), registry.get<Game::Health>(characterEntity).current, sideDummy.position.x, sideDummyState.facingRight ? 1 : 0,
                    sideDummyState.currentState.c_str(), registry.get<Game::Health>(dummyEntity).current);
            }
#endif
        }

        // rootTransform/meshWorldTransform are both Identity here, not characterRootTransform -
        // see Engine::ComputeSkinningMatrices's own comment for why this asset's skin data
        // specifically needs that. Runs every frame regardless of ClipPlayback::playing so a
        // paused clip still renders its current (frozen) pose rather than disappearing.
        //
        // No Advance() call here anymore - UpdateFighterState (in the tick loop above) now owns
        // this entity's playbackTimeSeconds directly, setting it from frame counts each tick so
        // the visible animation and frame-exact combat data (hit windows, cancel timing) share
        // one clock instead of drifting apart. This does mean the character's pose is now
        // 60Hz-stepped rather than smoothly wall-clock-interpolated - a deliberate consequence of
        // "gameplay-affecting logic runs once per tick," not a bug.
        //
        // Shared between the player and the dummy - both are separate GPU-backed instances of
        // the same imported asset (characterModel/characterSkin), differing only in their own
        // Transform/ClipPlayback/draw item/facing correction. advanceManually is player-only
        // (see manualClipOverride's own declaration) - false for the dummy, whose
        // playbackTimeSeconds is always frame-count-driven by UpdateFighterState instead.
        auto updateFighterRender = [&](entt::entity entity, float facingCorrectionRadians, float groundingOffsetY, bool advanceManually,
                                       Renderer::SkinnedMeshDrawItem* shadowItem = nullptr)
        {
            if (characterSkin == nullptr || characterModel.clips.empty() || !registry.all_of<Engine::SkinnedRenderable>(entity))
            {
                return;
            }
            Renderer::SkinnedMeshDrawItem& drawItem = *registry.get<Engine::SkinnedRenderable>(entity).drawItem;

            // World transform, recomputed and re-uploaded every frame - previously baked once at
            // startup from Transform{0,0,0} and never touched again, a real bug: moving
            // Transform.position (walk/run/jump) never actually moved the rendered mesh, only
            // its pose/animation updated. assetCorrection (scale) is deliberately NOT reapplied
            // here - it's already permanently baked into this draw item's vertex data (see the
            // rootTransform passed to CreateSkinnedMeshDrawItems at setup, above); multiplying it
            // in again here would double it every frame. facingCorrectionRadians (rotation) is
            // NOT baked into vertex data the same way (kept out deliberately so it stays a single
            // source of truth alongside the live Transform below, and to match FighterState.cpp's
            // bone-attached hitbox lookup, which applies it the same way rather than assuming
            // it's pre-baked). groundingOffsetY is a world-space Y translate applied outermost
            // (row-vector convention: rightmost factor = last applied), seating this asset's feet
            // on the Y=0 ground independent of facing or the live Transform - see
            // CharacterDefinition::groundingOffsetY. FighterState.cpp's ResolveHitboxOffset adds
            // the same shift so bone-attached hitboxes drop with the mesh.
            {
                const DirectX::XMMATRIX facingCorrection = DirectX::XMMatrixRotationY(facingCorrectionRadians);
                const DirectX::XMMATRIX groundingOffset = DirectX::XMMatrixTranslation(0.0f, groundingOffsetY, 0.0f);
                const DirectX::XMMATRIX worldTransform =
                    facingCorrection * Engine::ComputeWorldMatrix(registry.get<Engine::Transform>(entity)) * groundingOffset;
                DirectX::XMFLOAT4X4 worldForGpu;
                DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(worldTransform));
                Renderer::Buffer::Update(drawItem.worldBuffer, &worldForGpu, sizeof(worldForGpu));
#if defined(OMD_PROTOTYPE)
                if (shadowItem != nullptr)
                {
                    // The shadow copy uses the fighter's own world matrix; SkinnedMesh.hlsl's
                    // flat shadow mode does the projection onto the floor.
                    Renderer::Buffer::Update(shadowItem->worldBuffer, &worldForGpu, sizeof(worldForGpu));
                }
#endif
            }

            Engine::Animator& animator = registry.get<Engine::Animator>(entity);
            // Manual override restores the pre-state-machine behavior for this entity only:
            // wall-clock advance of one clip in isolation (the state machine, skipped above,
            // isn't setting the time). No crossfade in this mode - it's a raw clip preview.
            if (advanceManually)
            {
                animator.useBlendSpace = false;
                animator.prevPose.nodes.clear();
                animator.blendRemaining = 0.0f;
                animator.overlayClipIndex = -1; // raw single-clip preview - no masked overlay
                animator.overlayMask = nullptr;
                if (manualClipPlaying)
                {
                    const float dur =
                        (animator.clipIndex >= 0 && animator.clipIndex < static_cast<int32_t>(characterModel.clips.size()))
                            ? characterModel.clips[static_cast<size_t>(animator.clipIndex)].durationSeconds
                            : 0.0f;
                    float t = animator.clipSeconds + deltaSeconds;
                    if (dur > 0.0f)
                    {
                        t = std::fmod(t, dur);
                    }
                    animator.clipSeconds = t;
                }
            }
            const Engine::Pose pose = Engine::EvaluateAnimator(animator, characterModel, characterModel.clips);
            Engine::UpdateSkinnedPose(
                characterModel, *characterSkin, pose, DirectX::XMMatrixIdentity(), DirectX::XMMatrixIdentity(), drawItem);
        };
        // +180 degrees when facing left (see FighterState::facingRight's own comment) - same
        // convention FighterState.cpp's ResolveHitboxOffset uses, so the rendered mesh and its
        // bone-attached hitboxes always agree on which way this tick's facing actually points.
        // NOTE: this assumes facingRight=true (no extra rotation) faces toward +X in world
        // space, matching this asset's own unmirrored facingCorrectionRadians. If fighters turn
        // out to face AWAY from each other instead of toward once run, the fix is a one-line
        // sign flip - swap the "<=" comparisons in the facing block above (or equivalently swap
        // which case gets the +XM_PI here and in ResolveHitboxOffset).
        const float playerFacingRadians =
            characterDefinition.facingCorrectionRadians +
            (registry.get<Game::FighterState>(characterEntity).facingRight ? 0.0f : DirectX::XM_PI);
        const float dummyFacingRadians =
            dummyDefinition.facingCorrectionRadians + (registry.get<Game::FighterState>(dummyEntity).facingRight ? 0.0f : DirectX::XM_PI);
#if defined(OMD_PROTOTYPE)
        updateFighterRender(
            characterEntity, playerFacingRadians, characterDefinition.groundingOffsetY, manualClipOverride,
            characterShadowItems.empty() ? nullptr : &characterShadowItems[0]);
        updateFighterRender(
            dummyEntity, dummyFacingRadians, dummyDefinition.groundingOffsetY, /*advanceManually*/ false,
            dummyShadowItems.empty() ? nullptr : &dummyShadowItems[0]);
#else
        updateFighterRender(characterEntity, playerFacingRadians, characterDefinition.groundingOffsetY, manualClipOverride);
        updateFighterRender(dummyEntity, dummyFacingRadians, dummyDefinition.groundingOffsetY, /*advanceManually*/ false);
#endif

        // Shadow discs track their fighter's current X/Z every frame and fade with jump
        // height (Y) - see FighterShadow.h's own comment.
        {
            const Engine::Transform& playerTransform = registry.get<Engine::Transform>(characterEntity);
            const Engine::Transform& dummyTransform = registry.get<Engine::Transform>(dummyEntity);
            Game::UpdateFighterShadowPosition(
                playerShadowDrawItem, playerTransform.position.x, playerTransform.position.y, playerTransform.position.z);
            Game::UpdateFighterShadowPosition(
                dummyShadowDrawItem, dummyTransform.position.x, dummyTransform.position.y, dummyTransform.position.z);
        }
#if defined(OMD_PROTOTYPE)
        Game::UpdateProjectileRender(registry, projectileSystem);
        if (prototype.vfx || menuActive)
        {
            Game::UpdateBackdropFire(backdrop, vfxSystem, menuActive ? shakenCamera.position.x : camera.position.x, deltaSeconds);
            Game::UpdateVfx(registry, vfxSystem, shakenCamera, deltaSeconds);
            Game::UpdateVfxPools(registry, vfxSystem, deltaSeconds);
        }
        else
        {
            Game::ClearVfx(vfxSystem);
        }
        Game::UpdateVfxRender(vfxSystem, shakenCamera);
        Game::SetBackdropVisible(backdrop, prototype.backdrop || menuActive);
        Game::SetBackdropMenu(backdrop, menuActive);
#endif

#ifdef OMD_DEV_TOOLS
        // Rebuilt every render frame from the latest resolved tick's events (see above) - cheap
        // at this entity count. Purely feeds DebugDrawPass's visualization (itself dev-tools-
        // gated), so gated together rather than built and silently never rendered.
        std::vector<Renderer::DebugDrawLine> debugLines = Engine::BuildCollisionDebugLines(registry, lastCollisionEvents);
        Renderer::DebugDrawPass::SetLines(debugLines);
#endif

#ifdef OMD_DEV_TOOLS
        // Character is real, shipped content - shown above RenderTasks' own "Debug" section,
        // not inside it. Kept minimal on purpose: this window (RenderTasks' own "Renderer
        // Debug") is meant to stay short enough never to need scrolling - everything else
        // Game contributes (fighter/combo readouts, animation/hitbox editing tools) lives in
        // extraWindowsUI's own windows below instead of growing this one.
        auto primaryContentUI = [&]()
        {
            if (ImGui::Checkbox("Character", &enableCharacter))
            {
                rebuildDrawItems();
            }
            // Opens the "Animation Editor" window (extraWindowsUI below) - see
            // showAnimationEditor's own declaration for why this is independent of Ctrl+R.
            // A button rather than a checkbox: this only ever opens the window - closing it is
            // the window's own titlebar [x] (same bool either way), not a second way to do the
            // same thing here.
            if (ImGui::Button("Open animation editor"))
            {
                showAnimationEditor = true;
            }
#if defined(OMD_PROTOTYPE)
            // Same switch as the one next to the AI simulation controls (and F4).
            ImGui::Checkbox("Fast-forward (F4)", &fastForward);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::SliderFloat("##fastForwardFactorMain", &fastForwardFactor, 1.5f, 8.0f, "x%.1f");
#endif
        };
        auto debugSectionUI = [&]()
        {
            // On-screen orientation gizmo (top-right corner, Blender-style) - projects the
            // world X/Y/Z axes through the free-fly camera's current rotation onto a small
            // fixed screen-space widget with letter labels. Replaces the old world-space
            // DebugDrawLine cross anchored at the character (unlabeled colored lines competing
            // visually with real geometry, and only visible while "Collision debug draw" was
            // also on). Drawn on the foreground draw list, which renders over all ImGui windows
            // and 3D content regardless of window layout - not itself a window, and not gated
            // behind any checkbox, since it's a navigation aid rather than a collision-debug
            // artifact.
            {
                const Engine::CameraBasis basis = Engine::ComputeCameraBasis(camera);
                const ImVec2 center(static_cast<float>(Renderer::Device::GetWidth()) - 70.0f, 70.0f);
                constexpr float kRadius = 42.0f;

                ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
                foregroundDrawList->AddCircleFilled(center, kRadius + 16.0f, IM_COL32(20, 20, 20, 90));

                struct AxisSpec
                {
                    DirectX::XMFLOAT3 worldDir;
                    ImU32 color;
                    const char* label;
                };
                static const AxisSpec kAxes[] = {
                    { { 1.0f, 0.0f, 0.0f }, IM_COL32(230, 70, 70, 255), "X" },
                    { { 0.0f, 1.0f, 0.0f }, IM_COL32(90, 210, 90, 255), "Y" },
                    { { 0.0f, 0.0f, 1.0f }, IM_COL32(90, 150, 230, 255), "Z" },
                };
                for (const AxisSpec& axis : kAxes)
                {
                    const float screenRight = axis.worldDir.x * basis.right.x + axis.worldDir.y * basis.right.y +
                        axis.worldDir.z * basis.right.z;
                    // Screen Y grows downward, unlike world/camera up - negated so "up" in world
                    // space draws upward on screen.
                    const float screenUp = axis.worldDir.x * basis.up.x + axis.worldDir.y * basis.up.y + axis.worldDir.z * basis.up.z;
                    const ImVec2 tip(center.x + screenRight * kRadius, center.y - screenUp * kRadius);
                    foregroundDrawList->AddLine(center, tip, axis.color, 2.5f);
                    foregroundDrawList->AddCircleFilled(tip, 4.0f, axis.color);
                    const ImVec2 textSize = ImGui::CalcTextSize(axis.label);
                    const ImVec2 labelPos(
                        center.x + screenRight * (kRadius + 14.0f) - textSize.x * 0.5f,
                        center.y - screenUp * (kRadius + 14.0f) - textSize.y * 0.5f);
                    foregroundDrawList->AddText(labelPos, IM_COL32(255, 255, 255, 255), axis.label);
                }
            }

            if (ImGui::Checkbox("Local test scene", &enableLocalTestScene))
            {
                rebuildDrawItems();
            }

            // See allowCameraMouse above - this is the manual half of that decision (the
            // automatic half, io.WantCaptureMouse, needs no UI). Unrelated to the free-fly
            // camera's future fixed-2D-view toggle (not built yet - a future "which camera is
            // active" mode switch, not a mouse-specific concern like this one).
            ImGui::SeparatorText("Camera");
            ImGui::Checkbox("Camera follows players", &enableCameraFollow);
            ImGui::Checkbox("Camera mouse control", &enableCameraMouseControl);
            // See allowCameraKeyboard above - same manual+automatic pattern as the mouse toggle,
            // just off by default (see enableCameraKeyboardControl's own declaration for why).
            ImGui::Checkbox("Camera keyboard control", &enableCameraKeyboardControl);
            ImGui::Checkbox("Disable character movement through controller analog stick", &disableCharacterAnalogStickMovement);
        };

        // Everything below used to also live in debugSectionUI above, all stacked into the one
        // "Renderer Debug" window - scrolling through fighter/combo readouts to reach hitbox or
        // character tuning (or the other way around) was the actual complaint that split it.
        // Split by how often each is touched: fighterDebugUI is read/left-open during ordinary
        // play (live state, AI, KO/collision-rig toggles); animationEditorUI is a dedicated,
        // normally-closed tool session (previewing/scrubbing a clip, placing a hitbox) that
        // shouldn't cost the other window space while unused. Both are plain content lambdas,
        // same shape as primaryContentUI/debugSectionUI above - extraWindowsUI just below is
        // what actually opens a window and calls them.
        auto fighterDebugUI = [&]()
        {
            // Visualizes the fixed-tick loop and the InputCommand it produces, since nothing
            // else consumes either yet. One line per concern to stay compact. Names here are
            // Game's own (Engine's InputCommand has no idea these slots mean "Jump"/"Punch"/etc.
            // - see Engine/Input.h).
            static const char* kButtonNames[] = { "Jump", "Crouch", "Punch", "Kick", "Block", "Run" };
            ImGui::SeparatorText("Input");
            ImGui::Text("Tick: %u  Axis: %.2f", simClock.tickCount, lastInputCommand.axis);
            char buttonSummary[128] = {};
            size_t offset = 0;
            for (size_t i = 0; i < static_cast<size_t>(Game::FighterButton::Count); ++i)
            {
                const Engine::ButtonState& button = lastInputCommand.buttons[i];
                offset += static_cast<size_t>(snprintf(
                    buttonSummary + offset, sizeof(buttonSummary) - offset, "%s%s=%s%s%s", i == 0 ? "" : " ", kButtonNames[i],
                    button.held ? "held" : "-", button.pressedThisTick ? "(P)" : "", button.releasedThisTick ? "(R)" : ""));
            }
            ImGui::Text("%s", buttonSummary);

            // Phase 3 of the state-machine work - proves SelectMove() in isolation before it's
            // load-bearing for anything visible (no clip swap/state change wired up to it yet).
            ImGui::Text("Selected move: %s", lastSelectedMove.has_value() ? lastSelectedMove->c_str() : "-");

            // Fighter state machine readout - state name/frame count/health, live every tick.
            ImGui::SeparatorText("Fighter");
            {
                const Game::FighterState& fighterState = registry.get<Game::FighterState>(characterEntity);
                const Game::Health& health = registry.get<Game::Health>(characterEntity);
                const Engine::Transform& fighterTransform = registry.get<Engine::Transform>(characterEntity);
                ImGui::Text(
                    "State: %s  Frame: %u  Health: %d/%d  X: %.2f  Y: %.2f", fighterState.currentState.c_str(), fighterState.framesInState,
                    health.current, health.max, fighterTransform.position.x, fighterTransform.position.y);
            }

            // Combo debug - reads the same mechanism FighterState.cpp's own cancel loop uses
            // (a cancel target's own inputButton + its condition, evaluated against this same
            // hitConfirmed/frame context) so "is a follow-up reachable right now" is read
            // straight off the real state instead of inferred from replaying a hunch. Paired
            // with the rolling transition history logged (Log category "Combo") right after
            // this fighter's own UpdateFighterState call above.
            ImGui::SeparatorText("Combo debug");
            {
                const Game::FighterState& comboState = registry.get<Game::FighterState>(characterEntity);
                ImGui::Text(
                    "Active move: %s  hitConfirmed: %s", comboState.activeMoveId.empty() ? "-" : comboState.activeMoveId.c_str(),
                    comboState.moveHitConfirmed ? "true" : "false");
                if (comboState.currentState == "Attack")
                {
                    if (const Game::MoveDefinition* activeMove = characterDefinition.moveTable.FindById(comboState.activeMoveId))
                    {
                        Game::CombatDsl::EvaluationContext cancelContext;
                        cancelContext.getFlag = [&comboState](const std::string& name) -> bool
                        {
                            if (name == "hitConfirmed") return comboState.moveHitConfirmed;
                            return false;
                        };
                        cancelContext.getNumber = [&comboState](const std::string& name) -> float
                        {
                            if (name == "frame") return static_cast<float>(comboState.framesInMove);
                            return 0.0f;
                        };
                        for (const Game::CombatDsl::Cancel& cancel : activeMove->cancels)
                        {
                            const Game::MoveDefinition* target = characterDefinition.moveTable.FindById(cancel.toMoveId);
                            const bool conditionMet = Game::CombatDsl::EvaluateCondition(*cancel.condition, cancelContext);
                            ImGui::Text(
                                "  -> %s (button %s): %s", cancel.toMoveId.c_str(), target ? target->inputButton.c_str() : "?",
                                conditionMet ? "READY" : "waiting");
                        }
                    }
                }
                ImGui::Text("History (green = real cancel/hit-confirm link, white = fresh from neutral):");
                for (const ComboHistoryEntry& entry : comboHistory)
                {
                    if (entry.viaCancel)
                    {
                        ImGui::TextColored(
                            ImVec4(0.35f, 1.0f, 0.35f, 1.0f), "  t%u cancel -> %s", entry.tick, entry.moveId.c_str());
                    }
                    else
                    {
                        ImGui::Text("  t%u enter %s", entry.tick, entry.moveId.c_str());
                    }
                }
            }

            // Dummy readout - same shape as the player's own above, since UpdateFighterState
            // drives it through the identical generic state machine. Directly useful for
            // confirming HitStun/block/KO/push-back actually landed on a real second entity.
            ImGui::SeparatorText("Fighter (dummy)");
            {
                const Game::FighterState& dummyState = registry.get<Game::FighterState>(dummyEntity);
                const Game::Health& dummyHealth = registry.get<Game::Health>(dummyEntity);
                const Engine::Transform& dummyTransform = registry.get<Engine::Transform>(dummyEntity);
                ImGui::Text(
                    "State: %s  Frame: %u  Health: %d/%d  X: %.2f  Y: %.2f", dummyState.currentState.c_str(), dummyState.framesInState,
                    dummyHealth.current, dummyHealth.max, dummyTransform.position.x, dummyTransform.position.y);
            }
            // See dummyAiPreset's own declaration - a testing convenience, not real AI.
            static const char* kDummyAiPresetNames[] = { "Manual", "Always block", "Block then punish", "Fireball (every 2.5s)", "AI: Passive", "AI: Reactive", "AI: Aggressive" };
            ImGui::Combo("Dummy AI", &dummyAiPreset, kDummyAiPresetNames, IM_ARRAYSIZE(kDummyAiPresetNames));
            ImGui::BeginDisabled(dummyAiPreset != 0);
            ImGui::Checkbox("Dummy blocks", &dummyBlocksHeld);
            ImGui::EndDisabled();
            // See dummyUndying's own declaration - lets repeated combo testing keep going
            // without a manual Reset after every knockout.
            ImGui::Checkbox("Dummy undying (auto-revive after KO)", &dummyUndying);
#if defined(OMD_PROTOTYPE)
            ImGui::SeparatorText("Prototype");
            if (ImGui::Button("Restart in demo mode"))
            {
                restartDemoRequested = true;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(same as --demo)");
            ImGui::Checkbox("HUD (health bars / K.O. banner)", &prototype.hud);
            ImGui::Checkbox("Match flow (K.O. slow-mo + auto reset)", &prototype.matchFlow);
            ImGui::Checkbox("Juice (hit flash, camera shake)", &prototype.juice);
            ImGui::Checkbox("VFX (sparks, dust, fireball glow)", &prototype.vfx);
            ImGui::Checkbox("Backdrop (dungeon wall, torches)", &prototype.backdrop);
            ImGui::Checkbox("SFX", &prototype.sfx);
            ImGui::SameLine();
            ImGui::Checkbox("Music", &prototype.music);
            ImGui::SliderFloat("SFX volume", &prototype.sfxVolume, 0.0f, 3.0f);
            ImGui::SliderFloat("Music volume", &prototype.musicVolume, 0.0f, 1.0f);
            {
                bool randomized = sfxCues.Randomized();
                if (ImGui::Checkbox("Random SFX picks (F6)", &randomized))
                {
                    sfxCues.SetRandomized(randomized);
                }
                ImGui::SameLine();
                if (ImGui::Button("Next variant (F5)"))
                {
                    sfxCues.CycleVariant();
                }
                ImGui::SameLine();
                ImGui::Text("fixed: %d", sfxCues.FixedVariant() + 1);
                bool sparse = sfxCues.Sparse();
                if (ImGui::Checkbox("Sparse SFX (F7)", &sparse))
                {
                    sfxCues.SetSparse(sparse);
                }
                ImGui::Text("SFX families:");
                ImGui::Checkbox("Attack swing/voice: always", &sfxCues.categories.attackAlways);
                ImGui::Checkbox("Attack swing/voice: after K.O.", &sfxCues.categories.attackAfterKo);
                ImGui::Checkbox("Hit sound", &sfxCues.categories.hit);
                ImGui::Checkbox("Whiff swing sound", &sfxCues.categories.whiff);
                ImGui::Checkbox("Recorded set (hit/whiff/voice/K.O.)", &sfxCues.categories.own);
                ImGui::SameLine();
                ImGui::Checkbox("Attack voice", &sfxCues.categories.voice);
                ImGui::SliderFloat("Recorded hit level", &sfxCues.categories.ownHitVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("Recorded whiff level", &sfxCues.categories.ownWhiffVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("Recorded voice level", &sfxCues.categories.ownVoiceVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("Voice chance (light/medium)", &sfxCues.categories.voiceChance, 0.0f, 1.0f);
                ImGui::SliderFloat("Voice chance (heavy)", &sfxCues.categories.heavyVoiceChance, 0.0f, 1.0f);
                ImGui::SliderFloat("Recorded K.O. level", &sfxCues.categories.ownKoVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("Run roar level", &sfxCues.categories.runRoarVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("Run loop level", &sfxCues.categories.runLoopVolume, 0.0f, 1.0f);
                ImGui::SliderFloat("Older layer chance", &sfxCues.categories.legacyChance, 0.0f, 1.0f);
                ImGui::SliderFloat("Older layer level", &sfxCues.categories.legacyVolume, 0.0f, 1.0f);
                ImGui::SliderFloat("Hit level", &sfxCues.categories.hitVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("Whiff level", &sfxCues.categories.whiffVolume, 0.0f, 3.0f);
                ImGui::SliderFloat("K.O. cry level", &sfxCues.categories.koVolume, 0.0f, 2.0f);
                if (ImGui::Button("Solo hits"))
                {
                    auto& c = sfxCues.categories;
                    c.attackAlways = c.attackAfterKo = c.whiff = c.jump = c.fireball = c.run = c.koCry = false;
                    c.hit = true;
                }
                ImGui::Checkbox("Jump / land", &sfxCues.categories.jump);
                ImGui::Checkbox("Fireball", &sfxCues.categories.fireball);
                ImGui::Checkbox("Run", &sfxCues.categories.run);
                ImGui::Checkbox("K.O. cry", &sfxCues.categories.koCry);
            }
            {
                bool simulation = aiSimulation;
                if (ImGui::Checkbox("AI simulation - both fighters (F2)", &simulation))
                {
                    if (simulation)
                    {
                        startAiSimulation();
                    }
                    else
                    {
                        aiSimulation = false;
                    }
                }
                int seed = static_cast<int>(aiSeed);
                if (ImGui::InputInt("Seed", &seed))
                {
                    aiSeed = static_cast<uint32_t>(seed > 0 ? seed : 0);
                }
                ImGui::SameLine();
                if (ImGui::Button("Restart with next seed (F3)"))
                {
                    ++aiSeed;
                    startAiSimulation();
                }
                ImGui::SliderFloat("Fight length (s)", &fightDirector.fightLengthSeconds, 20.0f, 180.0f);
                ImGui::SliderFloat("Game speed", &gameSpeed, 0.5f, 2.0f);
                ImGui::Checkbox("Fast-forward (F4)", &fastForward);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("##fastForwardFactor", &fastForwardFactor, 1.5f, 8.0f, "x%.1f");
                ImGui::SliderFloat("Health scale (next round)", &healthScale, 0.5f, 4.0f);
                if (aiSimulation)
                {
                    static const char* kRoleNames[] = { "Neutral", "Attacker", "Defender" };
                    ImGui::Text(
                        "t=%.1fs%s  P1: %s  P2: %s", fightDirector.elapsedSeconds, fightDirector.finale ? " FINALE" : "",
                        kRoleNames[static_cast<int>(fightDirector.role[0])], kRoleNames[static_cast<int>(fightDirector.role[1])]);
                }
            }
#endif

            // Combined with "Collision debug draw" above (RenderTasks' own toggle) - this alone
            // draws nothing, it just widens WHEN a move's hitbox exists so there's something to
            // see with that toggle on. Also read by the Hitbox tuning preview in the Animation
            // Editor window, hence living here rather than there - it affects real gameplay
            // hitboxes too, not just the preview.
            ImGui::Checkbox("Force show all hitboxes (whole Attack state, not just active frames)", &forceShowAllHitboxes);

            // Per-asset placement tuning (character.combat's "correction" / "ko" blocks). Edits
            // are live in memory on both fighters; "Save" writes them back via
            // Game::SaveCharacterToFile, same memory-only-until-clicked contract as the hitbox
            // tool. The Hurtbox is startup-baked from groundingOffsetY, so its debug-draw box
            // won't follow the slider until a restart (mesh + bone-attached hitboxes do follow).
            ImGui::SeparatorText("Character");
            {
                bool characterTuningChanged = false;
                if (ImGui::SliderFloat("Grounding offset Y", &characterDefinition.groundingOffsetY, -0.6f, 0.6f, "%.3f"))
                {
                    dummyDefinition.groundingOffsetY = characterDefinition.groundingOffsetY;
                    characterTuningChanged = true;
                }

                if (ImGui::SliderFloat("KO drop offset Y", &characterDefinition.koDropOffsetY, -1.5f, 0.0f, "%.3f"))
                {
                    dummyDefinition.koDropOffsetY = characterDefinition.koDropOffsetY;
                    characterTuningChanged = true;
                }
                // Raw clip frames - rescaled into both definitions' speed-scaled runtime fields
                // on any change (see koDropStartFrameRawUI's own declaration).
                bool koFramesChanged = ImGui::SliderInt("KO drop start frame", &koDropStartFrameRawUI, 0, 90);
                koFramesChanged |= ImGui::SliderInt("KO drop end frame", &koDropEndFrameRawUI, 0, 90);
                if (koFramesChanged)
                {
                    const float invSpeed = 1.0f / Game::kCombatSpeedMultiplier;
                    characterDefinition.koDropStartFrame =
                        static_cast<uint32_t>(std::lround(static_cast<float>(koDropStartFrameRawUI) * invSpeed));
                    characterDefinition.koDropEndFrame =
                        static_cast<uint32_t>(std::lround(static_cast<float>(koDropEndFrameRawUI) * invSpeed));
                    dummyDefinition.koDropStartFrame = characterDefinition.koDropStartFrame;
                    dummyDefinition.koDropEndFrame = characterDefinition.koDropEndFrame;
                    characterTuningChanged = true;
                }
                if (characterTuningChanged)
                {
                    characterTuningSaveMessage.clear();
                }

                // KO is otherwise awkward to trigger on demand - drop the player's HP to 0 and
                // the shared "healthDepleted" transition takes them to KO next tick.
                if (ImGui::Button("KO player now"))
                {
                    registry.get<Game::Health>(characterEntity).current = 0;
                }
                ImGui::SameLine();
                if (ImGui::Button("Save to character.combat"))
                {
                    // Slider-bounded to [0, 90], so these are always non-negative.
                    const bool saved = Game::SaveCharacterToFile(
                        kCharacterFilePath, characterDefinition.groundingOffsetY, characterDefinition.koDropOffsetY,
                        static_cast<uint32_t>(koDropStartFrameRawUI), static_cast<uint32_t>(koDropEndFrameRawUI));
                    characterTuningSaveMessage = saved ? "Saved." : "Save failed - see logs/omdlab.log.";
                }
                if (!characterTuningSaveMessage.empty())
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", characterTuningSaveMessage.c_str());
                }
            }

            // Collision module test rig - see the entity setup above for why this one entity
            // still exists (the dummy above now covers hitbox testing). Enable "Collision debug
            // draw" above to see the box; drag it to make the test trigger overlap a hurtbox
            // (turns red, Triggers count increases).
            ImGui::SeparatorText("Collision (test rig)");
            ImGui::SliderFloat("Test trigger X", &registry.get<Engine::Transform>(testTriggerEntity).position.x, -3.0f, 3.0f);
            ImGui::Text("Hits: %zu  Triggers: %zu", lastCollisionEvents.hits.size(), lastCollisionEvents.triggers.size());
        };

        // Move preview/scrub, hitbox placement, and clip timing - all three drive the character
        // from an isolated preview rather than real input/state, watch it, drag a number, Save,
        // and none is needed for ordinary play, which is why they share a window kept closed by
        // default (see showAnimationEditor's own declaration) instead of living in
        // fighterDebugUI above. "Move" is the shared anchor for all three (which move's preview
        // is Hitbox tuning/Clip timing below actually editing); "Manual clip override" is a
        // separate, unrelated tool (a raw clip, not tied to any move) and sits at the bottom for
        // that reason - see its own comment below.
        auto animationEditorUI = [&]()
        {
            // "None" (index 0 here, -1 in hitboxTuningMoveIndex) hands the character back to
            // real gameplay - see hitboxTuningMoveIndex's own declaration.
            ImGui::SeparatorText("Move");
            int hitboxTuningComboIndex = hitboxTuningMoveIndex + 1;
            if (ImGui::Combo(
                    "Move", &hitboxTuningComboIndex, hitboxTuningMoveNames.data(), static_cast<int>(hitboxTuningMoveNames.size())))
            {
                hitboxTuningMoveIndex = hitboxTuningComboIndex - 1;
                hitboxTuningFrameCounter = 0;
                hitboxTuningBoxIndex = 0;
                hitboxTuningSaveMessage.clear();
                timingSaveMessage.clear();
                timingOutRawUI = (hitboxTuningMoveIndex >= 0 &&
                                   hitboxTuningMoveIndex < static_cast<int>(characterDefinition.moveTable.moves.size()))
                                      ? static_cast<int>(std::lround(
                                            static_cast<float>(
                                                characterDefinition.moveTable.moves[static_cast<size_t>(hitboxTuningMoveIndex)]
                                                    .timingOutFrames) *
                                            Game::kCombatSpeedMultiplier))
                                      : 0;
            }

            if (hitboxTuningMoveIndex >= 0 && hitboxTuningMoveIndex < static_cast<int>(characterDefinition.moveTable.moves.size()))
            {
                Game::MoveDefinition& previewMove = characterDefinition.moveTable.moves[static_cast<size_t>(hitboxTuningMoveIndex)];

                // Preview - pause and scrub straight to a keyframe instead of chasing a
                // continuously-looping playback. Drives the exact code path a real Attack uses
                // (Game::PreviewMoveHitbox -> ApplyStateEffects), so this is a frame-accurate look
                // at the actual WARPED playback the selected move produces in real combat - not
                // an approximation, and not the same thing "Manual clip override" below shows
                // (that one plays the raw, unwarped clip, with no move data involved at all).
                // Shared by Hitbox tuning and Clip timing underneath: enable "Collision debug
                // draw" (Renderer Debug window) and "Force show all hitboxes" (Fighter Debug
                // window) to watch a hitbox continuously while scrubbing here.
                ImGui::SeparatorText("Preview");
                ImGui::Checkbox("Pause", &hitboxTuningPaused);
                const uint32_t previewTotalFrames = previewMove.startupFrames + previewMove.activeFrames + previewMove.recoveryFrames;
                int hitboxTuningFrameSlider = static_cast<int>(hitboxTuningFrameCounter);
                ImGui::BeginDisabled(!hitboxTuningPaused);
                if (ImGui::SliderInt(
                        "Frame", &hitboxTuningFrameSlider, 0, static_cast<int>(previewTotalFrames > 0 ? previewTotalFrames - 1 : 0)))
                {
                    hitboxTuningFrameCounter = static_cast<uint32_t>(hitboxTuningFrameSlider);
                }
                ImGui::EndDisabled();
                ImGui::Text(
                    "startup=%u active=%u recovery=%u", previewMove.startupFrames, previewMove.activeFrames,
                    previewMove.recoveryFrames);

                ImGui::SeparatorText("Hitbox tuning");
                if (previewMove.hitboxes.empty())
                {
                    ImGui::TextDisabled("Selected move has no hitboxes");
                }
                else
                {
                    // Only shown once a move actually has more than one hitbox (none do
                    // today) - no point cluttering the common single-hitbox case with a
                    // slider that only ever reads 0.
                    if (previewMove.hitboxes.size() > 1)
                    {
                        ImGui::SliderInt("Hitbox index", &hitboxTuningBoxIndex, 0, static_cast<int>(previewMove.hitboxes.size()) - 1);
                        hitboxTuningSaveMessage.clear();
                    }
                    hitboxTuningBoxIndex = std::clamp(hitboxTuningBoxIndex, 0, static_cast<int>(previewMove.hitboxes.size()) - 1);
                    Game::MoveHitboxDef& hitboxDef = previewMove.hitboxes[static_cast<size_t>(hitboxTuningBoxIndex)];
                    Engine::CollisionBox& box = hitboxDef.box;
                    ImGui::SliderFloat3("Offset", &box.offset.x, -1.0f, 1.0f, "%.3f");
                    ImGui::SliderFloat3("Half-extents", &box.halfExtents.x, 0.05f, 1.5f, "%.3f");
                    // Active-frame window - defaults to [startup, startup+active) (see
                    // MoveHitboxDef's own comment) but tunable independently here, since
                    // that default often doesn't line up with when THIS specific retargeted
                    // clip's own bone is actually near a target - scrub "Frame" above (with
                    // "Force show all hitboxes" on) to find where contact really looks
                    // right, then drag these two to match.
                    int frameStartSlider = static_cast<int>(hitboxDef.frameStart);
                    int frameEndSlider = static_cast<int>(hitboxDef.frameEnd);
                    const int previewTotalFramesInt = static_cast<int>(previewTotalFrames);
                    if (ImGui::SliderInt("Active start frame", &frameStartSlider, 0, previewTotalFramesInt))
                    {
                        hitboxDef.frameStart = static_cast<uint32_t>(frameStartSlider);
                    }
                    if (ImGui::SliderInt("Active end frame", &frameEndSlider, 0, previewTotalFramesInt))
                    {
                        hitboxDef.frameEnd = static_cast<uint32_t>(frameEndSlider);
                    }
                    if (ImGui::Button("Save to moves.combat"))
                    {
                        const bool saved = Game::SaveHitboxToFile(
                            kMovesFilePath, previewMove.id, hitboxTuningBoxIndex, box, hitboxDef.frameStart, hitboxDef.frameEnd);
                        hitboxTuningSaveMessage = saved ? "Saved." : "Save failed - see logs/omdlab.log.";
                    }
                    if (!hitboxTuningSaveMessage.empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", hitboxTuningSaveMessage.c_str());
                    }
                }

                // Trims either side of the windup->contact core WarpedClipSeconds already
                // plays (FighterState.cpp's own comment on the full four-segment shape) -
                // Start skips a lazy lead-in, Out/End decide how much of the clip's
                // post-contact follow-through actually shows before the pose hands off to
                // idle/jump, instead of that always happening on the very first tick past
                // contact (today's hardcoded default, still the default here too - every
                // slider below starts at the value that reproduces it exactly). Same move,
                // same Pause/Frame scrub above - pause and drag "Frame" past startup+active
                // to watch this segment.
                ImGui::SeparatorText("Clip timing");
                if (!previewMove.HasTimingCurve())
                {
                    ImGui::TextDisabled("Selected move has no timing curve (no impact set) - clip timing has no effect");
                }
                else
                {
                    bool timingChanged = false;
                    float startFraction = previewMove.timingStartFraction;
                    // std::clamp, not std::max - Windows.h's own min/max macros (NOMINMAX
                    // isn't defined in this project) break a bare std::max the same way they
                    // break std::min elsewhere in this file.
                    if (ImGui::SliderFloat(
                            "Start (clip fraction)", &startFraction, 0.0f, std::clamp(previewMove.timingWindupFraction, 0.0f, 1.0f),
                            "%.3f"))
                    {
                        previewMove.timingStartFraction = startFraction;
                        timingChanged = true;
                    }

                    // Bound matches FinalizeMoveTiming's own clamp exactly (MoveTable.cpp) -
                    // a value tuned here can never be silently reduced on the next load.
                    const uint32_t afterContact = previewMove.startupFrames + std::max<uint32_t>(1u, previewMove.timingSnapFrames);
                    const uint32_t maxOutRuntime = previewTotalFrames > afterContact ? previewTotalFrames - afterContact : 0;
                    const int maxOutRawUI =
                        static_cast<int>(std::lround(static_cast<float>(maxOutRuntime) * Game::kCombatSpeedMultiplier));
                    timingOutRawUI = std::clamp(timingOutRawUI, 0, maxOutRawUI);
                    if (ImGui::SliderInt("Out (ticks past contact)", &timingOutRawUI, 0, maxOutRawUI))
                    {
                        previewMove.timingOutFrames =
                            static_cast<uint32_t>(std::lround(static_cast<float>(timingOutRawUI) / Game::kCombatSpeedMultiplier));
                        timingChanged = true;
                    }

                    float endFraction = previewMove.timingEndFraction;
                    if (ImGui::SliderFloat(
                            "End (clip fraction)", &endFraction, std::clamp(previewMove.timingContactFraction, 0.0f, 1.0f), 1.0f,
                            "%.3f"))
                    {
                        previewMove.timingEndFraction = endFraction;
                        timingChanged = true;
                    }
                    if (timingChanged)
                    {
                        timingSaveMessage.clear();
                    }
                    if (ImGui::Button("Save timing to moves.combat"))
                    {
                        const bool saved = Game::SaveMoveTimingToFile(
                            kMovesFilePath, previewMove.id, previewMove.timingStartFraction, static_cast<uint32_t>(timingOutRawUI),
                            previewMove.timingEndFraction);
                        timingSaveMessage = saved ? "Saved." : "Save failed - see logs/omdlab.log.";
                    }
                    if (!timingSaveMessage.empty())
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", timingSaveMessage.c_str());
                    }
                }
            }

            // A raw clip, independent of the Move picker above - no move needs to exist yet.
            // Useful for reading a brand-new clip's own real length/poses (see "Timing a move" in
            // the combat guide) before a move's startup/active/recovery are even authored, which
            // is the one thing "Preview" above genuinely can't do (it needs an already-defined
            // move's own frame budget to scrub against). Off by default: UpdateFighterState
            // (Idle/Walk/Run/Attack/.../KO) owns this entity's clip during real play and would
            // otherwise overwrite any manual pick made below on the very next tick.
            if (!characterClipNames.empty())
            {
                ImGui::SeparatorText("Manual clip override (raw clip, no move)");
                ImGui::Checkbox("Manual clip override", &manualClipOverride);
                Engine::Animator& characterAnimator = registry.get<Engine::Animator>(characterEntity);
                int clipIndex = characterAnimator.clipIndex;
                ImGui::BeginDisabled(!manualClipOverride);
                if (ImGui::Combo("Clip", &clipIndex, characterClipNames.data(), static_cast<int>(characterClipNames.size())))
                {
                    characterAnimator.useBlendSpace = false;
                    characterAnimator.clipIndex = clipIndex;
                    characterAnimator.clipSeconds = 0.0f;
                }
                ImGui::Checkbox("Playing", &manualClipPlaying);
                // Scrub + readout. Drag to a pose, read off its clip fraction - that fraction is
                // exactly what a move's timingContact / timingWindup fields want. Dragging pauses
                // playback so the pose holds still while it's read.
                if (clipIndex >= 0 && clipIndex < static_cast<int>(characterModel.clips.size()))
                {
                    const float scrubDuration = characterModel.clips[static_cast<size_t>(clipIndex)].durationSeconds;
                    if (scrubDuration > 0.0f)
                    {
                        float clipFraction = std::clamp(characterAnimator.clipSeconds / scrubDuration, 0.0f, 1.0f);
                        if (ImGui::SliderFloat("Scrub (clip fraction)", &clipFraction, 0.0f, 1.0f, "%.3f"))
                        {
                            manualClipPlaying = false;
                            characterAnimator.clipSeconds = clipFraction * scrubDuration;
                        }
                        ImGui::Text(
                            "clip %.3f   frame %d / %d   %.3fs / %.3fs", clipFraction,
                            static_cast<int>(std::round(clipFraction * scrubDuration * 60.0f)),
                            static_cast<int>(std::round(scrubDuration * 60.0f)), clipFraction * scrubDuration, scrubDuration);
                    }
                }
                ImGui::EndDisabled();
            }
        };

        // Opens whatever windows fighterDebugUI/animationEditorUI above actually need - see
        // RenderTasks::DoFrame's own comment on extraWindowsUI for why this owns its own
        // Begin()/End() calls instead of being nested inside "Renderer Debug" the way
        // primaryContentUI/debugSectionUI are. Sizes/positions are first-run defaults only
        // (ImGuiCond_FirstUseEver) - imgui.ini remembers whatever the user drags them to after
        // that, same as every other ImGui window here.
        auto extraWindowsUI = [&]()
        {
            // Height deliberately stays within the game's own fixed windowHeight (720, main.cpp)
            // minus this window's y - ImGui doesn't clip a window's position to the app's own
            // client area (no multi-viewport backend here), so a taller first-run request would
            // silently render past the bottom of the screen instead of scrolling within itself.
            // Content that still doesn't fit gets this window's own scrollbar, same as any
            // other ImGui window - that's the graceful case, going off-screen is not.
            ImGui::SetNextWindowPos(ImVec2(360.0f, 20.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(460.0f, 680.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Fighter Debug");
            fighterDebugUI();
            ImGui::End();

            // Closed by default and opened by the "Open animation editor" button in Renderer
            // Debug - see showAnimationEditor's own declaration. The window's own titlebar
            // close button also clears showAnimationEditor via this same p_open pointer, so
            // either one closes it and the checkbox reflects it back correctly next frame.
            if (showAnimationEditor)
            {
                ImGui::SetNextWindowPos(ImVec2(840.0f, 20.0f), ImGuiCond_FirstUseEver);
                ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);
                ImGui::Begin("Animation Editor", &showAnimationEditor);
                animationEditorUI();
                ImGui::End();
            }
        };

        // Feed the capture annotation / motion.csv with this frame's player-fighter state, and
        // (in a capture run) queue a screenshot when the script says so.
        {
            const Game::FighterState& playerState = registry.get<Game::FighterState>(characterEntity);
            const Engine::Animator& playerAnimator = registry.get<Engine::Animator>(characterEntity);
            // For locomotion (blend space) the meaningful clock is the shared phase [0,1), not a
            // clip second - the state field (Walk/Run/Idle) says which the number is.
            const float clockValue = playerAnimator.useBlendSpace ? playerAnimator.blendPhase01 : playerAnimator.clipSeconds;
            Engine::FrameCapture::SetContext(
                simClock.tickCount, playerState.currentState.c_str(), playerState.framesInMove, clockValue,
                playerState.activeMoveId.c_str());

            // Skeleton-space signal for motion.csv / summary.json: recompose the player's world
            // node transforms this tick (cheap - one entity, ~dozens of nodes, capture runs
            // only) and report the strike effector + body centre of mass and their speeds. The
            // effector is the active move's hitbox bone while attacking, else the resolved
            // default hand. Runs every tick in a capture run (not just captured ones) so speed
            // is always a one-fixed-tick difference; SetSkeleton only stages - ResolvePending
            // records it on the frames that are actually grabbed.
            if (captureRun && characterSkin != nullptr && !characterModel.clips.empty())
            {
                const Engine::Animator& capAnimator = registry.get<Engine::Animator>(characterEntity);
                const Engine::Pose capPose = Engine::EvaluateAnimator(capAnimator, characterModel, characterModel.clips);
                const std::vector<DirectX::XMMATRIX> capWorld =
                    Engine::ComposePoseToWorld(characterModel, capPose, DirectX::XMMatrixIdentity());

                DirectX::XMVECTOR com = DirectX::XMVectorZero();
                for (const DirectX::XMMATRIX& m : capWorld)
                {
                    com = DirectX::XMVectorAdd(com, m.r[3]);
                }
                if (!capWorld.empty())
                {
                    com = DirectX::XMVectorScale(com, 1.0f / static_cast<float>(capWorld.size()));
                }
                // capWorld is composed from Identity (pose-local) - add the fighter's own world
                // position so effector/COM speed and travel include locomotion and lunge, not
                // just the pose's internal sway. Facing/grounding are skipped: the capture player
                // faces +X the whole run and grounding is a constant Y offset, neither of which
                // changes a distance or a speed.
                const DirectX::XMFLOAT3& capPos = registry.get<Engine::Transform>(characterEntity).position;
                const DirectX::XMVECTOR capPosV = DirectX::XMVectorSet(capPos.x, capPos.y, capPos.z, 0.0f);
                com = DirectX::XMVectorAdd(com, capPosV);

                int effNode = captureEffectorNodeDefault;
                if (playerState.currentState == "Attack")
                {
                    if (const Game::MoveDefinition* mv = characterDefinition.moveTable.FindById(playerState.activeMoveId))
                    {
                        for (const Game::MoveHitboxDef& hb : mv->hitboxes)
                        {
                            if (hb.resolvedJointIndex >= 0)
                            {
                                effNode = hb.resolvedJointIndex;
                                break;
                            }
                        }
                    }
                }
                const bool effValid = effNode >= 0 && effNode < static_cast<int>(capWorld.size());
                const DirectX::XMVECTOR effPos = effValid
                                                     ? DirectX::XMVectorAdd(capWorld[static_cast<size_t>(effNode)].r[3], capPosV)
                                                     : DirectX::XMVectorZero();

                constexpr float capDt = Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
                float effSpeed = 0.0f;
                float comSpeed = 0.0f;
                if (captureSkeletonHasPrev)
                {
                    effSpeed =
                        DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(effPos, capturePrevEffector))) /
                        capDt;
                    comSpeed =
                        DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(com, capturePrevCom))) / capDt;
                }
                const float effReach =
                    DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(effPos, com)));

                DirectX::XMFLOAT3 e3;
                DirectX::XMFLOAT3 c3;
                DirectX::XMStoreFloat3(&e3, effPos);
                DirectX::XMStoreFloat3(&c3, com);
                Engine::FrameCapture::SetSkeleton(effValid, &e3.x, effSpeed, effReach, &c3.x, comSpeed);

                capturePrevEffector = effPos;
                capturePrevCom = com;
                captureSkeletonHasPrev = true;
            }

            if (captureRun)
            {
                std::string caption;
                if (captureRun->CaptureThisTick(simClock.tickCount, caption))
                {
                    Engine::FrameCapture::Grab(caption);
                }
            }
        }

        if (Renderer::RenderTasks::DoFrame(
                viewProjection, primaryContentUI, debugSectionUI, extraWindowsUI, captureDebugUIVisible && !menuActive,
                // The readout is part of the debug UI in live play (hidden with it); scripted
                // capture runs always carry it, since their frames have the debug UI off.
                ((captureRun || captureDebugUIVisible) && !menuActive) ? Engine::FrameCapture::AnnotationText() : nullptr, hudUI))
        {
            camera = Engine::Camera{};
            enableCameraFollow = true;
            enableCameraMouseControl = true;
            enableCameraKeyboardControl = false;
            disableCharacterAnalogStickMovement = true;
            enableCharacter = true;
            enableLocalTestScene = false;
            manualClipOverride = false;
            forceShowAllHitboxes = false;
            // Does NOT revert any in-memory hitbox edits made via the tuning sliders above -
            // only turns tuning off and hands the character back to real gameplay, same "no
            // write-back, no undo either" scope the tool itself deliberately keeps (see
            // hitboxTuningMoveIndex's own declaration).
            hitboxTuningMoveIndex = -1;
            hitboxTuningBoxIndex = 0;
            hitboxTuningFrameCounter = 0;
            hitboxTuningPaused = false;
            hitboxTuningSaveMessage.clear();
            timingSaveMessage.clear();
            registry.get<Engine::Transform>(testTriggerEntity).position.x = -2.0f;
            // Also recovers the character's own fighter state/health - previously left out, which
            // meant landing in KO (whether from real play or the test-hitbox slider above) had no
            // UI-accessible way back to Idle short of restarting the app.
            registry.get<Engine::Transform>(characterEntity) = Engine::Transform{};
            registry.replace<Game::FighterState>(characterEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                characterEntity, Game::Health{ characterDefinition.stats.maxHealth, characterDefinition.stats.maxHealth });
            // Same recovery for the dummy - it can reach KO/HitStun exactly like the player can.
            registry.get<Engine::Transform>(dummyEntity) = Engine::Transform{ { Game::kDefaultFighterGapX, 0.0f, 0.0f } };
            registry.replace<Game::FighterState>(dummyEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                dummyEntity, Game::Health{ dummyDefinition.stats.maxHealth, dummyDefinition.stats.maxHealth });
            dummyBlocksHeld = false;
            dummyAiPreset = 0;
            dummyPunishCountdown = 0;
#if defined(OMD_PROTOTYPE)
            Game::ClearProjectiles(registry, projectileSystem);
            Game::ClearVfx(vfxSystem);
            sfxCues.OnRoundReset();
#endif
            // Also drop the last-resolved hit(s) - UpdateFighterState reacts to lastCollisionEvents
            // one tick late (see its own comment on why), so without this a hitbox that was still
            // overlapping the instant Reset was clicked would land one more hit right after the
            // health/state reset above, immediately re-damaging a freshly-restored fighter.
            lastCollisionEvents = Engine::CollisionEvents{};
            rebuildDrawItems();
        }
#else
        Renderer::RenderTasks::DoFrame(viewProjection, {}, {}, {}, true, nullptr, hudUI);
#endif

        // Encode + write any screenshot queued this frame (F9 or a scripted capture). No-op
        // unless FrameCapture::Grab was called; the readback it reads is valid now that
        // DoFrame's EndFrame has presented and waited on the GPU.
        Engine::FrameCapture::ResolvePending();

#ifdef OMD_DEV_TOOLS
        if (captureRun && captureRun->Finished(simClock.tickCount))
        {
            captureRun->Finish(); // writes motion.csv
            Foundation::Log::Write(Foundation::Log::Severity::Info, "Game", "--capture: run '%s' complete, exiting", captureRun->Name().c_str());
            break;
        }
#endif
    }

    Engine::Audio::Shutdown();
    Renderer::RenderTasks::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
