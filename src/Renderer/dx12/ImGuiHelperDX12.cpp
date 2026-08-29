#include "ImGuiHelperDX12.h"

#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Foundation/Window.h"

#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

// Not declared in imgui_impl_win32.h (to avoid dragging <windows.h> into a
// header meant to be includable without it) - forward-declared here exactly
// as that header's own comment instructs.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;

namespace
{
    // ImGui may allocate more than one SRV descriptor (font atlas plus any
    // dynamic textures) - a small free-list over its own heap rather than a
    // single fixed descriptor.
    constexpr UINT kSrvHeapCapacity = 64;

    ComPtr<ID3D12DescriptorHeap> g_srvHeap;
    UINT g_srvDescriptorSize = 0;
    std::vector<UINT> g_freeSrvIndices;

    void CheckHr(HRESULT hr, const char* what)
    {
        OMD_ASSERT(SUCCEEDED(hr), "%s failed with HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
    }

    void AllocSrvDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        OMD_ASSERT(!g_freeSrvIndices.empty(), "ImGui SRV descriptor heap exhausted (capacity %u)", kSrvHeapCapacity);
        const UINT index = g_freeSrvIndices.back();
        g_freeSrvIndices.pop_back();

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<SIZE_T>(index) * g_srvDescriptorSize;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle.ptr += static_cast<UINT64>(index) * g_srvDescriptorSize;

        *outCpuHandle = cpuHandle;
        *outGpuHandle = gpuHandle;
    }

    void FreeSrvDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        const SIZE_T heapStart = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        const UINT index = static_cast<UINT>((cpuHandle.ptr - heapStart) / g_srvDescriptorSize);
        g_freeSrvIndices.push_back(index);
    }
}

namespace Renderer
{
    void ImGuiHelperDX12::Init(HWND window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsDark();

        ImGui_ImplWin32_Init(window);
        Foundation::SetWindowMessageHook(ImGui_ImplWin32_WndProcHandler);

        ID3D12Device* device = DeviceDX12::GetDevice();

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = kSrvHeapCapacity;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CheckHr(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap)), "ID3D12Device::CreateDescriptorHeap (ImGui SRV)");
        g_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        g_freeSrvIndices.resize(kSrvHeapCapacity);
        for (UINT i = 0; i < kSrvHeapCapacity; ++i)
        {
            g_freeSrvIndices[i] = kSrvHeapCapacity - 1 - i;
        }

        ImGui_ImplDX12_InitInfo initInfo = {};
        initInfo.Device = device;
        initInfo.CommandQueue = DeviceDX12::GetCommandQueue();
        // 1, not the back buffer count: every frame is already fully waited
        // on (see DeviceDX12's WaitForGpu TODO) before the next one starts,
        // so there is never more than one frame's resources in flight.
        initInfo.NumFramesInFlight = 1;
        initInfo.RTVFormat = kBackBufferFormat;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = g_srvHeap.Get();
        initInfo.SrvDescriptorAllocFn = AllocSrvDescriptor;
        initInfo.SrvDescriptorFreeFn = FreeSrvDescriptor;
        ImGui_ImplDX12_Init(&initInfo);

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "ImGui initialized");
    }

    void ImGuiHelperDX12::Shutdown()
    {
        Foundation::SetWindowMessageHook(nullptr);
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_srvHeap.Reset();
    }

    void ImGuiHelperDX12::NewFrame()
    {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiHelperDX12::Render()
    {
        ImGui::Render();

        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
    }
}
