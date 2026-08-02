#pragma once

#include "Renderer/CompiledShader.h"

namespace Renderer
{
    struct ShaderDX12
    {
        // entryPoint e.g. "VSMain", target e.g. "vs_6_0"/"ps_6_0" (DXC profile).
        // Returns a CompiledShader with an empty bytecode vector on failure -
        // the compile error is logged, callers decide whether that's fatal.
        static CompiledShader CompileShader(const char* path, const char* entryPoint, const char* target);
    };
}
