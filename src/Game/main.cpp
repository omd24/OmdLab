#include "Engine/Engine.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"
#include "Renderer/Device.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

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

    Renderer::Device::Init(window, windowWidth, windowHeight);

    while (Foundation::PumpMessages())
    {
        Renderer::Device::BeginFrame();
        Renderer::Device::EndFrame();
    }

    Renderer::Device::Shutdown();
    Foundation::Log::Shutdown();
    return 0;
}
