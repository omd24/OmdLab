#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <dxgiformat.h>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace Renderer
{
    // Shared with other dx12/ backend files (e.g. PipelineDX12) that need to
    // build resource descs against the actual swap chain format.
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // Shared with PipelineDX12, which needs it for a depth-tested PSO's
    // DSVFormat to match the actual depth buffer.
    constexpr DXGI_FORMAT kDepthBufferFormat = DXGI_FORMAT_D32_FLOAT;

    struct DeviceDX12
    {
        static void Init(HWND window, unsigned int width, unsigned int height);
        static void Shutdown();

        // One frame's fixed recipe, in three stages (see RenderTasks for
        // where these get called in order):
        //
        // 1. BeginFrame() - resets command recording and binds the UAV
        //    descriptor heap, ready for a compute pass to dispatch into the
        //    offscreen compute target (D3D12 disallows UAV usage directly
        //    on swap chain back buffers, unlike D3D11).
        // 2. Either CompositeComputeTarget() - copies the compute target
        //    into the actual back buffer (no clear beforehand - the copy
        //    already filled every pixel) - or, if no compute pass ran this
        //    frame, ClearAndBindRenderTarget() instead, which clears
        //    directly. Either way, the back buffer ends up bound as the
        //    active render target with its viewport/scissor set, ready for
        //    a graphics draw on top.
        // 3. EndFrame() - transitions the back buffer to PRESENT, submits,
        //    and presents.
        static void BeginFrame();
        static void CompositeComputeTarget();
        static void ClearAndBindRenderTarget();
        static void EndFrame();

        // Raw device pointer for other dx12/ backend files that need to
        // create GPU resources (PSOs, buffers, ...). Only valid between
        // Init() and Shutdown().
        static ID3D12Device* GetDevice();

        // The active frame's command list, for other dx12/ backend files
        // (render passes) that need to record draw/dispatch commands. Only
        // valid between BeginFrame() and EndFrame().
        static ID3D12GraphicsCommandList* GetCommandList();

        // GPU descriptor handle (as a raw UINT64 - avoids needing the full
        // D3D12_GPU_DESCRIPTOR_HANDLE definition in this header) for the
        // offscreen compute target's UAV. Only valid between BeginFrame()
        // and CompositeComputeTarget().
        static unsigned long long GetComputeTargetUAV();

        static unsigned int GetWidth();
        static unsigned int GetHeight();

        // For other dx12/ backend files that need to queue their own GPU
        // work (e.g. ImGui's texture uploads). Only valid between Init()
        // and Shutdown().
        static ID3D12CommandQueue* GetCommandQueue();
    };
}
