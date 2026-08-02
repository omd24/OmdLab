#include "PipelineDX12.h"

#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"

#include <d3d12.h>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    struct PipelineEntry
    {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pso;
    };

    std::vector<PipelineEntry> g_pipelines;

    void CheckHr(HRESULT hr, const char* what)
    {
        OMD_ASSERT(SUCCEEDED(hr), "%s failed with HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
    }

    DXGI_FORMAT FormatForFloatCount(unsigned int floatCount)
    {
        switch (floatCount)
        {
            case 1: return DXGI_FORMAT_R32_FLOAT;
            case 2: return DXGI_FORMAT_R32G32_FLOAT;
            case 3: return DXGI_FORMAT_R32G32B32_FLOAT;
            case 4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
            default:
                OMD_ASSERT(false, "Unsupported vertex attribute float count: %u", floatCount);
                return DXGI_FORMAT_UNKNOWN;
        }
    }

    // Shared by CreateGraphics()/CreateCompute() - both serialize a
    // D3D12_ROOT_SIGNATURE_DESC the same way, they just build a different
    // one.
    ComPtr<ID3D12RootSignature> SerializeAndCreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& rootSignatureDesc)
    {
        ComPtr<ID3DBlob> signatureBlob;
        ComPtr<ID3DBlob> errorBlob;
        const HRESULT serializeHr =
            D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(serializeHr) && errorBlob)
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Error, "Renderer", "Root signature serialization failed: %s",
                static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        CheckHr(serializeHr, "D3D12SerializeRootSignature");

        ComPtr<ID3D12RootSignature> rootSignature;
        CheckHr(
            device->CreateRootSignature(
                0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature)),
            "ID3D12Device::CreateRootSignature");
        return rootSignature;
    }
}

namespace Renderer
{
    PipelineHandle PipelineDX12::CreateGraphics(const GraphicsPipelineDesc& desc)
    {
        ID3D12Device* device = DeviceDX12::GetDevice();

        // No CBV/SRV/UAV/samplers - see GraphicsPipelineDesc.
        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        PipelineEntry entry;
        entry.rootSignature = SerializeAndCreateRootSignature(device, rootSignatureDesc);

        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements(desc.vertexAttributeCount);
        for (unsigned int i = 0; i < desc.vertexAttributeCount; ++i)
        {
            const VertexAttribute& attribute = desc.vertexAttributes[i];
            inputElements[i] = {
                attribute.semanticName,
                attribute.semanticIndex,
                FormatForFloatCount(attribute.floatCount),
                0,
                attribute.offset,
                D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
                0,
            };
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = entry.rootSignature.Get();
        psoDesc.VS = { desc.vertexShader->bytecode.data(), desc.vertexShader->bytecode.size() };
        psoDesc.PS = { desc.pixelShader->bytecode.data(), desc.pixelShader->bytecode.size() };
        psoDesc.InputLayout = { inputElements.data(), static_cast<UINT>(inputElements.size()) };
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // No depth buffer - depth testing stays off until one exists.
        //
        // TODO(OM): enable depth testing once a depth buffer exists (needed
        // once real 3D geometry with occlusion is rendered).
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.SampleDesc.Count = 1;

        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = kBackBufferFormat;

        CheckHr(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&entry.pso)), "ID3D12Device::CreateGraphicsPipelineState");

        g_pipelines.push_back(entry);

        PipelineHandle handle;
        handle.index = static_cast<int>(g_pipelines.size() - 1);

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Renderer", "Graphics pipeline created (root signature + PSO), handle %d", handle.index);
        return handle;
    }

    PipelineHandle PipelineDX12::CreateCompute(const ComputePipelineDesc& desc)
    {
        ID3D12Device* device = DeviceDX12::GetDevice();

        // One UAV descriptor table (u0) - see ComputePipelineDesc.
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParameter = {};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameter.DescriptorTable.NumDescriptorRanges = 1;
        rootParameter.DescriptorTable.pDescriptorRanges = &uavRange;
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
        rootSignatureDesc.NumParameters = 1;
        rootSignatureDesc.pParameters = &rootParameter;

        PipelineEntry entry;
        entry.rootSignature = SerializeAndCreateRootSignature(device, rootSignatureDesc);

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = entry.rootSignature.Get();
        psoDesc.CS = { desc.computeShader->bytecode.data(), desc.computeShader->bytecode.size() };

        CheckHr(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&entry.pso)), "ID3D12Device::CreateComputePipelineState");

        g_pipelines.push_back(entry);

        PipelineHandle handle;
        handle.index = static_cast<int>(g_pipelines.size() - 1);

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Renderer", "Compute pipeline created (root signature + PSO), handle %d", handle.index);
        return handle;
    }

    void PipelineDX12::Shutdown()
    {
        g_pipelines.clear();
    }

    ID3D12PipelineState* PipelineDX12::GetPSO(PipelineHandle handle)
    {
        OMD_ASSERT(handle.index >= 0 && static_cast<size_t>(handle.index) < g_pipelines.size(), "Invalid PipelineHandle");
        return g_pipelines[handle.index].pso.Get();
    }

    ID3D12RootSignature* PipelineDX12::GetRootSignature(PipelineHandle handle)
    {
        OMD_ASSERT(handle.index >= 0 && static_cast<size_t>(handle.index) < g_pipelines.size(), "Invalid PipelineHandle");
        return g_pipelines[handle.index].rootSignature.Get();
    }
}
