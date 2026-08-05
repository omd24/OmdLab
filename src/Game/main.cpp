#include "Asset/GltfImporter.h"
#include "Engine/Engine.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"
#include "Renderer/RenderTasks.h"
#include "LocalTestScene.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <DirectXMath.h>
#include <chrono>
#include <cmath>

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

namespace
{
    // Temporary free-fly debug camera - lives here only until Engine's real
    // Camera system exists (see the camera ownership convention in
    // DESIGN_ARCHITECTURE.md). W/S forward/back, A/D strafe, Q/E up/down
    // (all relative to the current yaw/pitch), Left/Right arrow yaw around
    // the world up axis, Up/Down arrow pitch - added one axis at a time on
    // top of a verified translation-only baseline (see DESIGN_LOG.md), not
    // all at once.
    struct FreeFlyCamera
    {
        DirectX::XMFLOAT3 position = { 0.0f, 1.5f, -4.0f };
        float yaw = 0.0f;   // radians; 0 looks down +Z, rotates around world +Y
        float pitch = 0.0f; // radians; positive looks up
    };

    bool KeyDown(int virtualKey)
    {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    void UpdateFreeFlyCamera(FreeFlyCamera& camera, float deltaSeconds)
    {
        constexpr float moveUnitsPerSecond = 3.0f;
        constexpr float turnRadiansPerSecond = 1.5f;

        if (KeyDown(VK_LEFT))
        {
            camera.yaw -= turnRadiansPerSecond * deltaSeconds;
        }
        if (KeyDown(VK_RIGHT))
        {
            camera.yaw += turnRadiansPerSecond * deltaSeconds;
        }
        if (KeyDown(VK_UP))
        {
            camera.pitch += turnRadiansPerSecond * deltaSeconds;
        }
        if (KeyDown(VK_DOWN))
        {
            camera.pitch -= turnRadiansPerSecond * deltaSeconds;
        }
        // Clamped just short of straight up/down - forward and world-up become parallel
        // there, which degenerates XMMatrixLookToLH's internally-derived basis.
        constexpr float kMaxPitch = DirectX::XM_PIDIV2 - 0.01f;
        camera.pitch = camera.pitch < -kMaxPitch ? -kMaxPitch : (camera.pitch > kMaxPitch ? kMaxPitch : camera.pitch);

        const DirectX::XMVECTOR forward =
            DirectX::XMVectorSet(sinf(camera.yaw) * cosf(camera.pitch), sinf(camera.pitch), cosf(camera.yaw) * cosf(camera.pitch), 0.0f);
        const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, forward));

        const float moveStep = moveUnitsPerSecond * deltaSeconds;
        DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&camera.position);
        if (KeyDown('W'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(forward, moveStep));
        }
        if (KeyDown('S'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(forward, moveStep));
        }
        if (KeyDown('D'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(right, moveStep));
        }
        if (KeyDown('A'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(right, moveStep));
        }
        if (KeyDown('Q'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(up, moveStep));
        }
        if (KeyDown('E'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(up, moveStep));
        }
        DirectX::XMStoreFloat3(&camera.position, eye);
    }

    DirectX::XMFLOAT4X4 ComputeViewProjection(const FreeFlyCamera& camera, float aspectRatio)
    {
        using namespace DirectX;

        const XMVECTOR forward =
            XMVectorSet(sinf(camera.yaw) * cosf(camera.pitch), sinf(camera.pitch), cosf(camera.yaw) * cosf(camera.pitch), 0.0f);
        const XMVECTOR eye = XMLoadFloat3(&camera.position);
        const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspectRatio, 0.1f, 500.0f);

        // DirectXMath matrices are constructed for row-vector use (v' = v * M) and stored
        // row-major in memory. HLSL shaders declare this cbuffer's matrix as `row_major`
        // explicitly (see Triangle.hlsl/LitTextured.hlsl) - no default-packing guessing - so
        // the bytes below are read by the GPU exactly as laid out here, with no implicit
        // reinterpretation. Transposing once here, then using a matrix-first mul(M, vector)
        // in the shader, correctly reproduces v * M: mul(M^T, v) == (v * M) as a column result.
        XMFLOAT4X4 viewProjection;
        XMStoreFloat4x4(&viewProjection, XMMatrixTranspose(XMMatrixMultiply(view, projection)));
        return viewProjection;
    }
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    Foundation::CreateDebugConsole("OmdLab - Debug Console");

    Foundation::Log::Init("logs/omdlab.log");
    Foundation::Log::Write(Foundation::Log::Severity::Info, "Game", "OmdLab starting up");

    Engine::PrintDependencyChain();

    // Parse-and-log verification of the Asset importer - no rendering yet, and not routed
    // through Engine yet since there's no GPU-resource connective layer to route it through.
    Asset::Model polyOneStickManModel;
    Asset::ImportGltf("data/characters/polyone_stick_man/StickMan.glb", polyOneStickManModel);

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
    LocalTestScene::LoadIfAvailable();

    FreeFlyCamera camera;
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (Foundation::PumpMessages())
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        // GetAsyncKeyState reads global keyboard state regardless of which window has focus -
        // without this gate, held movement keys would keep driving the camera even while
        // e.g. alt-tabbed away or typing in another application.
        if (GetForegroundWindow() == window)
        {
            UpdateFreeFlyCamera(camera, deltaSeconds);
        }
        const float aspectRatio = static_cast<float>(Renderer::Device::GetWidth()) / static_cast<float>(Renderer::Device::GetHeight());
        const DirectX::XMFLOAT4X4 viewProjection = ComputeViewProjection(camera, aspectRatio);

        if (Renderer::RenderTasks::DoFrame(viewProjection))
        {
            camera = FreeFlyCamera{};
        }
    }

    Renderer::RenderTasks::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
