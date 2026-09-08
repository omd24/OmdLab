#include "DeviceDX12.h"

#include "Foundation/Debug.h"
#include "Foundation/Log.h"

#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kBackBufferCount = 2;

    HWND g_window = nullptr;
    UINT g_width = 0;
    UINT g_height = 0;

    ComPtr<IDXGIFactory6> g_factory;
    ComPtr<ID3D12Device> g_device;
    ComPtr<ID3D12CommandQueue> g_commandQueue;
    ComPtr<IDXGISwapChain3> g_swapChain;

    ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
    UINT g_rtvDescriptorSize = 0;
    ComPtr<ID3D12Resource> g_backBuffers[kBackBufferCount];

    // Single depth buffer, not double-buffered - same reasoning as
    // g_computeTarget: every frame is fully waited on before the next
    // starts, so there's no concurrent-access risk to guard against.
    ComPtr<ID3D12Resource> g_depthBuffer;
    ComPtr<ID3D12DescriptorHeap> g_dsvHeap;

    // Offscreen UAV target for the compute dispatch - D3D12 disallows UAV
    // usage directly on swap chain back buffers, so a compute pass writes
    // here and CompositeComputeTarget() copies the result into the actual
    // back buffer. Single texture, not double-buffered like g_backBuffers:
    // every frame is already fully waited on (WaitForGpu()) before the
    // next one starts, so there's no concurrent-access risk.
    ComPtr<ID3D12Resource> g_computeTarget;
    ComPtr<ID3D12DescriptorHeap> g_uavHeap;

    ComPtr<ID3D12CommandAllocator> g_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> g_commandList;

    ComPtr<ID3D12Fence> g_fence;
    HANDLE g_fenceEvent = nullptr;
    UINT64 g_fenceValue = 0;

    // Dev capture (DeviceDX12::RequestCapture/TakeCapture). g_captureReadback is a
    // HEAP_TYPE_READBACK buffer the back buffer is CopyTextureRegion'd into during EndFrame,
    // grown on demand and reused. Not double-buffered: EndFrame's WaitForGpu() fully retires
    // the copy before anything reads it, same reasoning as g_depthBuffer/g_computeTarget.
    bool g_captureRequested = false;
    bool g_captureReady = false;
    ComPtr<ID3D12Resource> g_captureReadback;
    UINT64 g_captureReadbackBytes = 0;
    UINT g_captureRowPitch = 0;
    UINT g_captureWidth = 0;
    UINT g_captureHeight = 0;

    void CheckHr(HRESULT hr, const char* what)
    {
        OMD_ASSERT(SUCCEEDED(hr), "%s failed with HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandleFor(UINT backBufferIndex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(backBufferIndex) * g_rtvDescriptorSize;
        return handle;
    }

    void TransitionResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_commandList->ResourceBarrier(1, &barrier);
    }

    // Records a copy of the back buffer into g_captureReadback, for DeviceDX12::TakeCapture to
    // read on the CPU. Called from EndFrame while the back buffer is still RENDER_TARGET (before
    // its PRESENT transition); the copy rides the same command-list submission and is covered by
    // EndFrame's WaitForGpu(). The readback buffer's row pitch is aligned to
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (GetCopyableFootprints handles that) - TakeCapture
    // repacks to a tight width*4 stride.
    void RecordBackBufferCapture(ID3D12Resource* backBuffer)
    {
        const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 totalBytes = 0;
        g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &totalBytes);

        if (!g_captureReadback || g_captureReadbackBytes < totalBytes)
        {
            g_captureReadback.Reset();
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;

            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = totalBytes;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            CheckHr(
                g_device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&g_captureReadback)),
                "CreateCommittedResource (capture readback)");
            g_captureReadbackBytes = totalBytes;
        }

        TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = g_captureReadback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = backBuffer;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        g_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        TransitionResource(backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

        g_captureRowPitch = footprint.Footprint.RowPitch;
        g_captureWidth = footprint.Footprint.Width;
        g_captureHeight = footprint.Footprint.Height;
        g_captureReady = true;
    }

    // Shared by CompositeComputeTarget()/ClearAndBindRenderTarget() - both
    // end with the back buffer already transitioned into
    // D3D12_RESOURCE_STATE_RENDER_TARGET and just need it bound. The depth
    // buffer is cleared and bound unconditionally alongside it, regardless
    // of which of the two callers ran - cheap, and keeps it in a known-clear
    // state for whatever draws happen this frame, the same reasoning
    // already applied to viewport/scissor below.
    void BindBackBufferAsRenderTarget(UINT backBufferIndex)
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = RtvHandleFor(backBufferIndex);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
        g_commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        // Required every time the command list records draws - the
        // rasterizer clips away all geometry without an explicit
        // viewport/scissor rect, regardless of the render target's own
        // size (this has no default).
        const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(g_width), static_cast<float>(g_height), 0.0f, 1.0f };
        const D3D12_RECT scissorRect = { 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
        g_commandList->RSSetViewports(1, &viewport);
        g_commandList->RSSetScissorRects(1, &scissorRect);
    }

    // Fully synchronous: every frame waits for the GPU to finish before
    // returning. Correct and simple.
    //
    // TODO(OM): pipeline multiple frames in flight once performance
    // actually demands it.
    void WaitForGpu()
    {
        const UINT64 valueToWaitFor = ++g_fenceValue;
        CheckHr(g_commandQueue->Signal(g_fence.Get(), valueToWaitFor), "ID3D12CommandQueue::Signal");

        if (g_fence->GetCompletedValue() < valueToWaitFor)
        {
            CheckHr(g_fence->SetEventOnCompletion(valueToWaitFor, g_fenceEvent), "ID3D12Fence::SetEventOnCompletion");
            WaitForSingleObject(g_fenceEvent, INFINITE);
        }
    }

    // Shared by Init() and ResizeIfNeeded() - fetches the swap chain's
    // current back buffer resources and (re)creates their RTVs.
    void CreateBackBuffersAndRtvs()
    {
        for (UINT i = 0; i < kBackBufferCount; ++i)
        {
            CheckHr(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i])), "IDXGISwapChain::GetBuffer");
            g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, RtvHandleFor(i));
        }
    }

    // Shared by Init() and ResizeIfNeeded() - (re)creates the offscreen
    // compute target at the given size and points the existing UAV heap
    // slot at it.
    void CreateComputeTargetAndUav(UINT width, UINT height)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = width;
        resourceDesc.Height = height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = Renderer::kBackBufferFormat;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CheckHr(
            g_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&g_computeTarget)),
            "ID3D12Device::CreateCommittedResource (compute target)");

        g_device->CreateUnorderedAccessView(g_computeTarget.Get(), nullptr, nullptr, g_uavHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Shared by Init() and ResizeIfNeeded() - (re)creates the depth buffer
    // at the given size and points the existing DSV heap slot at it.
    void CreateDepthBufferAndDsv(UINT width, UINT height)
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = width;
        resourceDesc.Height = height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = Renderer::kDepthBufferFormat;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = Renderer::kDepthBufferFormat;
        clearValue.DepthStencil.Depth = 1.0f;

        CheckHr(
            g_device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
                IID_PPV_ARGS(&g_depthBuffer)),
            "ID3D12Device::CreateCommittedResource (depth buffer)");

        g_device->CreateDepthStencilView(g_depthBuffer.Get(), nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Polled once per frame from BeginFrame() rather than driven by a
    // WM_SIZE hook - GetClientRect is cheap, and this avoids adding a
    // second Foundation::Window hook alongside the message hook ImGui
    // already uses. Skips zero-size (minimized) and no-op resizes.
    void ResizeIfNeeded()
    {
        RECT clientRect = {};
        GetClientRect(g_window, &clientRect);
        const UINT newWidth = static_cast<UINT>(clientRect.right - clientRect.left);
        const UINT newHeight = static_cast<UINT>(clientRect.bottom - clientRect.top);

        if (newWidth == 0 || newHeight == 0 || (newWidth == g_width && newHeight == g_height))
        {
            return;
        }

        WaitForGpu();

        for (UINT i = 0; i < kBackBufferCount; ++i)
        {
            g_backBuffers[i].Reset();
        }
        g_computeTarget.Reset();
        g_depthBuffer.Reset();

        CheckHr(
            g_swapChain->ResizeBuffers(kBackBufferCount, newWidth, newHeight, Renderer::kBackBufferFormat, 0), "IDXGISwapChain::ResizeBuffers");

        CreateBackBuffersAndRtvs();
        CreateComputeTargetAndUav(newWidth, newHeight);
        CreateDepthBufferAndDsv(newWidth, newHeight);

        g_width = newWidth;
        g_height = newHeight;

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Resized to %ux%u", newWidth, newHeight);
    }
}

namespace Renderer
{
    void DeviceDX12::Init(HWND window, unsigned int width, unsigned int height)
    {
        g_window = window;
        g_width = width;
        g_height = height;

#if defined(OMD_DEBUG)
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            {
                debugController->EnableDebugLayer();
            }
        }
#endif

        UINT factoryFlags = 0;
#if defined(OMD_DEBUG)
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        CheckHr(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&g_factory)), "CreateDXGIFactory2");

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0;
             g_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
            {
                char nameBuffer[128];
                size_t convertedChars = 0;
                wcstombs_s(&convertedChars, nameBuffer, sizeof(nameBuffer), desc.Description, _TRUNCATE);
                Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Selected adapter: %s", nameBuffer);
                break;
            }
        }
        OMD_ASSERT(g_device != nullptr, "No suitable D3D12 adapter found");

        // Agility SDK verification: D3D12SDKVersion/D3D12SDKPath (exported from
        // Game/main.cpp) tell the loader where to look, but a missing/mismatched
        // redist silently falls back to the OS-inbox runtime instead of failing -
        // so log which D3D12Core.dll actually got loaded into the process.
        if (HMODULE d3d12Core = GetModuleHandleA("D3D12Core.dll"))
        {
            char corePath[MAX_PATH] = {};
            GetModuleFileNameA(d3d12Core, corePath, static_cast<DWORD>(sizeof(corePath)));
            Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "D3D12 runtime: %s", corePath);
        }
        else
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Warning, "Renderer", "D3D12Core.dll not found as a loaded module - Agility SDK may not be active");
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        CheckHr(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue)), "ID3D12Device::CreateCommandQueue");

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = kBackBufferFormat;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = kBackBufferCount;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        CheckHr(
            g_factory->CreateSwapChainForHwnd(g_commandQueue.Get(), window, &swapChainDesc, nullptr, nullptr, &swapChain1),
            "IDXGIFactory6::CreateSwapChainForHwnd");
        CheckHr(swapChain1.As(&g_swapChain), "IDXGISwapChain1::As<IDXGISwapChain3>");

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = kBackBufferCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        CheckHr(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)), "ID3D12Device::CreateDescriptorHeap (RTV)");
        g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        CreateBackBuffersAndRtvs();

        D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
        uavHeapDesc.NumDescriptors = 1;
        uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CheckHr(g_device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&g_uavHeap)), "ID3D12Device::CreateDescriptorHeap (UAV)");
        CreateComputeTargetAndUav(width, height);

        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        CheckHr(g_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_dsvHeap)), "ID3D12Device::CreateDescriptorHeap (DSV)");
        CreateDepthBufferAndDsv(width, height);

        CheckHr(
            g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocator)),
            "ID3D12Device::CreateCommandAllocator");
        CheckHr(
            g_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList)),
            "ID3D12Device::CreateCommandList");
        CheckHr(g_commandList->Close(), "ID3D12GraphicsCommandList::Close");

        CheckHr(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "ID3D12Device::CreateFence");
        g_fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        OMD_ASSERT(g_fenceEvent != nullptr, "CreateEventA failed for the frame fence");

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Renderer", "DX12 device initialized (%ux%u, %u back buffers)", width, height, kBackBufferCount);
    }

    void DeviceDX12::Shutdown()
    {
        WaitForGpu();
        CloseHandle(g_fenceEvent);
    }

    void DeviceDX12::BeginFrame()
    {
        ResizeIfNeeded();

        CheckHr(g_commandAllocator->Reset(), "ID3D12CommandAllocator::Reset");
        CheckHr(g_commandList->Reset(g_commandAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");

        // g_computeTarget is always left in UNORDERED_ACCESS state by
        // CompositeComputeTarget() (or its initial creation state, on the
        // first frame) - ready to write without a transition here.
        ID3D12DescriptorHeap* heaps[] = { g_uavHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, heaps);
    }

    void DeviceDX12::CompositeComputeTarget()
    {
        const UINT backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = g_backBuffers[backBufferIndex].Get();

        TransitionResource(g_computeTarget.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        TransitionResource(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

        g_commandList->CopyResource(backBuffer, g_computeTarget.Get());

        TransitionResource(backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Back to UNORDERED_ACCESS so next frame's BeginFrame() can write
        // to it again without needing its own transition.
        TransitionResource(g_computeTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        BindBackBufferAsRenderTarget(backBufferIndex);
    }

    void DeviceDX12::ClearAndBindRenderTarget()
    {
        const UINT backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = g_backBuffers[backBufferIndex].Get();

        TransitionResource(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const float clearColor[4] = { 0.05f, 0.32f, 0.5f, 1.0f };
        g_commandList->ClearRenderTargetView(RtvHandleFor(backBufferIndex), clearColor, 0, nullptr);

        BindBackBufferAsRenderTarget(backBufferIndex);
    }

    void DeviceDX12::EndFrame()
    {
        const UINT backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = g_backBuffers[backBufferIndex].Get();

        // Dev capture: while the back buffer is still RENDER_TARGET, record a readback copy of
        // the finished frame. TakeCapture() reads it after this function's WaitForGpu().
        if (g_captureRequested)
        {
            g_captureRequested = false;
            RecordBackBufferCapture(backBuffer);
        }

        TransitionResource(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        CheckHr(g_commandList->Close(), "ID3D12GraphicsCommandList::Close");

        ID3D12CommandList* commandLists[] = { g_commandList.Get() };
        g_commandQueue->ExecuteCommandLists(1, commandLists);

        CheckHr(g_swapChain->Present(1, 0), "IDXGISwapChain::Present");

        WaitForGpu();
    }

    ID3D12Device* DeviceDX12::GetDevice()
    {
        return g_device.Get();
    }

    ID3D12GraphicsCommandList* DeviceDX12::GetCommandList()
    {
        return g_commandList.Get();
    }

    unsigned long long DeviceDX12::GetComputeTargetUAV()
    {
        return g_uavHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    }

    unsigned int DeviceDX12::GetWidth()
    {
        return g_width;
    }

    unsigned int DeviceDX12::GetHeight()
    {
        return g_height;
    }

    ID3D12CommandQueue* DeviceDX12::GetCommandQueue()
    {
        return g_commandQueue.Get();
    }

    void DeviceDX12::RequestCapture()
    {
        g_captureRequested = true;
    }

    bool DeviceDX12::TakeCapture(std::vector<unsigned char>& outRgba, unsigned& outWidth, unsigned& outHeight)
    {
        if (!g_captureReady)
        {
            return false;
        }
        g_captureReady = false;

        const SIZE_T mappedBytes = static_cast<SIZE_T>(g_captureRowPitch) * g_captureHeight;
        const D3D12_RANGE readRange = { 0, mappedBytes };
        void* mapped = nullptr;
        CheckHr(g_captureReadback->Map(0, &readRange, &mapped), "Map (capture readback)");

        outWidth = g_captureWidth;
        outHeight = g_captureHeight;
        const size_t tightPitch = static_cast<size_t>(outWidth) * 4;
        outRgba.resize(tightPitch * outHeight);
        const auto* srcBytes = static_cast<const unsigned char*>(mapped);
        for (unsigned y = 0; y < outHeight; ++y)
        {
            std::memcpy(outRgba.data() + y * tightPitch, srcBytes + static_cast<size_t>(y) * g_captureRowPitch, tightPitch);
        }

        const D3D12_RANGE writtenNothing = { 0, 0 };
        g_captureReadback->Unmap(0, &writtenNothing);
        return true;
    }
}
