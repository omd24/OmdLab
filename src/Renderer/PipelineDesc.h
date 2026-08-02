#pragma once

#include "CompiledShader.h"
#include "VertexAttribute.h"

namespace Renderer
{
    // Opaque handle into Pipeline's internal registry (graphics and compute
    // pipelines share one registry/handle type - both are just "a root
    // signature + a PSO" as far as anything binding them is concerned). Its
    // own header (like the desc types below) so both the front layer and
    // the DX12 implementation can see the full definition without
    // including each other.
    struct PipelineHandle
    {
        int index = -1;
    };

    // Describes one graphics root signature + PSO. Shader bytecode and
    // vertex layout only - no resource bindings beyond the input assembler.
    //
    // TODO(OM): add CBV/SRV/UAV/sampler bindings once a pass needs any
    // (e.g. a camera constant buffer, a material texture).
    struct GraphicsPipelineDesc
    {
        const CompiledShader* vertexShader = nullptr;
        const CompiledShader* pixelShader = nullptr;
        const VertexAttribute* vertexAttributes = nullptr;
        unsigned int vertexAttributeCount = 0;
    };

    // Describes one compute root signature + PSO. Root signature is fixed
    // to a single UAV descriptor table (u0) - the only case that exists is
    // a full-screen pass writing directly to the back buffer.
    //
    // TODO(OM): generalize the root signature (more UAVs, CBVs, SRVs) once
    // a second compute pass needs a different layout.
    struct ComputePipelineDesc
    {
        const CompiledShader* computeShader = nullptr;
    };
}
