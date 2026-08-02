#include "Engine/Engine.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"
#include "Renderer/RenderTasks.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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

    while (Foundation::PumpMessages())
    {
        Renderer::RenderTasks::DoFrame();
    }

    Renderer::RenderTasks::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
