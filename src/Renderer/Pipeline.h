#pragma once

#include "PipelineDesc.h"
#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/PipelineDX12.h"
#endif

namespace Renderer
{
    // Root signature + PSO registry. Backend-agnostic front layer - see
    // PlatformMacros.h. Every render pass creates its pipeline(s) through
    // this instead of hand-rolling its own PSO boilerplate (see DESIGN.md's
    // render pass convention section).
    struct Pipeline : public OMD_GFX_CLASS(Pipeline)
    {
        static PipelineHandle Create(const PipelineDesc& desc)
        {
            return OMD_GFX_CALL(Pipeline, Create(desc));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(Pipeline, Shutdown());
        }
    };
}
