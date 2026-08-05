#include "ForwardPassDX12.h"

#include "BufferDX12.h"
#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "PipelineDX12.h"
#include "Renderer/Buffer.h"
#include "Renderer/Pipeline.h"
#include "Renderer/Shader.h"

#include <DirectXMath.h>
#include <cstddef>
#include <d3d12.h>
#include <iterator>

namespace
{
    // Matches Triangle.hlsl's VSInput (float3 position, float3 color).
    struct TriangleVertex
    {
        float position[3];
        float color[3];
    };

    Renderer::PipelineHandle g_pipelineHandle;
    Renderer::BufferHandle g_vertexBufferHandle;
    UINT g_vertexStride = 0;
    UINT g_vertexCount = 0;

    Renderer::BufferHandle g_cameraBufferHandle;
}

namespace Renderer
{
    void ForwardPassDX12::Init()
    {
        CompiledShader vertexShader = Shader::CompileShader("data/shaders/Triangle.hlsl", "VSMain", "vs_6_0");
        OMD_ASSERT(!vertexShader.bytecode.empty(), "Forward pass vertex shader failed to compile");
        CompiledShader pixelShader = Shader::CompileShader("data/shaders/Triangle.hlsl", "PSMain", "ps_6_0");
        OMD_ASSERT(!pixelShader.bytecode.empty(), "Forward pass pixel shader failed to compile");

        const VertexAttribute vertexAttributes[] = {
            { "POSITION", 0, 3, offsetof(TriangleVertex, position) },
            { "COLOR", 0, 3, offsetof(TriangleVertex, color) },
        };

        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = &vertexShader;
        pipelineDesc.pixelShader = &pixelShader;
        pipelineDesc.vertexAttributes = vertexAttributes;
        pipelineDesc.vertexAttributeCount = static_cast<unsigned int>(std::size(vertexAttributes));
        pipelineDesc.constantBufferCount = 1;
        // Now a real world-space object placed by the camera (since 9b) rather than a fixed
        // clip-space overlay - needs depth test/write like any other spatial geometry so it
        // composites correctly against other depth-tested passes (e.g. StaticMeshPass) when
        // both are enabled at once, instead of always drawing over whatever it doesn't cover.
        pipelineDesc.depthTestEnabled = true;
        g_pipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        // World-space positions at the origin, in the XY plane - the camera
        // (currently a temporary one computed directly in Game/main.cpp; see
        // the camera ownership convention) is what places this in view.
        // Same relative winding as the original hardcoded clip-space
        // version, still clockwise as seen by a camera looking down +Z.
        const TriangleVertex vertices[] = {
            { { 0.0f, 0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
            { { 0.45f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
            { { -0.45f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f } },
        };
        g_vertexBufferHandle = Buffer::Create(vertices, sizeof(vertices));
        g_vertexStride = sizeof(TriangleVertex);
        g_vertexCount = static_cast<UINT>(std::size(vertices));

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        g_cameraBufferHandle = Buffer::Create(&identity, sizeof(identity));

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Forward pass initialized");
    }

    void ForwardPassDX12::Shutdown()
    {
        // No-op: the PSO/root signature and buffers live in Pipeline's and
        // Buffer's own registries, released via their own Shutdown(). Kept
        // for shape consistency with other passes, which may own resources
        // directly.
    }

    void ForwardPassDX12::Render(const ForwardRenderDesc& desc)
    {
        Buffer::Update(g_cameraBufferHandle, &desc.viewProjection, sizeof(desc.viewProjection));

        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();

        cmdList->SetGraphicsRootSignature(PipelineDX12::GetRootSignature(g_pipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_pipelineHandle));
        cmdList->SetGraphicsRootConstantBufferView(0, BufferDX12::GetResource(g_cameraBufferHandle)->GetGPUVirtualAddress());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
        vertexBufferView.BufferLocation = BufferDX12::GetResource(g_vertexBufferHandle)->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = g_vertexStride * g_vertexCount;
        vertexBufferView.StrideInBytes = g_vertexStride;
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);

        cmdList->DrawInstanced(g_vertexCount, 1, 0, 0);
    }
}
