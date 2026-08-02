#include "ShaderDX12.h"

#include "Foundation/Debug.h"
#include "Foundation/Log.h"

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

// dxcapi.h needs full COM support (IUnknown, BSTR) that WIN32_LEAN_AND_MEAN
// trims out of Windows.h.
#include <oleauto.h>
#include <unknwn.h>

#include <dxcapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    struct CacheEntry
    {
        std::filesystem::file_time_type mtime;
        Renderer::CompiledShader shader;
    };

    // Keyed on path+entryPoint+target so one HLSL file with multiple entry
    // points (e.g. VSMain/PSMain in the same file) caches independently.
    // Only successful compiles are cached, so a failing edit gets retried
    // every call instead of getting stuck - see the hot-reload plan.
    std::unordered_map<std::string, CacheEntry> g_shaderCache;

    void CheckHr(HRESULT hr, const char* what)
    {
        OMD_ASSERT(SUCCEEDED(hr), "%s failed with HRESULT 0x%08lX", what, static_cast<unsigned long>(hr));
    }

    std::wstring ToWide(const char* narrow)
    {
        wchar_t buffer[256] = {};
        size_t convertedChars = 0;
        mbstowcs_s(&convertedChars, buffer, std::size(buffer), narrow, _TRUNCATE);
        return std::wstring(buffer);
    }
}

namespace Renderer
{
    CompiledShader ShaderDX12::CompileShader(const char* path, const char* entryPoint, const char* target)
    {
        std::error_code fileStatError;
        const std::filesystem::file_time_type currentMtime = std::filesystem::last_write_time(path, fileStatError);
        OMD_ASSERT(!fileStatError, "Shader source not found: %s", path);

        const std::string cacheKey = std::string(path) + "|" + entryPoint + "|" + target;
        auto cached = g_shaderCache.find(cacheKey);
        if (cached != g_shaderCache.end() && cached->second.mtime == currentMtime)
        {
            return cached->second.shader;
        }

        ComPtr<IDxcUtils> utils;
        CheckHr(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcCreateInstance(CLSID_DxcUtils)");

        ComPtr<IDxcCompiler3> compiler;
        CheckHr(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCreateInstance(CLSID_DxcCompiler)");

        const std::wstring widePath = ToWide(path);
        ComPtr<IDxcBlobEncoding> sourceBlob;
        CheckHr(utils->LoadFile(widePath.c_str(), nullptr, &sourceBlob), "IDxcUtils::LoadFile");

        DxcBuffer sourceBuffer = {};
        sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
        sourceBuffer.Size = sourceBlob->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_ACP;

        const std::wstring wideEntryPoint = ToWide(entryPoint);
        const std::wstring wideTarget = ToWide(target);
        LPCWSTR arguments[] = { widePath.c_str(), L"-E", wideEntryPoint.c_str(), L"-T", wideTarget.c_str() };

        ComPtr<IDxcIncludeHandler> includeHandler;
        CheckHr(utils->CreateDefaultIncludeHandler(&includeHandler), "IDxcUtils::CreateDefaultIncludeHandler");

        ComPtr<IDxcResult> result;
        CheckHr(
            compiler->Compile(
                &sourceBuffer, arguments, static_cast<UINT32>(std::size(arguments)), includeHandler.Get(), IID_PPV_ARGS(&result)),
            "IDxcCompiler3::Compile");

        ComPtr<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0)
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Warning, "Renderer", "Shader compile messages (%s, %s): %s", path, entryPoint,
                errors->GetStringPointer());
        }

        HRESULT compileStatus = S_OK;
        result->GetStatus(&compileStatus);
        if (FAILED(compileStatus))
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Error, "Renderer", "Shader compile failed: %s [%s, %s]", path, entryPoint, target);
            return CompiledShader{};
        }

        ComPtr<IDxcBlob> objectBlob;
        CheckHr(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objectBlob), nullptr), "IDxcResult::GetOutput(DXC_OUT_OBJECT)");

        CompiledShader compiledShader;
        const uint8_t* objectBytes = static_cast<const uint8_t*>(objectBlob->GetBufferPointer());
        compiledShader.bytecode.assign(objectBytes, objectBytes + objectBlob->GetBufferSize());

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Renderer", "Compiled shader: %s [%s, %s], %zu bytes", path, entryPoint, target,
            compiledShader.bytecode.size());

        g_shaderCache[cacheKey] = CacheEntry{ currentMtime, compiledShader };
        return compiledShader;
    }
}
