#include "Asset/GltfImporter.h"
#include "Engine/Camera.h"
#include "Engine/Engine.h"
#include "Engine/ModelResources.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"
#include "Renderer/RenderTasks.h"
#include "LocalTestScene.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DirectXMath.h>
#include <chrono>
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

    // Character asset, in bind pose (no animation yet - see the incremental plan's step 11).
    // Engine's connective resource layer does the Asset-CPU-data-to-Renderer-GPU-resource
    // translation, the same generic path the local test scene below also goes through.
    constexpr const char* kCharacterDirectory = "data/characters/polyone_stick_man";
    Asset::Model characterModel;
    std::vector<Renderer::StaticMeshDrawItem> characterDrawItems;
    if (Asset::ImportGltf("data/characters/polyone_stick_man/StickMan.glb", characterModel))
    {
        // Correction for this specific source file: Sketchfab's FBX-to-glTF conversion wraps
        // the whole scene in a node (visible in the imported hierarchy as a node named after
        // the original FBX's hash) carrying a 0.01 unit-conversion scale, which collapses the
        // character down to world-space centimeter scale when imported standalone. Not
        // something Engine's generic connective resource layer can detect or correct on its
        // own (a legitimately tiny model is indistinguishable from this from the geometry
        // alone) - a caller-known correction for this asset, per rootTransform's own contract.
        const DirectX::XMMATRIX characterRootTransform = DirectX::XMMatrixScaling(100.0f, 100.0f, 100.0f);
        characterDrawItems = Engine::CreateStaticMeshDrawItems(characterModel, kCharacterDirectory, characterRootTransform);
    }

    // Dev-only stress-test content (see the "Bulk external test content" working convention) -
    // empty (and a no-op) when local/ isn't present.
    std::vector<Renderer::StaticMeshDrawItem> localTestSceneDrawItems = LocalTestScene::LoadIfAvailable();

    // Both categories feed the one shared Renderer::StaticMeshPass draw item list -
    // StaticMeshPass itself never learns a "character" or "local test scene" exists (the
    // Renderer/Asset dependency rule), it only ever sees the combined StaticMeshDrawItem
    // list. Which categories are actually included is a Game-owned decision, re-applied
    // whenever the debug checkboxes below change - cheap, since the underlying GPU buffers/
    // textures were already uploaded once above and this only rebuilds the item list.
    bool enableCharacter = true;
    // Off by default - dev-only content (see the "Bulk external test content" working
    // convention), not something the default view should depend on.
    bool enableLocalTestScene = false;
    auto rebuildDrawItems = [&]()
    {
        std::vector<Renderer::StaticMeshDrawItem> combined;
        if (enableCharacter)
        {
            combined.insert(combined.end(), characterDrawItems.begin(), characterDrawItems.end());
        }
        if (enableLocalTestScene)
        {
            combined.insert(combined.end(), localTestSceneDrawItems.begin(), localTestSceneDrawItems.end());
        }
        Renderer::StaticMeshPass::SetDrawItems(combined);
    };
    rebuildDrawItems();

    Engine::Camera camera;
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (Foundation::PumpMessages())
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        Engine::UpdateFreeFlyCamera(camera, window, deltaSeconds);
        const float aspectRatio = static_cast<float>(Renderer::Device::GetWidth()) / static_cast<float>(Renderer::Device::GetHeight());
        const DirectX::XMFLOAT4X4 viewProjection = Engine::ComputeViewProjection(camera, aspectRatio);

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
        };
        auto debugSectionUI = [&]()
        {
            if (ImGui::Checkbox("Local test scene", &enableLocalTestScene))
            {
                rebuildDrawItems();
            }
        };

        if (Renderer::RenderTasks::DoFrame(viewProjection, primaryContentUI, debugSectionUI))
        {
            camera = Engine::Camera{};
            enableCharacter = true;
            enableLocalTestScene = false;
            rebuildDrawItems();
        }
    }

    Renderer::RenderTasks::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
