#include "Asset/GltfImporter.h"
#include "Engine/Animation.h"
#include "Engine/Camera.h"
#include "Engine/ClipPlayback.h"
#include "Engine/Collision.h"
#include "Engine/Components.h"
#include "Engine/Engine.h"
#include "Engine/FixedTimestep.h"
#include "Engine/Input.h"
#include "Engine/ModelResources.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"
#include "Renderer/Buffer.h"
#include "Renderer/DebugDrawPass.h"
#include "Renderer/RenderTasks.h"
#include "CombatDsl.h"
#include "FighterShadow.h"
#include "FighterState.h"
#include "GameConstants.h"
#include "GroundPlane.h"
#include "InputBindings.h"
#include "LocalTestScene.h"
#include "MoveTable.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DirectXMath.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <entt.hpp>
#include <filesystem>
#include <imgui.h>
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

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Foundation::CreateDebugConsole("OmdLab - Debug Console");

    Foundation::Log::Init("logs/omdlab.log");
    Foundation::Log::Write(Foundation::Log::Severity::Info, "Game", "OmdLab starting up");

    Engine::PrintDependencyChain();

    OMD_DEBUG_PRINT("Debug print smoke test, value = %d", 42);
    OMD_ASSERT(1 + 1 == 2, "Sanity check failed: math is broken");
    Game::CombatDsl::RunSelfTest();

    constexpr unsigned int windowWidth = 1280;
    constexpr unsigned int windowHeight = 720;

    HWND window = Foundation::CreateGameWindow("OmdLab", windowWidth, windowHeight);
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

    // This character's own moveset. modelDirectory duplicates the (still-hardcoded for now)
    // kCharacterDirectory constant below - collapsed into one source of truth once
    // CharacterDefinition actually drives character loading later in this same pass.
    Game::CharacterDefinition characterDefinition;
    characterDefinition.modelDirectory = "data/characters/polyone_stick_man";
    // Named (not just a load-site literal) since the hitbox-tuning debug tool's "Save" button
    // later needs the same path to write back to.
    constexpr const char* kMovesFilePath = "data/characters/polyone_stick_man/moves.combat";
    {
        Game::CombatDsl::CombatFile movesFile;
        const bool movesLoaded = Game::CombatDsl::LoadCombatFile(kMovesFilePath, movesFile);
        OMD_ASSERT(movesLoaded, "Failed to load moves.combat");
        const bool built = Game::BuildMoveTable(std::move(movesFile), characterDefinition.moveTable);
        OMD_ASSERT(built, "Failed to build MoveTable from moves.combat");
    }
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
    // local test scene below also goes through.
    constexpr const char* kCharacterDirectory = "data/characters/polyone_stick_man";
    Asset::Model characterModel;
    std::vector<Renderer::SkinnedMeshDrawItem> characterDrawItems;
    // The dummy fighter's own definition - a separate CharacterDefinition, not aliased to the
    // player's, since MoveSource::moveTable is a raw pointer into a CharacterDefinition's own
    // moveTable address; sharing the player's would point the dummy's MoveSource at the
    // player's real move table. moveTable is deliberately left empty (no moves.combat load) -
    // that's the entire mechanism behind "no AI, never attacks": SelectMove over an empty table
    // always returns nullopt, so Attack can never fire for this entity, with zero new branching
    // anywhere else.
    Game::CharacterDefinition dummyDefinition;
    std::vector<Renderer::SkinnedMeshDrawItem> dummyDrawItems;
    if (Asset::ImportGltf("data/characters/polyone_stick_man/StickMan.glb", characterModel))
    {
        // Correction for this specific source file: Sketchfab's FBX-to-glTF conversion wraps
        // the whole scene in a node (visible in the imported hierarchy as a node named after
        // the original FBX's hash) carrying a 0.01 unit-conversion scale, which collapses the
        // character down to world-space centimeter scale when imported standalone. Not
        // something Engine's generic connective resource layer can detect or correct on its
        // own (a legitimately tiny model is indistinguishable from this from the geometry
        // alone) - a caller-known correction for this asset, per rootTransform's own contract.
        // Stored on characterDefinition (not just a local here) so both FighterState.cpp's
        // bone-attached-hitbox lookup and the per-frame render update below (tick loop) read
        // the exact same source rather than a second hand-copied constant.
        DirectX::XMStoreFloat4x4(&characterDefinition.assetCorrection, DirectX::XMMatrixScaling(100.0f, 100.0f, 100.0f));
        // This character's own source asset was authored facing along Z (front/back toward a
        // camera positioned there), but every other system in this game (ground plane,
        // movement, the default camera) already assumes a 2D side view - camera looking down
        // +Z, X the screen-horizontal gameplay axis. Rotate 90 degrees so its profile faces
        // that camera instead - see CharacterDefinition::facingCorrectionRadians's own comment
        // for why this is a second, separate correction from assetCorrection above, not folded
        // into it (and why it's applied per-frame below rather than baked into rootTransform
        // here alongside assetCorrection).
        characterDefinition.facingCorrectionRadians = DirectX::XM_PIDIV2;

        // Only assetCorrection bakes into the vertex data here - the entity's own Transform is
        // still {0,0,0}/identity at this point (nothing has moved yet), and facingCorrection is
        // deliberately applied per-frame instead (see the render loop's own comment on why).
        const DirectX::XMMATRIX assetCorrection = DirectX::XMLoadFloat4x4(&characterDefinition.assetCorrection);
        characterDrawItems = Engine::CreateSkinnedMeshDrawItems(characterModel, kCharacterDirectory, assetCorrection);
        if (!characterDrawItems.empty())
        {
            registry.emplace<Engine::SkinnedRenderable>(characterEntity, Engine::SkinnedRenderable{ &characterDrawItems[0] });
        }

        // One-time name->index resolution for every hitbox's optional bone attachment - needs
        // the real imported model, so this is the earliest point it can run.
        Game::ResolveHitboxJoints(characterModel, characterDefinition.moveTable);

        // Dummy shares the player's own import quirks (same source asset) but gets its own
        // CharacterDefinition instance (see its own declaration above) and its own GPU-backed
        // draw item instance - CreateSkinnedMeshDrawItems's own persistent per-instance world/
        // bone-palette buffers are exactly why a second real fighter needs a second call here,
        // not a shared draw item (reusing one buffer across items was a real historical bug -
        // see StaticMeshDrawItem's own comment).
        dummyDefinition.stats = characterDefinition.stats;
        dummyDefinition.assetCorrection = characterDefinition.assetCorrection;
        dummyDefinition.facingCorrectionRadians = characterDefinition.facingCorrectionRadians;
        dummyDrawItems = Engine::CreateSkinnedMeshDrawItems(characterModel, kCharacterDirectory, assetCorrection);

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
            dummyDefinition.moveTable.moves.push_back(std::move(punishMove));
        }
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
    registry.emplace<Engine::ClipPlayback>(characterEntity);
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
            registry.get<Engine::ClipPlayback>(characterEntity).clipIndex = static_cast<int32_t>(i);
            break;
        }
    }

    // A first estimate of the character's body extents (feet at Transform.position, ~1.8-unit
    // standing height) - no per-state hurtbox profile exists yet, so this one box is always
    // active regardless of pose. Checked visually against the "Collision debug draw" toggle
    // below, not just guessed and left unverified.
    registry.emplace<Engine::Hurtbox>(
        characterEntity, Engine::Hurtbox{ { Engine::CollisionBox{ { 0.0f, 0.9f, 0.0f }, { 0.35f, 0.9f, 0.25f } } } });

    registry.emplace<Game::FighterState>(characterEntity);
    registry.emplace<Game::Health>(
        characterEntity, Game::Health{ characterDefinition.stats.maxHealth, characterDefinition.stats.maxHealth });
    registry.emplace<Game::MoveSource>(characterEntity, Game::MoveSource{ &characterDefinition.moveTable });

    // The dummy fighter - a real second entity (no AI, stands still, blocks only via a debug
    // checkbox below) to test the player's real moveset against a real Hurtbox, replacing the
    // old dev-only test-hitbox rig this same rig used to stand in for one. Real content, not
    // dev-only (see rebuildDrawItems below), same "always included" precedent groundPlaneDrawItems
    // already sets - so unlike the removed test rig, this is unconditional, not OMD_DEV_TOOLS-gated.
    const entt::entity dummyEntity = registry.create();
    registry.emplace<Engine::Transform>(dummyEntity, Engine::Transform{ { 2.0f, 0.0f, 0.0f } });
    if (!dummyDrawItems.empty())
    {
        registry.emplace<Engine::SkinnedRenderable>(dummyEntity, Engine::SkinnedRenderable{ &dummyDrawItems[0] });
    }
    registry.emplace<Engine::ClipPlayback>(dummyEntity);
    for (size_t i = 0; i < characterModel.clips.size(); ++i)
    {
        if (characterModel.clips[i].name == "idle")
        {
            registry.get<Engine::ClipPlayback>(dummyEntity).clipIndex = static_cast<int32_t>(i);
            break;
        }
    }
    registry.emplace<Engine::Hurtbox>(
        dummyEntity, Engine::Hurtbox{ { Engine::CollisionBox{ { 0.0f, 0.9f, 0.0f }, { 0.35f, 0.9f, 0.25f } } } });
    registry.emplace<Game::FighterState>(dummyEntity);
    registry.emplace<Game::Health>(dummyEntity, Game::Health{ dummyDefinition.stats.maxHealth, dummyDefinition.stats.maxHealth });
    // Never actually dereferenced (dummyDefinition.moveTable is always empty, so the dummy can
    // never be a HitEvent's attacker) - kept only for uniform assembly with every other fighter.
    registry.emplace<Game::MoveSource>(dummyEntity, Game::MoveSource{ &dummyDefinition.moveTable });

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
    std::vector<Renderer::StaticMeshDrawItem> groundPlaneDrawItems{ Game::CreateGroundPlaneDrawItem() };
    // One small "grounding" shadow quad per fighter - see FighterShadow.h's own comment for why
    // this is a flat opaque patch, not a real soft shadow. Repositioned every render frame
    // (below) to track each fighter's current X/Z - kept as separate persistent draw items
    // (own world buffers), same "each instance owns its own buffer" reasoning every other
    // per-instance draw item in this project already follows.
    Renderer::StaticMeshDrawItem playerShadowDrawItem = Game::CreateFighterShadowDrawItem();
    Renderer::StaticMeshDrawItem dummyShadowDrawItem = Game::CreateFighterShadowDrawItem();

    // The character (GPU-skinned) and local test scene/ground plane (rigid) are different draw
    // item types feeding different passes - StaticMeshPass/SkinnedMeshPass never learn a
    // "character"/"local test scene"/"ground plane" exists (the Renderer/Asset dependency rule),
    // each only ever sees its own flat draw item list. Which categories are actually included is
    // a Game-owned decision, re-applied whenever the debug checkboxes below change - cheap,
    // since the underlying GPU buffers/textures were already uploaded once above and this only
    // rebuilds the item lists.
    bool enableCharacter = true;
    auto rebuildDrawItems = [&]()
    {
        // Dummy's own draw items are concatenated in unconditionally - real content, like the
        // ground plane below, not gated behind enableCharacter (which stays player-only).
        std::vector<Renderer::SkinnedMeshDrawItem> skinnedItems;
        if (enableCharacter)
        {
            skinnedItems.insert(skinnedItems.end(), characterDrawItems.begin(), characterDrawItems.end());
        }
        skinnedItems.insert(skinnedItems.end(), dummyDrawItems.begin(), dummyDrawItems.end());
        Renderer::SkinnedMeshPass::SetDrawItems(skinnedItems);
        std::vector<Renderer::StaticMeshDrawItem> staticItems = groundPlaneDrawItems;
        // Same enableCharacter/unconditional split as the skinned items above - player's shadow
        // follows whether the player mesh itself is shown, dummy's shadow is always there.
        if (enableCharacter)
        {
            staticItems.push_back(playerShadowDrawItem);
        }
        staticItems.push_back(dummyShadowDrawItem);
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

    while (Foundation::PumpMessages())
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

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
        Engine::UpdateFreeFlyCamera(camera, window, deltaSeconds, allowCameraMouse, allowCameraKeyboard);
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
        const DirectX::XMFLOAT4X4 viewProjection = Engine::ComputeViewProjection(camera, aspectRatio);

        simClock.BeginFrame(deltaSeconds);
        while (simClock.TryConsumeTick())
        {
            // Facing - snap (not interpolated), computed once per tick from last tick's settled
            // positions, before either fighter's state machine runs below, so both the render
            // (updateFighterRender further down) and the bone-attached hitbox lookup
            // (FighterState.cpp's ResolveHitboxOffset) read this same tick's value. Whichever
            // fighter is more to the left faces right (toward the other) and vice versa - always
            // facing the opponent regardless of which way either is currently walking/attacking,
            // so crossups/jumping past an opponent don't leave anyone facing backward.
            {
                const float playerX = registry.get<Engine::Transform>(characterEntity).position.x;
                const float dummyX = registry.get<Engine::Transform>(dummyEntity).position.x;
                registry.get<Game::FighterState>(characterEntity).facingRight = playerX <= dummyX;
                registry.get<Game::FighterState>(dummyEntity).facingRight = dummyX <= playerX;
            }

            // See disableCharacterAnalogStickMovement's own declaration - unconditionally false
            // outside dev tools, so this always compiles to the existing behavior there.
#ifdef OMD_DEV_TOOLS
            const bool ignoreGamepadAnalogAxis = disableCharacterAnalogStickMovement;
#else
            const bool ignoreGamepadAnalogAxis = false;
#endif
            lastInputCommand = Engine::AssembleInputCommand(
                simClock.tickCount, fighterBindings, lastInputCommand, /*gamepadIndex*/ 0, ignoreGamepadAnalogAxis);
            playerInputHistory.Push(lastInputCommand);

            // Debug readout only for now (Phase 3) - only overwritten on an actual selection
            // (not cleared back to empty every tick that isn't one) so it shows "the last real
            // selection" long enough to see, rather than a value only ever true for one ~16ms
            // tick. UpdateFighterState below calls SelectMove again itself once move selection
            // actually becomes load-bearing (a later phase of this step) - harmless duplicate
            // work at this entity count, not worth threading the result through as a parameter.
            if (const std::optional<std::string> selected = Game::SelectMove(characterDefinition, playerInputHistory); selected.has_value())
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
            }

            // AI preset decision - see dummyAiPreset's own declaration for why this is a testing
            // convenience (a few hardcoded scripted reactions), not real AI. lastCollisionEvents
            // is still last tick's result at this point (ResolveCollisions runs after both
            // fighters' UpdateFighterState calls below) - the same one-tick-lag every other
            // reader of it in this loop already accepts.
            bool dummyBlockThisTick = false;
            bool dummyPunchThisTick = false;
            if (dummyAiPreset == 0) // Manual
            {
                dummyBlockThisTick = dummyBlocksHeld;
            }
            else if (dummyAiPreset == 1) // Always block
            {
                dummyBlockThisTick = true;
            }
            else // Block then punish
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
            }
            dummyInputHistory.Push(dummyInputCommand);
            Game::UpdateFighterState(
                registry, dummyEntity,
                Game::FighterUpdateInput{
                    dummyInputCommand, dummyInputHistory, lastCollisionEvents, sharedStates, dummyDefinition, characterModel.clips,
                    characterModel, /*forceShowAllHitboxes*/ false });

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
                const float minSeparation = Game::kFighterBodyHalfWidth * 2.0f;
                const float delta = dummyTransform.position.x - playerTransform.position.x;
                const float absDelta = delta >= 0.0f ? delta : -delta;
                if (absDelta < minSeparation)
                {
                    const float pushEach = (minSeparation - absDelta) * 0.5f;
                    const float pushDir = delta >= 0.0f ? 1.0f : -1.0f; // Dummy is at/after player's X.
                    dummyTransform.position.x += pushDir * pushEach;
                    playerTransform.position.x -= pushDir * pushEach;
                    // Re-clamp - pushing apart could in principle shove one past the stage edge
                    // if both were already hugging it when the overlap was resolved.
                    dummyTransform.position.x = std::clamp(dummyTransform.position.x, -Game::kStageHalfWidth, Game::kStageHalfWidth);
                    playerTransform.position.x = std::clamp(playerTransform.position.x, -Game::kStageHalfWidth, Game::kStageHalfWidth);
                }
            }

            // Deterministic, fixed-tick per the networking-readiness design - resolved here, not
            // once per render frame, even though nothing yet moves entities per tick (the debug
            // sliders below move them at render rate instead, so overlap color can lag a tick
            // behind a dragged slider - intentional, an honest side effect of ticks/frames being
            // decoupled, not a bug).
            lastCollisionEvents = Engine::ResolveCollisions(registry);
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
        auto updateFighterRender = [&](entt::entity entity, float facingCorrectionRadians, bool advanceManually)
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
            // it's pre-baked).
            {
                const DirectX::XMMATRIX facingCorrection = DirectX::XMMatrixRotationY(facingCorrectionRadians);
                const DirectX::XMMATRIX worldTransform = facingCorrection * Engine::ComputeWorldMatrix(registry.get<Engine::Transform>(entity));
                DirectX::XMFLOAT4X4 worldForGpu;
                DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(worldTransform));
                Renderer::Buffer::Update(drawItem.worldBuffer, &worldForGpu, sizeof(worldForGpu));
            }

            Engine::ClipPlayback& playback = registry.get<Engine::ClipPlayback>(entity);
            const Asset::Clip& clip = characterModel.clips[playback.clipIndex];
            // Manual override restores the pre-state-machine behavior for this entity only:
            // wall-clock Advance(), same as every other clip preview in this app - lets one clip
            // be eyeballed in isolation without the state machine (skipped above) setting
            // playbackTimeSeconds itself.
            if (advanceManually)
            {
                playback.Advance(deltaSeconds, clip.durationSeconds);
            }
            Engine::UpdateSkinnedPose(
                characterModel, *characterSkin, clip, playback.playbackTimeSeconds, DirectX::XMMatrixIdentity(),
                DirectX::XMMatrixIdentity(), drawItem);
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
        updateFighterRender(characterEntity, playerFacingRadians, manualClipOverride);
        updateFighterRender(dummyEntity, dummyFacingRadians, /*advanceManually*/ false);

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

#ifdef OMD_DEV_TOOLS
        // Rebuilt every render frame from the latest resolved tick's events (see above) - cheap
        // at this entity count. Purely feeds DebugDrawPass's visualization (itself dev-tools-
        // gated), so gated together rather than built and silently never rendered.
        std::vector<Renderer::DebugDrawLine> debugLines = Engine::BuildCollisionDebugLines(registry, lastCollisionEvents);
        Renderer::DebugDrawPass::SetLines(debugLines);
#endif

#ifdef OMD_DEV_TOOLS
        // Character is real, shipped content - shown above RenderTasks' own "Debug" section,
        // not inside it. Local test scene is dev-only content (see the "Bulk external test
        // content" working convention), grouped with RenderTasks' own bring-up toggles
        // instead - both land in the one "Renderer Debug" window regardless, since Renderer
        // just invokes whichever of these two callbacks was given at whichever point in that
        // window it doesn't itself know or care what they contain. Both lambdas (and the
        // DoFrame call passing them) only exist under OMD_DEV_TOOLS - there's no non-ImGui UI
        // system yet, so this whole window is dev tooling, not just its individual toggles.
        auto primaryContentUI = [&]()
        {
            if (ImGui::Checkbox("Character", &enableCharacter))
            {
                rebuildDrawItems();
            }
            if (!characterClipNames.empty())
            {
                // Off by default: UpdateFighterState (Idle/Walk/Run/Attack/.../KO) owns this
                // entity's clip during real play and would otherwise overwrite any manual pick
                // made below on the very next tick. Flip this on to preview one clip in
                // isolation, same workflow as before the state machine existed.
                ImGui::Checkbox("Manual clip override", &manualClipOverride);
                Engine::ClipPlayback& characterPlayback = registry.get<Engine::ClipPlayback>(characterEntity);
                int clipIndex = characterPlayback.clipIndex;
                ImGui::BeginDisabled(!manualClipOverride);
                if (ImGui::Combo("Clip", &clipIndex, characterClipNames.data(), static_cast<int>(characterClipNames.size())))
                {
                    characterPlayback.clipIndex = clipIndex;
                    characterPlayback.playbackTimeSeconds = 0.0f;
                }
                ImGui::Checkbox("Playing", &characterPlayback.playing);
                ImGui::EndDisabled();
            }
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

            // Visualizes the fixed-tick loop and the InputCommand it produces, since nothing
            // else consumes either yet. One line per concern to stay compact in this
            // already-tall debug window. Names here are Game's own (Engine's InputCommand has
            // no idea these slots mean "Jump"/"Punch"/etc. - see Engine/Input.h).
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
            static const char* kDummyAiPresetNames[] = { "Manual", "Always block", "Block then punish" };
            ImGui::Combo("Dummy AI", &dummyAiPreset, kDummyAiPresetNames, IM_ARRAYSIZE(kDummyAiPresetNames));
            ImGui::BeginDisabled(dummyAiPreset != 0);
            ImGui::Checkbox("Dummy blocks", &dummyBlocksHeld);
            ImGui::EndDisabled();

            // Combined with "Collision debug draw" above (RenderTasks' own toggle) - this alone
            // draws nothing, it just widens WHEN a move's hitbox exists so there's something to
            // see with that toggle on.
            ImGui::Checkbox("Force show all hitboxes (whole Attack state, not just active frames)", &forceShowAllHitboxes);

            // Hitbox authoring/adjustment tooling - see hitboxTuningMoveIndex's own
            // declaration for how the tick loop uses this. "None" (index 0 here, -1 in
            // hitboxTuningMoveIndex) hands the character back to real gameplay. Enable
            // "Collision debug draw" above to actually see the box while tuning.
            ImGui::SeparatorText("Hitbox tuning");
            {
                int hitboxTuningComboIndex = hitboxTuningMoveIndex + 1;
                if (ImGui::Combo(
                        "Move", &hitboxTuningComboIndex, hitboxTuningMoveNames.data(), static_cast<int>(hitboxTuningMoveNames.size())))
                {
                    hitboxTuningMoveIndex = hitboxTuningComboIndex - 1;
                    hitboxTuningFrameCounter = 0;
                    hitboxTuningBoxIndex = 0;
                    hitboxTuningSaveMessage.clear();
                }
                if (hitboxTuningMoveIndex >= 0 && hitboxTuningMoveIndex < static_cast<int>(characterDefinition.moveTable.moves.size()))
                {
                    Game::MoveDefinition& previewMove = characterDefinition.moveTable.moves[static_cast<size_t>(hitboxTuningMoveIndex)];

                    // A continuously-looping preview is hard to tune against - hold a specific
                    // keyframe still (Pause) and scrub to it directly (Frame) instead of chasing
                    // a moving target. The slider only accepts input while paused (BeginDisabled
                    // below) - unpaused, the tick loop's own auto-advance would just immediately
                    // overwrite a drag.
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
                            ImGui::SliderInt(
                                "Hitbox index", &hitboxTuningBoxIndex, 0, static_cast<int>(previewMove.hitboxes.size()) - 1);
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

        if (Renderer::RenderTasks::DoFrame(viewProjection, primaryContentUI, debugSectionUI))
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
            registry.get<Engine::Transform>(testTriggerEntity).position.x = -2.0f;
            // Also recovers the character's own fighter state/health - previously left out, which
            // meant landing in KO (whether from real play or the test-hitbox slider above) had no
            // UI-accessible way back to Idle short of restarting the app.
            registry.get<Engine::Transform>(characterEntity) = Engine::Transform{};
            registry.replace<Game::FighterState>(characterEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                characterEntity, Game::Health{ characterDefinition.stats.maxHealth, characterDefinition.stats.maxHealth });
            // Same recovery for the dummy - it can reach KO/HitStun exactly like the player can.
            registry.get<Engine::Transform>(dummyEntity) = Engine::Transform{ { 2.0f, 0.0f, 0.0f } };
            registry.replace<Game::FighterState>(dummyEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                dummyEntity, Game::Health{ dummyDefinition.stats.maxHealth, dummyDefinition.stats.maxHealth });
            dummyBlocksHeld = false;
            dummyAiPreset = 0;
            dummyPunishCountdown = 0;
            // Also drop the last-resolved hit(s) - UpdateFighterState reacts to lastCollisionEvents
            // one tick late (see its own comment on why), so without this a hitbox that was still
            // overlapping the instant Reset was clicked would land one more hit right after the
            // health/state reset above, immediately re-damaging a freshly-restored fighter.
            lastCollisionEvents = Engine::CollisionEvents{};
            rebuildDrawItems();
        }
#else
        Renderer::RenderTasks::DoFrame(viewProjection);
#endif
    }

    Renderer::RenderTasks::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
