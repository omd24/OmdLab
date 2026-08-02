#include "Engine/Engine.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"

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

    HWND window = Foundation::CreateGameWindow("OmdLab", 1280, 720);
    if (window == nullptr)
    {
        Foundation::Log::Shutdown();
        return -1;
    }

    while (Foundation::PumpMessages())
    {
        // Rendering comes later; idle loop for now.
    }

    Foundation::Log::Shutdown();
    return 0;
}
