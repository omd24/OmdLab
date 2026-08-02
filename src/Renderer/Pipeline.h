#pragma once

#include "PipelineDesc.h"
#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/PipelineDX12.h"
#endif

namespace Renderer
{
    // Root signature + PSO registry, for both graphics and compute
    // pipelines. Backend-agnostic front layer - see PlatformMacros.h. Every
    // render pass creates its pipeline(s) through this instead of
    // hand-rolling its own PSO boilerplate.
    struct Pipeline : public OMD_GFX_CLASS(Pipeline)
    {
        static PipelineHandle CreateGraphics(const GraphicsPipelineDesc& desc)
        {
            return OMD_GFX_CALL(Pipeline, CreateGraphics(desc));
        }

        static PipelineHandle CreateCompute(const ComputePipelineDesc& desc)
        {
            return OMD_GFX_CALL(Pipeline, CreateCompute(desc));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(Pipeline, Shutdown());
        }
    };
}
