#pragma once

namespace Renderer
{
    // Backend-agnostic description of one vertex input element - the DX12
    // backend translates this into a D3D12_INPUT_ELEMENT_DESC.
    struct VertexAttribute
    {
        const char* semanticName = nullptr;
        unsigned int semanticIndex = 0;
        unsigned int floatCount = 0; // 1-4 -> R32_FLOAT .. R32G32B32A32_FLOAT
        unsigned int offset = 0;     // byte offset within the vertex struct
    };
}
