#include "StaticMeshPassDX12.h"

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
    // Matches LitTextured.hlsl's VSInput layout. Deliberately not tied to Asset::Vertex's
    // exact type (Renderer must never depend on Asset) - the caller populating draw items is
    // responsible for uploading data this shape, which Asset::Vertex's own position/normal/
    // uv0/tangent field order happens to satisfy as a prefix without repacking.
    struct MeshVertex
    {
        float position[3];
        float normal[3];
        float uv0[2];
    };

    Renderer::PipelineHandle g_pipelineHandle;
    Renderer::BufferHandle g_cameraBufferHandle;
    Renderer::BufferHandle g_worldBufferHandle;
    std::vector<Renderer::StaticMeshDrawItem> g_drawItems;
}

namespace Renderer
{
    void StaticMeshPassDX12::Init()
    {
        CompiledShader vertexShader = Shader::CompileShader("data/shaders/LitTextured.hlsl", "VSMain", "vs_6_0");
        OMD_ASSERT(!vertexShader.bytecode.empty(), "Static mesh pass vertex shader failed to compile");
        CompiledShader pixelShader = Shader::CompileShader("data/shaders/LitTextured.hlsl", "PSMain", "ps_6_0");
        OMD_ASSERT(!pixelShader.bytecode.empty(), "Static mesh pass pixel shader failed to compile");

        const VertexAttribute vertexAttributes[] = {
            { "POSITION", 0, 3, offsetof(MeshVertex, position) },
            { "NORMAL", 0, 3, offsetof(MeshVertex, normal) },
            { "TEXCOORD", 0, 2, offsetof(MeshVertex, uv0) },
        };

        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = &vertexShader;
        pipelineDesc.pixelShader = &pixelShader;
        pipelineDesc.vertexAttributes = vertexAttributes;
        pipelineDesc.vertexAttributeCount = static_cast<unsigned int>(std::size(vertexAttributes));
        pipelineDesc.constantBufferCount = 2;
        pipelineDesc.srvCount = 1;
        pipelineDesc.depthTestEnabled = true;
        // Sponza (and glTF scenes generally) mix single- and double-sided materials
        // (Asset::Material::doubleSided, imported since 9c but never wired in here before) -
        // thin single-layer surfaces like banners/curtains are typically authored double-sided
        // specifically because only one winding is modeled. Unconditional back-face culling
        // was silently dropping those triangles whenever the camera saw their "back" side.
        // TODO(OM): a per-material doubleSided split (two PSOs, picked per draw item) would
        // restore the minor performance win of culling single-sided materials; not needed yet
        // at this scene's scale.
        pipelineDesc.cullBackFaces = false;
        g_pipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        g_cameraBufferHandle = Buffer::Create(&identity, sizeof(identity));
        g_worldBufferHandle = Buffer::Create(&identity, sizeof(identity));

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Static mesh pass initialized");
    }

    void StaticMeshPassDX12::Shutdown()
    {
        g_drawItems.clear();
    }

    void StaticMeshPassDX12::SetDrawItems(const std::vector<StaticMeshDrawItem>& items)
    {
        g_drawItems = items;
        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Static mesh pass: %zu draw item(s)", g_drawItems.size());
    }

    void StaticMeshPassDX12::Render(const StaticMeshRenderDesc& desc)
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

        for (const StaticMeshDrawItem& item : g_drawItems)
        {
            Buffer::Update(g_worldBufferHandle, &item.world, sizeof(item.world));
            cmdList->SetGraphicsRootConstantBufferView(1, BufferDX12::GetResource(g_worldBufferHandle)->GetGPUVirtualAddress());
            cmdList->SetGraphicsRootDescriptorTable(2, D3D12_GPU_DESCRIPTOR_HANDLE{ TextureDX12::GetSrvGpuHandle(item.baseColorTexture) });

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
