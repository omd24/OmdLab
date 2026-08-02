#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <dxgiformat.h>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace Renderer
{
    // Shared with other dx12/ backend files (e.g. PipelineDX12) that need to
    // build resource descs against the actual swap chain format.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    struct DeviceDX12
    {
        static void Init(HWND window, unsigned int width, unsigned int height);
        static void Shutdown();
        static void BeginFrame();
        static void EndFrame();

        // Raw device pointer for other dx12/ backend files that need to
        // create GPU resources (PSOs, buffers, ...). Only valid between
        // Init() and Shutdown().
        static ID3D12Device* GetDevice();

        // The active frame's command list, for other dx12/ backend files
        // (render passes) that need to record draw/dispatch commands. Only
        // valid between BeginFrame() and EndFrame().
        static ID3D12GraphicsCommandList* GetCommandList();
    };
}
