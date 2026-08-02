#include "BackgroundPassDX12.h"

#include "DeviceDX12.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "PipelineDX12.h"
#include "Renderer/Pipeline.h"
#include "Renderer/Shader.h"

#include <d3d12.h>

namespace
{
    Renderer::PipelineHandle g_pipelineHandle;
}

namespace Renderer
{
    void BackgroundPassDX12::Init()
    {
        CompiledShader computeShader = Shader::CompileShader("data/shaders/Background.hlsl", "CSMain", "cs_6_0");
        OMD_ASSERT(!computeShader.bytecode.empty(), "Background pass compute shader failed to compile");

        ComputePipelineDesc pipelineDesc;
        pipelineDesc.computeShader = &computeShader;
        g_pipelineHandle = Pipeline::CreateCompute(pipelineDesc);

        Foundation::Log::Write(Foundation::Log::Severity::Info, "Renderer", "Background pass initialized");
    }

    void BackgroundPassDX12::Shutdown()
    {
        // No-op: the PSO/root signature live in Pipeline's own registry,
        // released via its own Shutdown(). Kept for shape consistency with
        // other passes, which may own resources directly.
    }

    void BackgroundPassDX12::Render(const BackgroundRenderDesc&)
    {
        ID3D12GraphicsCommandList* cmdList = DeviceDX12::GetCommandList();

        cmdList->SetComputeRootSignature(PipelineDX12::GetRootSignature(g_pipelineHandle));
        cmdList->SetPipelineState(PipelineDX12::GetPSO(g_pipelineHandle));

        D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = {};
        uavHandle.ptr = DeviceDX12::GetComputeTargetUAV();
        cmdList->SetComputeRootDescriptorTable(0, uavHandle);

        constexpr UINT kThreadGroupSize = 8;
        const UINT dispatchX = (DeviceDX12::GetWidth() + kThreadGroupSize - 1) / kThreadGroupSize;
        const UINT dispatchY = (DeviceDX12::GetHeight() + kThreadGroupSize - 1) / kThreadGroupSize;
        cmdList->Dispatch(dispatchX, dispatchY, 1);
    }
}
