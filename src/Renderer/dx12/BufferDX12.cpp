#include "BufferDX12.h"

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
    std::vector<ComPtr<ID3D12Resource>> g_buffers;

    void CheckHr(HRESULT hr, const char* what)
    {
        OMD_ASSERT(SUCCEEDED(hr), "%s failed with HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
    }
}

namespace Renderer
{
    BufferHandle BufferDX12::Create(const void* data, size_t sizeBytes)
    {
        ID3D12Device* device = DeviceDX12::GetDevice();

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = sizeBytes;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> resource;
        CheckHr(
            device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)),
            "ID3D12Device::CreateCommittedResource (upload buffer)");

        void* mapped = nullptr;
        const D3D12_RANGE noRead = { 0, 0 };
        CheckHr(resource->Map(0, &noRead, &mapped), "ID3D12Resource::Map");
        memcpy(mapped, data, sizeBytes);
        resource->Unmap(0, nullptr);

        g_buffers.push_back(resource);

        BufferHandle handle;
        handle.index = static_cast<int>(g_buffers.size() - 1);

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Buffer created: %zu bytes, handle %d", sizeBytes, handle.index);
        return handle;
    }

    // Safe to Map/memcpy/Unmap directly with no extra synchronization only
    // because every frame is already fully waited on before the next one's
    // CPU work starts (see DeviceDX12's WaitForGpu) - the GPU is never still
    // reading this buffer when this runs. Would need real synchronization
    // (e.g. per-frame-in-flight buffer copies) if that ever changes.
    void BufferDX12::Update(BufferHandle handle, const void* data, size_t sizeBytes)
    {
        OMD_ASSERT(handle.index >= 0 && static_cast<size_t>(handle.index) < g_buffers.size(), "Invalid BufferHandle");
        ID3D12Resource* resource = g_buffers[handle.index].Get();

        void* mapped = nullptr;
        const D3D12_RANGE noRead = { 0, 0 };
        CheckHr(resource->Map(0, &noRead, &mapped), "ID3D12Resource::Map (update)");
        memcpy(mapped, data, sizeBytes);
        resource->Unmap(0, nullptr);
    }

    void BufferDX12::Shutdown()
    {
        g_buffers.clear();
    }

    ID3D12Resource* BufferDX12::GetResource(BufferHandle handle)
    {
        OMD_ASSERT(handle.index >= 0 && static_cast<size_t>(handle.index) < g_buffers.size(), "Invalid BufferHandle");
        return g_buffers[handle.index].Get();
    }
}
