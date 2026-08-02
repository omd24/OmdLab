#include "DeviceDX12.h"

#include "Foundation/Debug.h"
#include "Foundation/Log.h"

#include <cstdlib>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr UINT kBackBufferCount = 2;
    constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    ComPtr<IDXGIFactory6> g_factory;
    ComPtr<ID3D12Device> g_device;
    ComPtr<ID3D12CommandQueue> g_commandQueue;
    ComPtr<IDXGISwapChain3> g_swapChain;

    ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
    UINT g_rtvDescriptorSize = 0;
    ComPtr<ID3D12Resource> g_backBuffers[kBackBufferCount];

    ComPtr<ID3D12CommandAllocator> g_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> g_commandList;

    ComPtr<ID3D12Fence> g_fence;
    HANDLE g_fenceEvent = nullptr;
    UINT64 g_fenceValue = 0;

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

    void TransitionBackBuffer(ID3D12Resource* backBuffer, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = backBuffer;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_commandList->ResourceBarrier(1, &barrier);
    }

    // Fully synchronous for now: every frame waits for the GPU to finish
    // before returning. Correct and simple; multi-frame pipelining is a
    // later optimization once it actually matters for performance.
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
}

namespace Renderer
{
    void DeviceDX12::Init(HWND window, unsigned int width, unsigned int height)
    {
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

        for (UINT i = 0; i < kBackBufferCount; ++i)
        {
            CheckHr(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_backBuffers[i])), "IDXGISwapChain::GetBuffer");
            g_device->CreateRenderTargetView(g_backBuffers[i].Get(), nullptr, RtvHandleFor(i));
        }

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
        CheckHr(g_commandAllocator->Reset(), "ID3D12CommandAllocator::Reset");
        CheckHr(g_commandList->Reset(g_commandAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset");

        const UINT backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = g_backBuffers[backBufferIndex].Get();

        TransitionBackBuffer(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = RtvHandleFor(backBufferIndex);
        const float clearColor[4] = { 0.05f, 0.32f, 0.5f, 1.0f };
        g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
        g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    }

    void DeviceDX12::EndFrame()
    {
        const UINT backBufferIndex = g_swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = g_backBuffers[backBufferIndex].Get();

        TransitionBackBuffer(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        CheckHr(g_commandList->Close(), "ID3D12GraphicsCommandList::Close");

        ID3D12CommandList* commandLists[] = { g_commandList.Get() };
        g_commandQueue->ExecuteCommandLists(1, commandLists);

        CheckHr(g_swapChain->Present(1, 0), "IDXGISwapChain::Present");

        WaitForGpu();
    }
}
