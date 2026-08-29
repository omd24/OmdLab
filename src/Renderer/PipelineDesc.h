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
    // vertex layout, plus how many root constant-buffer parameters the
    // shaders expect (bound at b0, b1, ... in that order, visible to both
    // vertex and pixel stages - no per-stage split needed at this scale).
    struct GraphicsPipelineDesc
    {
        const CompiledShader* vertexShader = nullptr;
        const CompiledShader* pixelShader = nullptr;
        const VertexAttribute* vertexAttributes = nullptr;
        unsigned int vertexAttributeCount = 0;
        unsigned int constantBufferCount = 0;

        // 0 = no SRV descriptor table (and no sampler). >0 = one descriptor table at t0..,
        // this many descriptors, plus one static bilinear-wrap sampler at s0 - the one shape
        // needed so far (a material's base color texture, srvCount == 1 - the table's GPU
        // base handle gets re-pointed at whichever texture's descriptor a given draw needs,
        // via SetGraphicsRootDescriptorTable, rather than one large persistent table covering
        // every texture at once).
        unsigned int srvCount = 0;

        // Off by default - the 2D-ish flat-color triangle this project started
        // with has no use for it. Real 3D geometry with self-occlusion needs it on.
        bool depthTestEnabled = false;

        // On by default (standard back-face culling). Off entirely disables culling for
        // every draw using this PSO - a pass-wide switch, not per-material; a pass whose
        // source data mixes single-sided and double-sided materials (see
        // Asset::Material::doubleSided) needs two PSOs and to pick per draw item, not this.
        bool cullBackFaces = true;

        // Off by default - every existing PSO draws fully opaque, unblended. On enables
        // standard straight-alpha "over" blending so a draw's own alpha (from its texture
        // and/or a per-item tint - see StaticMeshDrawItem::tintBuffer) controls how much of
        // what's already in the render target shows through. Per-pass switch, not
        // per-draw-item - a pass mixing opaque and transparent content needs two PSOs and to
        // partition its own draw list (see StaticMeshPassDX12), not a bool here.
        bool alphaBlendEnabled = false;

        // On by default (matches every existing PSO's implicit behavior before this field
        // existed). Off for a blended PSO whose draws should still depth-TEST against opaque
        // content already in the depth buffer, but not depth-WRITE - this project relies on
        // draw-list order, not per-pixel order-independent transparency, for overlapping
        // transparent draws.
        bool depthWriteEnabled = true;

        // Triangle by default - every pass so far draws triangles. DebugDrawPass is the first
        // consumer of Line (wireframe boxes), hence this being caller-configurable at all rather
        // than hardcoded like it was before that pass existed.
        enum class Topology { Triangle, Line };
        Topology topology = Topology::Triangle;
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
