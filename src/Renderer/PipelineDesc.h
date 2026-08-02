#pragma once

#include "CompiledShader.h"
#include "VertexAttribute.h"

namespace Renderer
{
    // Opaque handle into Pipeline's internal registry. Its own header (like
    // PipelineDesc below) so both the front layer and the DX12
    // implementation can see the full definition without including
    // each other.
    struct PipelineHandle
    {
        int index = -1;
    };

    // Describes one root signature + PSO combination. Shader bytecode and
    // vertex layout only for now - no resource bindings beyond the input
    // assembler, since nothing built against this yet needs any (see
    // DESIGN.md's render pass convention section).
    struct PipelineDesc
    {
        const CompiledShader* vertexShader = nullptr;
        const CompiledShader* pixelShader = nullptr;
        const VertexAttribute* vertexAttributes = nullptr;
        unsigned int vertexAttributeCount = 0;
    };
}
