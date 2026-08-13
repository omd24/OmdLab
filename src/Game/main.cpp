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
#include "Renderer/DebugDrawPass.h"
#include "Renderer/RenderTasks.h"
#include "CombatDsl.h"
#include "FighterState.h"
#include "GroundPlane.h"
#include "InputBindings.h"
#include "LocalTestScene.h"
#include "MoveTable.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DirectXMath.h>
#include <chrono>
#include <cstdio>
#include <entt.hpp>
#include <imgui.h>
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
    {
        Game::CombatDsl::CombatFile movesFile;
        const bool movesLoaded = Game::CombatDsl::LoadCombatFile("data/characters/polyone_stick_man/moves.combat", movesFile);
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
    if (Asset::ImportGltf("data/characters/polyone_stick_man/StickMan.glb", characterModel))
    {
        // Correction for this specific source file: Sketchfab's FBX-to-glTF conversion wraps
        // the whole scene in a node (visible in the imported hierarchy as a node named after
        // the original FBX's hash) carrying a 0.01 unit-conversion scale, which collapses the
        // character down to world-space centimeter scale when imported standalone. Not
        // something Engine's generic connective resource layer can detect or correct on its
        // own (a legitimately tiny model is indistinguishable from this from the geometry
        // alone) - a caller-known correction for this asset, per rootTransform's own contract.
        // Composed with the entity's own Transform (applied second, so the asset-specific
        // scale-fix always happens first regardless of where the entity is placed) - this is
        // what makes moving Transform.position actually move the rendered character, not just
        // add an inert component alongside the existing rendering path.
        const DirectX::XMMATRIX characterAssetCorrection = DirectX::XMMatrixScaling(100.0f, 100.0f, 100.0f);
        const DirectX::XMMATRIX characterRootTransform =
            characterAssetCorrection * Engine::ComputeWorldMatrix(registry.get<Engine::Transform>(characterEntity));
        characterDrawItems = Engine::CreateSkinnedMeshDrawItems(characterModel, kCharacterDirectory, characterRootTransform);
        if (!characterDrawItems.empty())
        {
            registry.emplace<Engine::SkinnedRenderable>(characterEntity, Engine::SkinnedRenderable{ &characterDrawItems[0] });
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

#ifdef OMD_DEV_TOOLS
    // Collision module test rig (no real per-move hitbox/state data exists yet - that arrives
    // with the content and state-machine steps) - hand-placed volumes purely to prove
    // Engine::ResolveCollisions/BuildCollisionDebugLines end to end. Positioned off to either
    // side by default (no overlap with the character's hurtbox at rest); dragged via the
    // "Debug" section's sliders below to make hit/trigger events fire live. Dev-only by
    // construction (there's no non-debug reason for these entities to exist), so gated entirely
    // rather than just hidden from a UI that also wouldn't exist.
    const entt::entity testHitboxEntity = registry.create();
    registry.emplace<Engine::Transform>(testHitboxEntity, Engine::Transform{ { 2.0f, 0.9f, 0.0f } });

    // A tiny throwaway MoveTable so this dev-only "attacker" drives the same real hit-resolution
    // path (damage/hitstun/block via MoveSource - see FighterState.h) a real fighter's hitbox
    // would, instead of a bespoke test-only code path in FighterState.cpp. Declared here (not a
    // temporary) so its address stays valid for MoveSource's raw pointer to reference for the
    // rest of the program, same lifetime pattern as characterDefinition above.
    Game::MoveTable devTestMoveTable;
    devTestMoveTable.moves.push_back(Game::MoveDefinition{});
    devTestMoveTable.moves[0].id = "dev_test_hit";
    devTestMoveTable.moves[0].damage = 15;
    devTestMoveTable.moves[0].onHitStunFrames = 20;
    devTestMoveTable.moves[0].onBlockStunFrames = 10;
    registry.emplace<Game::MoveSource>(testHitboxEntity, Game::MoveSource{ &devTestMoveTable });
    registry.emplace<Engine::Hitbox>(
        testHitboxEntity,
        Engine::Hitbox{ Engine::CollisionBox{ {}, { 0.2f, 0.2f, 0.2f } }, static_cast<uint32_t>(devTestMoveTable.IndexOf("dev_test_hit")) });

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
        Renderer::SkinnedMeshPass::SetDrawItems(enableCharacter ? characterDrawItems : std::vector<Renderer::SkinnedMeshDrawItem>{});
        std::vector<Renderer::StaticMeshDrawItem> staticItems = groundPlaneDrawItems;
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
#ifdef OMD_DEV_TOOLS
    // The free-fly camera is itself dev-only tooling (see Engine::Camera's own header comment -
    // no real fixed 2D game camera exists yet), so this toggle lives entirely under dev tools
    // too. On by default, matching existing behavior. Combined with ImGui's own
    // io.WantCaptureMouse below - the toggle covers "I want the camera locked regardless of
    // what's under the cursor," WantCaptureMouse covers the common case (dragging a slider or
    // clicking a checkbox) automatically, without needing to remember to flip this first.
    bool enableCameraMouseControl = true;
    // The free-fly camera's own gamepad move axis and the fighter's movement axis both read the
    // same physical left stick (Engine::GamepadAxis::LeftStickX) - moving the camera with a
    // gamepad inevitably also walks the character, which can walk it straight into the dev
    // test-hitbox rig (or off the stage edge) while someone's just trying to look around. On by
    // default is wrong (it would silently eat real gameplay input), so this stays off by
    // default and is purely an opt-in navigation aid.
    bool freezeCharacterMovement = false;
#endif
    // Fixed-timestep sim loop skeleton - no sim state exists yet to actually drive with this;
    // it proves ticks run at a fixed rate decoupled from render rate, and that a per-tick
    // InputCommand is produced and stored, ahead of a real fighter state machine becoming the
    // first real tick consumer.
    Engine::FixedTimestepAccumulator simClock;
    Engine::InputHistory playerInputHistory;
    Engine::InputCommand lastInputCommand;
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
    const Engine::InputBindings fighterBindings = Game::MakeDefaultFighterBindings();
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (Foundation::PumpMessages())
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

#ifdef OMD_DEV_TOOLS
        // io.WantCaptureMouse reflects the previous frame's ImGui state (this frame's NewFrame()
        // hasn't run yet - see RenderTasks::DoFrame) - a harmless one-frame lag, same as any
        // other engine using this exact pattern to stop UI clicks/drags from also driving camera
        // controls underneath the window.
        const bool allowCameraMouse = enableCameraMouseControl && !ImGui::GetIO().WantCaptureMouse;
#else
        const bool allowCameraMouse = true;
#endif
        Engine::UpdateFreeFlyCamera(camera, window, deltaSeconds, allowCameraMouse);
        const float aspectRatio = static_cast<float>(Renderer::Device::GetWidth()) / static_cast<float>(Renderer::Device::GetHeight());
        const DirectX::XMFLOAT4X4 viewProjection = Engine::ComputeViewProjection(camera, aspectRatio);

        simClock.BeginFrame(deltaSeconds);
        while (simClock.TryConsumeTick())
        {
            lastInputCommand = Engine::AssembleInputCommand(simClock.tickCount, fighterBindings, lastInputCommand, /*gamepadIndex*/ 0);
#ifdef OMD_DEV_TOOLS
            // See freezeCharacterMovement's own declaration - zeroed here, before push/use, so
            // every downstream consumer (SelectMove, UpdateFighterState, InputHistory itself)
            // consistently sees "no movement input" rather than each needing its own check.
            if (freezeCharacterMovement)
            {
                lastInputCommand.axis = 0.0f;
            }
#endif
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
            // Skipped entirely while manualClipOverride is on (see the Clip dropdown below) so
            // the state machine can't fight the manual clip selection - it would otherwise
            // overwrite ClipPlayback right back to whatever Idle/Walk/etc. currently means every
            // tick, which is exactly the "dropdown doesn't do anything" symptom this override
            // exists to fix.
            if (!manualClipOverride)
            {
                Game::UpdateFighterState(
                    registry, characterEntity,
                    Game::FighterUpdateInput{
                        lastInputCommand, playerInputHistory, lastCollisionEvents, sharedStates, characterDefinition,
                        characterModel.clips });
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
        if (characterSkin != nullptr && !characterModel.clips.empty() && registry.all_of<Engine::SkinnedRenderable>(characterEntity))
        {
            Engine::ClipPlayback& characterPlayback = registry.get<Engine::ClipPlayback>(characterEntity);
            const Asset::Clip& clip = characterModel.clips[characterPlayback.clipIndex];
            // Manual override restores the pre-state-machine behavior for this entity only:
            // wall-clock Advance(), same as every other clip preview in this app - lets one clip
            // be eyeballed in isolation without the state machine (skipped above) setting
            // playbackTimeSeconds itself.
            if (manualClipOverride)
            {
                characterPlayback.Advance(deltaSeconds, clip.durationSeconds);
            }
            Engine::UpdateSkinnedPose(
                characterModel, *characterSkin, clip, characterPlayback.playbackTimeSeconds, DirectX::XMMatrixIdentity(),
                DirectX::XMMatrixIdentity(), *registry.get<Engine::SkinnedRenderable>(characterEntity).drawItem);
        }

#ifdef OMD_DEV_TOOLS
        // Rebuilt every render frame from the latest resolved tick's events (see above) - cheap
        // at this entity count. Purely feeds DebugDrawPass's visualization (itself dev-tools-
        // gated), so gated together rather than built and silently never rendered.
        Renderer::DebugDrawPass::SetLines(Engine::BuildCollisionDebugLines(registry, lastCollisionEvents));
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
            if (ImGui::Checkbox("Local test scene", &enableLocalTestScene))
            {
                rebuildDrawItems();
            }

            // See allowCameraMouse above - this is the manual half of that decision (the
            // automatic half, io.WantCaptureMouse, needs no UI). Unrelated to the free-fly
            // camera's future fixed-2D-view toggle (not built yet - a future "which camera is
            // active" mode switch, not a mouse-specific concern like this one).
            ImGui::SeparatorText("Camera");
            ImGui::Checkbox("Camera mouse control", &enableCameraMouseControl);
            ImGui::Checkbox("Freeze character movement (gamepad camera nav)", &freezeCharacterMovement);

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

            // Collision module test rig - see the entity setup above for why these two entities
            // exist. Enable "Collision debug draw" above to see the boxes; drag these to make
            // the test hitbox overlap the character's hurtbox (turns red, Hits count increases)
            // or the test trigger overlap it (turns red, Triggers count increases).
            ImGui::SeparatorText("Collision (test rig)");
            ImGui::SliderFloat("Test hitbox X", &registry.get<Engine::Transform>(testHitboxEntity).position.x, -3.0f, 3.0f);
            ImGui::SliderFloat("Test trigger X", &registry.get<Engine::Transform>(testTriggerEntity).position.x, -3.0f, 3.0f);
            ImGui::Text("Hits: %zu  Triggers: %zu", lastCollisionEvents.hits.size(), lastCollisionEvents.triggers.size());
        };

        if (Renderer::RenderTasks::DoFrame(viewProjection, primaryContentUI, debugSectionUI))
        {
            camera = Engine::Camera{};
            enableCameraMouseControl = true;
            freezeCharacterMovement = false;
            enableCharacter = true;
            enableLocalTestScene = false;
            manualClipOverride = false;
            registry.get<Engine::Transform>(testHitboxEntity).position.x = 2.0f;
            registry.get<Engine::Transform>(testTriggerEntity).position.x = -2.0f;
            // Also recovers the character's own fighter state/health - previously left out, which
            // meant landing in KO (whether from real play or the test-hitbox slider above) had no
            // UI-accessible way back to Idle short of restarting the app.
            registry.get<Engine::Transform>(characterEntity) = Engine::Transform{};
            registry.replace<Game::FighterState>(characterEntity, Game::FighterState{});
            registry.replace<Game::Health>(
                characterEntity, Game::Health{ characterDefinition.stats.maxHealth, characterDefinition.stats.maxHealth });
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
