#pragma once

#include <cstdint>
#include <vector>

namespace Renderer
{
    // Compiled shader bytecode, backend-agnostic (currently DXIL via DXC).
    // Its own header so both the front layer (Shader.h) and the DX12
    // implementation (dx12/ShaderDX12.h) can see the full definition
    // without including each other.
    struct CompiledShader
    {
        std::vector<uint8_t> bytecode;
    };
}
