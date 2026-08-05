#include "TextureDX12.h"

#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"

#include <cstring>
#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    // Fixed-size, bump-allocated heap - textures are created once (at load time) and live
    // for the rest of the process, same "no individual release" lifetime as Buffer/Pipeline,
    // so no free-list is needed (unlike ImGui's own SRV heap, which really does allocate and
    // free individual descriptors as ImGui textures come and go).
    constexpr UINT kMaxTextures = 256;
    constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::vector<ComPtr<ID3D12Resource>> g_textures;
    ComPtr<ID3D12DescriptorHeap> g_srvHeap;
    UINT g_srvDescriptorSize = 0;

    // A dedicated one-shot upload command list/fence, separate from Device's per-frame one.
    // Textures are created during Init() (via a pass's Init(), before the frame loop's first
    // BeginFrame()), when Device's shared command list is closed and not valid to record
    // into - see DeviceDX12::GetCommandList()'s own doc comment. Reused (reset) across calls
    // rather than torn down and rebuilt each time.
    ComPtr<ID3D12CommandAllocator> g_uploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> g_uploadCommandList;
    ComPtr<ID3D12Fence> g_uploadFence;
    HANDLE g_uploadFenceEvent = nullptr;
    UINT64 g_uploadFenceValue = 0;

    void CheckHr(HRESULT hr, const char* what)
    {
        OMD_ASSERT(SUCCEEDED(hr), "%s failed with HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
    }

    void EnsureSrvHeap(ID3D12Device* device)
    {
        if (g_srvHeap != nullptr)
        {
            return;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = kMaxTextures;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        CheckHr(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&g_srvHeap)), "ID3D12Device::CreateDescriptorHeap (texture SRV)");
        g_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void EnsureUploadResources(ID3D12Device* device)
    {
        if (g_uploadCommandList != nullptr)
        {
            return;
        }
        CheckHr(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_uploadAllocator)),
            "ID3D12Device::CreateCommandAllocator (texture upload)");
        CheckHr(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_uploadAllocator.Get(), nullptr, IID_PPV_ARGS(&g_uploadCommandList)),
            "ID3D12Device::CreateCommandList (texture upload)");
        CheckHr(g_uploadCommandList->Close(), "ID3D12GraphicsCommandList::Close (texture upload, initial)");
        CheckHr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_uploadFence)), "ID3D12Device::CreateFence (texture upload)");
        g_uploadFenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        OMD_ASSERT(g_uploadFenceEvent != nullptr, "CreateEventA failed for the texture upload fence");
    }

    // Submits and fully waits on the upload command list - fully synchronous, matching this
    // project's existing frame model (DeviceDX12's own WaitForGpu). Fine for load-time
    // texture uploads; would need batching/async if this ever became a per-frame path.
    void SubmitAndWaitUpload()
    {
        CheckHr(g_uploadCommandList->Close(), "ID3D12GraphicsCommandList::Close (texture upload)");
        ID3D12CommandList* lists[] = { g_uploadCommandList.Get() };
        ID3D12CommandQueue* queue = Renderer::DeviceDX12::GetCommandQueue();
        queue->ExecuteCommandLists(1, lists);

        const UINT64 valueToWaitFor = ++g_uploadFenceValue;
        CheckHr(queue->Signal(g_uploadFence.Get(), valueToWaitFor), "ID3D12CommandQueue::Signal (texture upload)");
        if (g_uploadFence->GetCompletedValue() < valueToWaitFor)
        {
            CheckHr(g_uploadFence->SetEventOnCompletion(valueToWaitFor, g_uploadFenceEvent), "ID3D12Fence::SetEventOnCompletion (texture upload)");
            WaitForSingleObject(g_uploadFenceEvent, INFINITE);
        }
    }
}

namespace Renderer
{
    TextureHandle TextureDX12::Create(const void* pixels, unsigned int width, unsigned int height)
    {
        ID3D12Device* device = DeviceDX12::GetDevice();
        EnsureSrvHeap(device);
        EnsureUploadResources(device);

        OMD_ASSERT(g_textures.size() < kMaxTextures, "Texture SRV heap exhausted (%u max)", kMaxTextures);

        D3D12_HEAP_PROPERTIES defaultHeapProps = {};
        defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = width;
        resourceDesc.Height = height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = kTextureFormat;
        resourceDesc.SampleDesc.Count = 1;

        ComPtr<ID3D12Resource> texture;
        CheckHr(
            device->CreateCommittedResource(
                &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
            "ID3D12Device::CreateCommittedResource (texture)");

        // Default-heap textures can't be Map()'d directly - upload via an intermediate
        // CPU-visible staging buffer + CopyTextureRegion, the standard D3D12 pattern.
        UINT64 uploadBufferSize = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT numRows = 0;
        UINT64 rowSizeInBytes = 0;
        device->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &uploadBufferSize);

        D3D12_HEAP_PROPERTIES uploadHeapProps = {};
        uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadBufferSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> uploadBuffer;
        CheckHr(
            device->CreateCommittedResource(
                &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)),
            "ID3D12Device::CreateCommittedResource (texture upload staging)");

        uint8_t* mapped = nullptr;
        const D3D12_RANGE noRead = { 0, 0 };
        CheckHr(uploadBuffer->Map(0, &noRead, reinterpret_cast<void**>(&mapped)), "ID3D12Resource::Map (texture upload staging)");
        const uint8_t* src = static_cast<const uint8_t*>(pixels);
        constexpr UINT kBytesPerPixel = 4;
        for (UINT row = 0; row < numRows; ++row)
        {
            memcpy(mapped + footprint.Offset + static_cast<size_t>(row) * footprint.Footprint.RowPitch, src + static_cast<size_t>(row) * width * kBytesPerPixel,
                static_cast<size_t>(width) * kBytesPerPixel);
        }
        uploadBuffer->Unmap(0, nullptr);

        CheckHr(g_uploadAllocator->Reset(), "ID3D12CommandAllocator::Reset (texture upload)");
        CheckHr(g_uploadCommandList->Reset(g_uploadAllocator.Get(), nullptr), "ID3D12GraphicsCommandList::Reset (texture upload)");

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
        srcLocation.pResource = uploadBuffer.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLocation.PlacedFootprint = footprint;

        g_uploadCommandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLocation, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_uploadCommandList->ResourceBarrier(1, &barrier);

        // Fully waited on before returning - uploadBuffer (a local ComPtr) is safe to release
        // once this call returns, since the GPU has already finished reading from it.
        SubmitAndWaitUpload();

        TextureHandle handle;
        handle.index = static_cast<int>(g_textures.size());
        g_textures.push_back(texture);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = kTextureFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += static_cast<SIZE_T>(handle.index) * g_srvDescriptorSize;
        device->CreateShaderResourceView(texture.Get(), &srvDesc, cpuHandle);

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Texture created: %ux%u, handle %d", width, height, handle.index);
        return handle;
    }

    void TextureDX12::Shutdown()
    {
        g_textures.clear();
        g_srvHeap.Reset();
        g_uploadCommandList.Reset();
        g_uploadAllocator.Reset();
        g_uploadFence.Reset();
        if (g_uploadFenceEvent != nullptr)
        {
            CloseHandle(g_uploadFenceEvent);
            g_uploadFenceEvent = nullptr;
        }
    }

    ID3D12DescriptorHeap* TextureDX12::GetHeap()
    {
        return g_srvHeap.Get();
    }

    unsigned long long TextureDX12::GetSrvGpuHandle(TextureHandle handle)
    {
        OMD_ASSERT(handle.index >= 0 && static_cast<size_t>(handle.index) < g_textures.size(), "Invalid TextureHandle");
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle.ptr += static_cast<UINT64>(handle.index) * g_srvDescriptorSize;
        return gpuHandle.ptr;
    }
}
