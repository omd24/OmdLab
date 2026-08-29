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
    Renderer::PipelineHandle g_transparentPipelineHandle;
    Renderer::BufferHandle g_cameraBufferHandle;
    // Bound at CBV slot 3 whenever a draw item doesn't set its own tintBuffer (index == -1) -
    // white/opaque, mirroring StaticMeshPassDX12's own default-tint fallback. Never Update()-d.
    Renderer::BufferHandle g_defaultTintBufferHandle;
    std::vector<Renderer::SkinnedMeshDrawItem> g_opaqueDrawItems;
    std::vector<Renderer::SkinnedMeshDrawItem> g_transparentDrawItems;
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
        // 4, not 3 - b0 camera, b1 world, b2 bone palette, b3 TintConstants (see
        // SkinnedMesh.hlsl). The SRV table's own root-parameter index shifts automatically
        // from this, same as StaticMeshPassDX12's own equivalent change.
        pipelineDesc.constantBufferCount = 4;
        pipelineDesc.srvCount = 1;
        pipelineDesc.depthTestEnabled = true;
        g_pipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        // Same shaders/vertex layout/root signature shape as the opaque PSO above - only the
        // blend/depth-write state differs. See StaticMeshPassDX12's own transparent PSO for
        // why depth-write is off while depth-test stays on. Nothing currently marks a
        // SkinnedMeshDrawItem transparent (built ahead of a concrete need, at the user's own
        // request), but the PSO exists so a future one can without further pass-level work.
        pipelineDesc.alphaBlendEnabled = true;
        pipelineDesc.depthWriteEnabled = false;
        g_transparentPipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        g_cameraBufferHandle = Buffer::Create(&identity, sizeof(identity));

        const DirectX::XMFLOAT4 whiteOpaqueTint = { 1.0f, 1.0f, 1.0f, 1.0f };
        g_defaultTintBufferHandle = Buffer::Create(&whiteOpaqueTint, sizeof(whiteOpaqueTint));

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Skinned mesh pass initialized");
    }

    void SkinnedMeshPassDX12::Shutdown()
    {
        g_opaqueDrawItems.clear();
        g_transparentDrawItems.clear();
    }

    void SkinnedMeshPassDX12::SetDrawItems(const std::vector<SkinnedMeshDrawItem>& items)
    {
        // Partitioned once here (not every Render call) - see StaticMeshPassDX12::
        // SetDrawItems's own comment for why.
        g_opaqueDrawItems.clear();
        g_transparentDrawItems.clear();
        for (const SkinnedMeshDrawItem& item : items)
        {
            (item.transparent ? g_transparentDrawItems : g_opaqueDrawItems).push_back(item);
        }
        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Renderer", "Skinned mesh pass: %zu opaque, %zu transparent draw item(s)",
            g_opaqueDrawItems.size(), g_transparentDrawItems.size());
    }

    namespace
    {
        void DrawItems(ID3D12GraphicsCommandList* cmdList, const std::vector<Renderer::SkinnedMeshDrawItem>& items)
        {
            for (const Renderer::SkinnedMeshDrawItem& item : items)
            {
                cmdList->SetGraphicsRootConstantBufferView(1, BufferDX12::GetResource(item.worldBuffer)->GetGPUVirtualAddress());
                cmdList->SetGraphicsRootConstantBufferView(2, BufferDX12::GetResource(item.bonePaletteBuffer)->GetGPUVirtualAddress());
                const Renderer::BufferHandle tintBuffer = item.tintBuffer.index >= 0 ? item.tintBuffer : g_defaultTintBufferHandle;
                cmdList->SetGraphicsRootConstantBufferView(3, BufferDX12::GetResource(tintBuffer)->GetGPUVirtualAddress());
                cmdList->SetGraphicsRootDescriptorTable(4, D3D12_GPU_DESCRIPTOR_HANDLE{ TextureDX12::GetSrvGpuHandle(item.baseColorTexture) });

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

    // Split into two entry points, same reasoning as StaticMeshPassDX12::RenderOpaque/
    // RenderTransparent - see that pass's own comment and RenderTasks::DoFrame.
    void SkinnedMeshPassDX12::RenderOpaque(const SkinnedMeshRenderDesc& desc)
    {
        if (g_opaqueDrawItems.empty())
        {
            return;
        }

        Buffer::Update(g_cameraBufferHandle, &desc.viewProjection, sizeof(desc.viewProjection));

        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();
        ID3D12DescriptorHeap* heaps[] = { TextureDX12::GetHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        cmdList->SetGraphicsRootSignature(PipelineDX12::GetRootSignature(g_pipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_pipelineHandle));
        cmdList->SetGraphicsRootConstantBufferView(0, BufferDX12::GetResource(g_cameraBufferHandle)->GetGPUVirtualAddress());
        DrawItems(cmdList, g_opaqueDrawItems);
    }

    void SkinnedMeshPassDX12::RenderTransparent(const SkinnedMeshRenderDesc& desc)
    {
        if (g_transparentDrawItems.empty())
        {
            return;
        }

        Buffer::Update(g_cameraBufferHandle, &desc.viewProjection, sizeof(desc.viewProjection));

        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();
        ID3D12DescriptorHeap* heaps[] = { TextureDX12::GetHeap() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        cmdList->SetGraphicsRootSignature(PipelineDX12::GetRootSignature(g_transparentPipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_transparentPipelineHandle));
        cmdList->SetGraphicsRootConstantBufferView(0, BufferDX12::GetResource(g_cameraBufferHandle)->GetGPUVirtualAddress());
        DrawItems(cmdList, g_transparentDrawItems);
    }
}
