#pragma once

#include "Renderer/PipelineDesc.h"

struct ID3D12PipelineState;
struct ID3D12RootSignature;

namespace Renderer
{
    struct PipelineDX12
    {
        static PipelineHandle CreateGraphics(const GraphicsPipelineDesc& desc);
        static PipelineHandle CreateCompute(const ComputePipelineDesc& desc);
        static void Shutdown();

        // For other dx12/ backend files (e.g. render passes) that need to
        // bind the pipeline. Only valid until Shutdown().
        static ID3D12PipelineState* GetPSO(PipelineHandle handle);
        static ID3D12RootSignature* GetRootSignature(PipelineHandle handle);
    };
}
