#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace Renderer
{
    struct ImGuiHelperDX12
    {
        static void Init(HWND window);
        static void Shutdown();
        static void NewFrame();
        static void Render();
    };
}
