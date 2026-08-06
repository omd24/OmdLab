#include "SkinnedMeshPassDX12.h"

#include "BufferDX12.h"
#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "PipelineDX12.h"
#include "Renderer/Buffer.h"
#include "Renderer/Pipeline.h"
#include "Renderer/Shader.h"
#include "TextureDX12.h"

#include <DirectXMath.h>
#include <cstddef>
#include <d3d12.h>
#include <iterator>

namespace
{
    // Matches SkinnedMesh.hlsl's VSInput layout. Joint indices are stored as floats rather than
    // an integer vertex format - this project's one skin has 22 joints, well within a float's
    // exact-integer range, so decoding via round() in the shader avoids extending
    // VertexAttribute/PipelineDX12's format table for one asset.
    struct SkinnedMeshVertex
    {
        float position[3];
        float normal[3];
        float uv0[2];
        float jointIndices[4];
        float jointWeights[4];
    };

    Renderer::PipelineHandle g_pipelineHandle;
    Renderer::BufferHandle g_cameraBufferHandle;
    std::vector<Renderer::SkinnedMeshDrawItem> g_drawItems;
}

namespace Renderer
{
    void SkinnedMeshPassDX12::Init()
    {
        CompiledShader vertexShader = Shader::CompileShader("data/shaders/SkinnedMesh.hlsl", "VSMain", "vs_6_0");
        OMD_ASSERT(!vertexShader.bytecode.empty(), "Skinned mesh pass vertex shader failed to compile");
        CompiledShader pixelShader = Shader::CompileShader("data/shaders/SkinnedMesh.hlsl", "PSMain", "ps_6_0");
        OMD_ASSERT(!pixelShader.bytecode.empty(), "Skinned mesh pass pixel shader failed to compile");

        const VertexAttribute vertexAttributes[] = {
            { "POSITION", 0, 3, offsetof(SkinnedMeshVertex, position) },
            { "NORMAL", 0, 3, offsetof(SkinnedMeshVertex, normal) },
            { "TEXCOORD", 0, 2, offsetof(SkinnedMeshVertex, uv0) },
            { "JOINTS", 0, 4, offsetof(SkinnedMeshVertex, jointIndices) },
            { "WEIGHTS", 0, 4, offsetof(SkinnedMeshVertex, jointWeights) },
        };

        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = &vertexShader;
        pipelineDesc.pixelShader = &pixelShader;
        pipelineDesc.vertexAttributes = vertexAttributes;
        pipelineDesc.vertexAttributeCount = static_cast<unsigned int>(std::size(vertexAttributes));
        // b0 camera, b1 world, b2 bone palette.
        pipelineDesc.constantBufferCount = 3;
        pipelineDesc.srvCount = 1;
        pipelineDesc.depthTestEnabled = true;
        g_pipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        g_cameraBufferHandle = Buffer::Create(&identity, sizeof(identity));

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Skinned mesh pass initialized");
    }

    void SkinnedMeshPassDX12::Shutdown()
    {
        g_drawItems.clear();
    }

    void SkinnedMeshPassDX12::SetDrawItems(const std::vector<SkinnedMeshDrawItem>& items)
    {
        g_drawItems = items;
        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Skinned mesh pass: %zu draw item(s)", g_drawItems.size());
    }

    void SkinnedMeshPassDX12::Render(const SkinnedMeshRenderDesc& desc)
    {
        if (g_drawItems.empty())
        {
            return;
        }

        Buffer::Update(g_cameraBufferHandle, &desc.viewProjection, sizeof(desc.viewProjection));

        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();

        ID3D12DescriptorHeap* heaps[] = { TextureDX12::GetHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);

        cmdList->SetGraphicsRootSignature(PipelineDX12::GetRootSignature(g_pipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_pipelineHandle));
        cmdList->SetGraphicsRootConstantBufferView(0, BufferDX12::GetResource(g_cameraBufferHandle)->GetGPUVirtualAddress());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (const SkinnedMeshDrawItem& item : g_drawItems)
        {
            cmdList->SetGraphicsRootConstantBufferView(1, BufferDX12::GetResource(item.worldBuffer)->GetGPUVirtualAddress());
            cmdList->SetGraphicsRootConstantBufferView(2, BufferDX12::GetResource(item.bonePaletteBuffer)->GetGPUVirtualAddress());
            cmdList->SetGraphicsRootDescriptorTable(3, D3D12_GPU_DESCRIPTOR_HANDLE{ TextureDX12::GetSrvGpuHandle(item.baseColorTexture) });

            D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
            vertexBufferView.BufferLocation = BufferDX12::GetResource(item.vertexBuffer)->GetGPUVirtualAddress();
            vertexBufferView.SizeInBytes = static_cast<UINT>(BufferDX12::GetResource(item.vertexBuffer)->GetDesc().Width);
            vertexBufferView.StrideInBytes = item.vertexStride;
            cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);

            D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
            indexBufferView.BufferLocation = BufferDX12::GetResource(item.indexBuffer)->GetGPUVirtualAddress();
            indexBufferView.SizeInBytes = item.indexCount * sizeof(uint32_t);
            indexBufferView.Format = DXGI_FORMAT_R32_UINT;
            cmdList->IASetIndexBuffer(&indexBufferView);

            cmdList->DrawIndexedInstanced(item.indexCount, 1, 0, 0, 0);
        }
    }
}
