#pragma once

#include "Renderer/TextureHandle.h"

struct ID3D12DescriptorHeap;

namespace Renderer
{
    struct TextureDX12
    {
        // pixels must be tightly-packed RGBA8 (4 bytes/pixel, width*height*4 bytes total).
        static TextureHandle Create(const void* pixels, unsigned int width, unsigned int height);
        static void Shutdown();

        // For other dx12/ backend files (render passes) that need to bind the shared texture
        // SRV heap via SetDescriptorHeaps before drawing - only one shader-visible
        // CBV_SRV_UAV heap can be bound at a time, so a pass using textures must (re-)bind
        // this one itself rather than assuming it's already active. Only valid until
        // Shutdown().
        static ID3D12DescriptorHeap* GetHeap();

        // GPU descriptor handle (as a raw UINT64 - avoids needing the full
        // D3D12_GPU_DESCRIPTOR_HANDLE definition in this header) for binding via
        // SetGraphicsRootDescriptorTable. Only valid until Shutdown().
        static unsigned long long GetSrvGpuHandle(TextureHandle handle);
    };
}
