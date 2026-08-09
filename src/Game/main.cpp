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
#include "InputBindings.h"
#include "LocalTestScene.h"

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

    constexpr unsigned int windowWidth = 1280;
    constexpr unsigned int windowHeight = 720;

    HWND window = Foundation::CreateGameWindow("OmdLab", windowWidth, windowHeight);
    if (window == nullptr)
    {
        Foundation::Log::Shutdown();
        return -1;
    }

    Renderer::RenderTasks::Init(window, windowWidth, windowHeight);

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
    // Debug-UI clip combo contents - built once since Asset::Clip::name (and characterModel
    // itself) don't change after import.
    std::vector<const char*> characterClipNames;
    for (const Asset::Clip& clip : characterModel.clips)
    {
        characterClipNames.push_back(clip.name.c_str());
    }
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

    // Collision module test rig (no real per-move hitbox/state data exists yet - that arrives
    // with the content and state-machine steps) - hand-placed volumes purely to prove
    // Engine::ResolveCollisions/BuildCollisionDebugLines end to end. Positioned off to either
    // side by default (no overlap with the character's hurtbox at rest); dragged via the
    // "Debug" section's sliders below to make hit/trigger events fire live.
    const entt::entity testHitboxEntity = registry.create();
    registry.emplace<Engine::Transform>(testHitboxEntity, Engine::Transform{ { 2.0f, 0.9f, 0.0f } });
    registry.emplace<Engine::Hitbox>(testHitboxEntity, Engine::Hitbox{ Engine::CollisionBox{ {}, { 0.2f, 0.2f, 0.2f } }, /*moveId*/ 1 });

    const entt::entity testTriggerEntity = registry.create();
    registry.emplace<Engine::Transform>(testTriggerEntity, Engine::Transform{ { -2.0f, 0.9f, 0.0f } });
    registry.emplace<Engine::TriggerVolume>(
        testTriggerEntity, Engine::TriggerVolume{ Engine::CollisionBox{ {}, { 0.5f, 1.0f, 0.5f } }, /*triggerId*/ 1 });

    // Dev-only stress-test content (see the "Bulk external test content" working convention) -
    // empty (and a no-op) when local/ isn't present.
    std::vector<Renderer::StaticMeshDrawItem> localTestSceneDrawItems = LocalTestScene::LoadIfAvailable();

    // The character (GPU-skinned) and local test scene (rigid) are different draw item types
    // feeding different passes - StaticMeshPass/SkinnedMeshPass never learn a "character" or
    // "local test scene" exists (the Renderer/Asset dependency rule), each only ever sees its
    // own flat draw item list. Which categories are actually included is a Game-owned decision,
    // re-applied whenever the debug checkboxes below change - cheap, since the underlying GPU
    // buffers/textures were already uploaded once above and this only rebuilds the item lists.
    bool enableCharacter = true;
    // Off by default - dev-only content (see the "Bulk external test content" working
    // convention), not something the default view should depend on.
    bool enableLocalTestScene = false;
    auto rebuildDrawItems = [&]()
    {
        Renderer::SkinnedMeshPass::SetDrawItems(enableCharacter ? characterDrawItems : std::vector<Renderer::SkinnedMeshDrawItem>{});
        Renderer::StaticMeshPass::SetDrawItems(enableLocalTestScene ? localTestSceneDrawItems : std::vector<Renderer::StaticMeshDrawItem>{});
    };
    rebuildDrawItems();

    Engine::Camera camera;
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
    const Engine::InputBindings fighterBindings = Game::MakeDefaultFighterBindings();
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (Foundation::PumpMessages())
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        Engine::UpdateFreeFlyCamera(camera, window, deltaSeconds);
        const float aspectRatio = static_cast<float>(Renderer::Device::GetWidth()) / static_cast<float>(Renderer::Device::GetHeight());
        const DirectX::XMFLOAT4X4 viewProjection = Engine::ComputeViewProjection(camera, aspectRatio);

        simClock.BeginFrame(deltaSeconds);
        while (simClock.TryConsumeTick())
        {
            lastInputCommand = Engine::AssembleInputCommand(simClock.tickCount, fighterBindings, lastInputCommand, /*gamepadIndex*/ 0);
            playerInputHistory.Push(lastInputCommand);
            // No sim state exists yet - a real fighter state machine will be the first real
            // tick consumer of playerInputHistory.

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
        if (characterSkin != nullptr && !characterModel.clips.empty() && registry.all_of<Engine::SkinnedRenderable>(characterEntity))
        {
            Engine::ClipPlayback& characterPlayback = registry.get<Engine::ClipPlayback>(characterEntity);
            const Asset::Clip& clip = characterModel.clips[characterPlayback.clipIndex];
            characterPlayback.Advance(deltaSeconds, clip.durationSeconds);
            Engine::UpdateSkinnedPose(
                characterModel, *characterSkin, clip, characterPlayback.playbackTimeSeconds, DirectX::XMMatrixIdentity(),
                DirectX::XMMatrixIdentity(), *registry.get<Engine::SkinnedRenderable>(characterEntity).drawItem);
        }

        // Rebuilt every render frame from the latest resolved tick's events (see above) - cheap
        // at this entity count, and DebugDrawPass::SetLines is a no-op-shaped call to make
        // regardless of whether the pass is actually toggled on (same "empty list draws nothing"
        // reasoning as StaticMeshPass/SkinnedMeshPass).
        Renderer::DebugDrawPass::SetLines(Engine::BuildCollisionDebugLines(registry, lastCollisionEvents));

        // Character is real, shipped content - shown above RenderTasks' own "Debug" section,
        // not inside it. Local test scene is dev-only content (see the "Bulk external test
        // content" working convention), grouped with RenderTasks' own bring-up toggles
        // instead - both land in the one "Renderer Debug" window regardless, since Renderer
        // just invokes whichever of these two callbacks was given at whichever point in that
        // window it doesn't itself know or care what they contain.
        auto primaryContentUI = [&]()
        {
            if (ImGui::Checkbox("Character", &enableCharacter))
            {
                rebuildDrawItems();
            }
            if (!characterClipNames.empty())
            {
                Engine::ClipPlayback& characterPlayback = registry.get<Engine::ClipPlayback>(characterEntity);
                int clipIndex = characterPlayback.clipIndex;
                if (ImGui::Combo("Clip", &clipIndex, characterClipNames.data(), static_cast<int>(characterClipNames.size())))
                {
                    characterPlayback.clipIndex = clipIndex;
                    characterPlayback.playbackTimeSeconds = 0.0f;
                }
                ImGui::Checkbox("Playing", &characterPlayback.playing);
            }
        };
        auto debugSectionUI = [&]()
        {
            if (ImGui::Checkbox("Local test scene", &enableLocalTestScene))
            {
                rebuildDrawItems();
            }

            // Visualizes the fixed-tick loop and the InputCommand it produces, since nothing
            // else consumes either yet. One line per concern to stay compact in this
            // already-tall debug window. Names here are Game's own (Engine's InputCommand has
            // no idea these slots mean "Jump"/"Punch"/etc. - see Engine/Input.h).
            static const char* kButtonNames[] = { "Jump", "Crouch", "Punch", "Kick", "Block" };
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
            enableCharacter = true;
            enableLocalTestScene = false;
            registry.get<Engine::Transform>(testHitboxEntity).position.x = 2.0f;
            registry.get<Engine::Transform>(testTriggerEntity).position.x = -2.0f;
            rebuildDrawItems();
        }
    }

    Renderer::RenderTasks::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
