#pragma once

#include "CompiledShader.h"
#include "PlatformMacros.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/ShaderDX12.h"
#endif

namespace Renderer
{
    // Compiles HLSL source to shader bytecode via DXC.
    // Backend-agnostic front layer - see PlatformMacros.h.
    //
    // All shader loads must go through this one function - the hot-reload
    // seam. Callers never invoke a compiler API directly, so hot-reload can
    // later be added by changing what happens inside this call (or adding a
    // caller that re-invokes it periodically) instead of touching every
    // place a shader gets loaded. Repeated calls with an unchanged source
    // file return a cached result instead of recompiling.
    struct Shader : public OMD_GFX_CLASS(Shader)
    {
        static CompiledShader CompileShader(const char* path, const char* entryPoint, const char* target)
        {
            return OMD_GFX_CALL(Shader, CompileShader(path, entryPoint, target));
        }
    };
}
