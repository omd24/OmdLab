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
    Renderer::PipelineHandle g_transparentPipelineHandle;
    Renderer::BufferHandle g_cameraBufferHandle;
    // Bound at CBV slot 2 whenever a draw item doesn't set its own tintBuffer (index == -1) -
    // white/opaque, so ordinary opaque callers (ground plane, local test scene) never need to
    // know tinting exists at all. Never Update()-d - only real per-item tintBuffers change.
    Renderer::BufferHandle g_defaultTintBufferHandle;
    std::vector<Renderer::StaticMeshDrawItem> g_opaqueDrawItems;
    std::vector<Renderer::StaticMeshDrawItem> g_transparentDrawItems;
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
        // 3, not 2 - b0 camera, b1 World, b2 TintConstants (see LitTextured.hlsl). The SRV
        // table's own root-parameter index shifts automatically from this (PipelineDX12::
        // CreateGraphics builds one CBV root parameter per constantBufferCount, then appends
        // the SRV table after), so nothing else needs to change to accommodate it.
        pipelineDesc.constantBufferCount = 3;
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

        // Same shaders/vertex layout/root signature shape as the opaque PSO above - only the
        // blend/depth-write state differs. Depth test stays on (still occluded correctly by
        // opaque geometry already in the depth buffer); depth write is off so overlapping
        // transparent draws don't occlude each other via depth alone (this project relies on
        // draw-list order for that, not per-pixel order-independent transparency - fine at
        // this project's scale, per its own "opaque first, then transparency, just sort by
        // draw order" convention).
        pipelineDesc.alphaBlendEnabled = true;
        pipelineDesc.depthWriteEnabled = false;
        g_transparentPipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        g_cameraBufferHandle = Buffer::Create(&identity, sizeof(identity));

        const DirectX::XMFLOAT4 whiteOpaqueTint = { 1.0f, 1.0f, 1.0f, 1.0f };
        g_defaultTintBufferHandle = Buffer::Create(&whiteOpaqueTint, sizeof(whiteOpaqueTint));

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Static mesh pass initialized");
    }

    void StaticMeshPassDX12::Shutdown()
    {
        g_opaqueDrawItems.clear();
        g_transparentDrawItems.clear();
    }

    void StaticMeshPassDX12::SetDrawItems(const std::vector<StaticMeshDrawItem>& items)
    {
        // Partitioned once here (not every Render() call, which runs every frame) - this is
        // already documented as not a per-frame call, so this is the cheap place to split by
        // StaticMeshDrawItem::transparent, preserving each subset's relative input order (the
        // caller's own push order becomes the de facto draw order within the transparent
        // subset - this project relies on that instead of real back-to-front sorting).
        g_opaqueDrawItems.clear();
        g_transparentDrawItems.clear();
        for (const StaticMeshDrawItem& item : items)
        {
            (item.transparent ? g_transparentDrawItems : g_opaqueDrawItems).push_back(item);
        }
        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Renderer", "Static mesh pass: %zu opaque, %zu transparent draw item(s)",
            g_opaqueDrawItems.size(), g_transparentDrawItems.size());
    }

    namespace
    {
        void DrawItems(ID3D12GraphicsCommandList* cmdList, const std::vector<Renderer::StaticMeshDrawItem>& items)
        {
            for (const Renderer::StaticMeshDrawItem& item : items)
            {
                cmdList->SetGraphicsRootConstantBufferView(1, BufferDX12::GetResource(item.worldBuffer)->GetGPUVirtualAddress());
                // Unset (index == -1, the default-constructed value) falls back to the pass's
                // own shared white/opaque tint buffer - see tintBuffer's own comment.
                const Renderer::BufferHandle tintBuffer = item.tintBuffer.index >= 0 ? item.tintBuffer : g_defaultTintBufferHandle;
                cmdList->SetGraphicsRootConstantBufferView(2, BufferDX12::GetResource(tintBuffer)->GetGPUVirtualAddress());
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

    // Split into two entry points (rather than one Render() doing both in sequence) so
    // RenderTasks can interleave this pass's two halves with every other pass's own -
    // "every opaque draw before any transparent draw," globally across the whole frame, not
    // just within this one pass. See StaticMeshPass.h's own comment and RenderTasks::DoFrame.
    // Each is self-contained (own camera CBV update, heaps, topology) rather than assuming a
    // particular call order.
    void StaticMeshPassDX12::RenderOpaque(const StaticMeshRenderDesc& desc)
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

    void StaticMeshPassDX12::RenderTransparent(const StaticMeshRenderDesc& desc)
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

        // Root signature is the same shape as the opaque PSO's (same constantBufferCount/
        // srvCount), but each PipelineDX12::CreateGraphics call still creates its own root
        // signature object, so it's rebound explicitly rather than assumed shared.
        cmdList->SetGraphicsRootSignature(PipelineDX12::GetRootSignature(g_transparentPipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_transparentPipelineHandle));
        cmdList->SetGraphicsRootConstantBufferView(0, BufferDX12::GetResource(g_cameraBufferHandle)->GetGPUVirtualAddress());
        DrawItems(cmdList, g_transparentDrawItems);
    }
}
