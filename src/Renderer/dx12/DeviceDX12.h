#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace Renderer
{
    struct DeviceDX12
    {
        static void Init(HWND window, unsigned int width, unsigned int height);
        static void Shutdown();
        static void BeginFrame();
        static void EndFrame();
    };
}
