#include "DebugDrawPassDX12.h"

#include "BufferDX12.h"
#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "PipelineDX12.h"
#include "Renderer/Buffer.h"
#include "Renderer/Pipeline.h"
#include "Renderer/Shader.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cstddef>
#include <d3d12.h>
#include <iterator>

namespace
{
    // Matches DebugDraw.hlsl's VSInput (float3 position, float3 color) - same layout as
    // Triangle.hlsl's own vertex, kept as a separate shader/struct anyway since this pass's
    // topology (line list) and PSO (no culling, since lines have no faces) differ.
    struct DebugDrawVertex
    {
        float position[3];
        float color[3];
    };

    // Fixed capacity - generous for this project's current scale (a handful of hitbox/hurtbox/
    // trigger boxes across one or two entities, 12 lines per box). Debug-only visualization, not
    // worth a dynamically-growing buffer for; revisit if a real need for more lines appears.
    constexpr UINT kMaxDebugDrawLines = 512;

    Renderer::PipelineHandle g_pipelineHandle;
    Renderer::BufferHandle g_vertexBufferHandle;
    UINT g_vertexStride = 0;
    UINT g_lineCount = 0;

    Renderer::BufferHandle g_cameraBufferHandle;
}

namespace Renderer
{
    void DebugDrawPassDX12::Init()
    {
        CompiledShader vertexShader = Shader::CompileShader("data/shaders/DebugDraw.hlsl", "VSMain", "vs_6_0");
        OMD_ASSERT(!vertexShader.bytecode.empty(), "Debug draw pass vertex shader failed to compile");
        CompiledShader pixelShader = Shader::CompileShader("data/shaders/DebugDraw.hlsl", "PSMain", "ps_6_0");
        OMD_ASSERT(!pixelShader.bytecode.empty(), "Debug draw pass pixel shader failed to compile");

        const VertexAttribute vertexAttributes[] = {
            { "POSITION", 0, 3, offsetof(DebugDrawVertex, position) },
            { "COLOR", 0, 3, offsetof(DebugDrawVertex, color) },
        };

        GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = &vertexShader;
        pipelineDesc.pixelShader = &pixelShader;
        pipelineDesc.vertexAttributes = vertexAttributes;
        pipelineDesc.vertexAttributeCount = static_cast<unsigned int>(std::size(vertexAttributes));
        pipelineDesc.constantBufferCount = 1;
        // Composites correctly against real geometry (e.g. a hitbox drawn behind the character
        // it belongs to isn't visible through it) - same reasoning as ForwardPass.
        pipelineDesc.depthTestEnabled = true;
        pipelineDesc.cullBackFaces = false; // Lines have no faces to cull.
        pipelineDesc.topology = GraphicsPipelineDesc::Topology::Line;
        g_pipelineHandle = Pipeline::CreateGraphics(pipelineDesc);

        g_vertexStride = sizeof(DebugDrawVertex);
        // Seeded at zero/max capacity up front - SetLines only ever memcpy's within this size
        // via Buffer::Update, never recreates the resource.
        const std::vector<DebugDrawVertex> initialVertices(static_cast<size_t>(kMaxDebugDrawLines) * 2);
        g_vertexBufferHandle = Buffer::Create(initialVertices.data(), initialVertices.size() * sizeof(DebugDrawVertex));

        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
        g_cameraBufferHandle = Buffer::Create(&identity, sizeof(identity));

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Debug draw pass initialized");
    }

    void DebugDrawPassDX12::Shutdown()
    {
        // No-op: see ForwardPassDX12::Shutdown's own comment for why.
    }

    void DebugDrawPassDX12::SetLines(const std::vector<DebugDrawLine>& lines)
    {
        // Silently truncated rather than asserted - a debug-only visualization whose input
        // count grows with unrelated content (more entities, more hitboxes) shouldn't be able to
        // hard-crash the whole app over a capacity this generous.
        const UINT lineCount = static_cast<UINT>(std::min<size_t>(lines.size(), kMaxDebugDrawLines));

        std::vector<DebugDrawVertex> vertices(static_cast<size_t>(lineCount) * 2);
        for (UINT i = 0; i < lineCount; ++i)
        {
            const DebugDrawLine& line = lines[i];
            vertices[i * 2 + 0] = { { line.start.x, line.start.y, line.start.z }, { line.color.x, line.color.y, line.color.z } };
            vertices[i * 2 + 1] = { { line.end.x, line.end.y, line.end.z }, { line.color.x, line.color.y, line.color.z } };
        }
        if (!vertices.empty())
        {
            Buffer::Update(g_vertexBufferHandle, vertices.data(), vertices.size() * sizeof(DebugDrawVertex));
        }
        g_lineCount = lineCount;
    }

    void DebugDrawPassDX12::Render(const DebugDrawRenderDesc& desc)
    {
        if (g_lineCount == 0)
        {
            return;
        }

        Buffer::Update(g_cameraBufferHandle, &desc.viewProjection, sizeof(desc.viewProjection));

        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();

        cmdList->SetGraphicsRootSignature(PipelineDX12::GetRootSignature(g_pipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_pipelineHandle));
        cmdList->SetGraphicsRootConstantBufferView(0, BufferDX12::GetResource(g_cameraBufferHandle)->GetGPUVirtualAddress());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);

        D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
        vertexBufferView.BufferLocation = BufferDX12::GetResource(g_vertexBufferHandle)->GetGPUVirtualAddress();
        vertexBufferView.SizeInBytes = g_vertexStride * g_lineCount * 2;
        vertexBufferView.StrideInBytes = g_vertexStride;
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);

        cmdList->DrawInstanced(g_lineCount * 2, 1, 0, 0);
    }
}
