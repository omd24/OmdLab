#pragma once

#include "Renderer/SkinnedMeshDrawItem.h"

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace Asset
{
    struct Model;
    struct Clip;
    struct Skin;
}

namespace Engine
{
    // Evaluates the world transform of every node in the model by walking the full hierarchy
    // from its scene roots down through every descendant, exactly once - the same walk
    // CreateStaticMeshDrawItems (ModelResources.h) used to do inline for bind-pose rendering,
    // factored out here so the animation runtime can reuse it with animated local transforms
    // instead of always using each node's authored rest pose.
    //
    // clip == nullptr (the default): bind pose - every node's own authored rest-pose local
    // transform is used directly, unchanged from before this function existed.
    //
    // clip != nullptr: for each node, any TRS component the clip has a channel targeting on
    // that node is sampled at timeSeconds (wrapped/looped against the clip's own duration)
    // instead of using the node's rest-pose value for that component; any component the clip
    // does NOT animate still falls back to the node's rest pose. This is why bind pose is the
    // degenerate case of the same walk rather than a separate code path.
    //
    // Returned in DirectXMath's natural row-vector form (v' = v * M), NOT transposed for GPU
    // row_major storage - this is raw material for further CPU-side composition (skinning
    // matrices need to multiply/invert these further), which transposing early would break.
    // Callers that need a GPU-ready matrix transpose it themselves at the point of upload, same
    // as CreateStaticMeshDrawItems already does.
    //
    // Indexed the same way as Asset::Model::nodes - result[i] is node i's world transform.
    std::vector<DirectX::XMMATRIX> EvaluateNodeWorldTransforms(
        const Asset::Model& model, const DirectX::XMMATRIX& rootTransform = DirectX::XMMatrixIdentity(),
        const Asset::Clip* clip = nullptr, float timeSeconds = 0.0f);

    // Per-joint matrix that carries a vertex from the skinned mesh's own local space into
    // posed local space, ready to combine with the mesh node's own world transform exactly the
    // same way an unskinned vertex already does (worldPos = mul(World, mul(skin, localPos))).
    //
    // Factor order is IBM_i * jointWorld_i * inverse(meshWorld) - the reverse of the glTF spec's
    // own column-vector transcription (inverse(meshWorld) * jointWorld_i * IBM_i), because this
    // codebase's row-vector convention (v' = v*M, leftmost applied first) always reverses
    // right-to-left column-vector chains. inverseBindMatrix_i already carries the same
    // column-major-to-row-major transpose every other imported matrix does (GltfImporter.cpp's
    // BuildSkins), so no extra per-matrix transpose is needed here - only the factor order
    // needed correcting.
    //
    // nodeWorldTransforms must be indexed the same way EvaluateNodeWorldTransforms returns them
    // (by Model::nodes index), evaluated at whatever pose (bind or animated) the caller wants
    // skinned - and, importantly, evaluated WITHOUT any caller-side corrective rootTransform
    // (see meshWorldTransform below for why).
    //
    // meshWorldTransform is the mesh node's own global transform to normalize against, taken as
    // an explicit parameter rather than looked up internally, because it is not always simply
    // "the mesh node's evaluated world transform" per the glTF spec's own formula: this
    // project's one skinned asset (StickMan.glb, a Sketchfab FBX-to-glTF conversion already
    // known for one scale quirk - see CreateStaticMeshDrawItems's rootTransform parameter) turns
    // out to have baked its inverseBindMatrices assuming the mesh node's global transform was
    // Identity, ignoring the same ancestor scale node responsible for that other quirk. Verified
    // on the CPU by checking that the weighted sum of skinning matrices collapses to Identity
    // at bind pose only when meshWorldTransform = Identity and nodeWorldTransforms is evaluated
    // with no corrective rootTransform; using the mesh node's real (rootTransform-corrected)
    // world transform here reintroduces exactly that correction as leftover scale,
    // since there is then nothing of matching magnitude on the other side of the multiplication
    // to cancel it against. The corrective rootTransform is still fully applied - just applied
    // once, separately, to the mesh's own "World" CBV at render time (same as the rigid/static
    // case), never to the skinning-matrix computation itself.
    //
    // Returned matrices are NOT transposed for GPU row_major storage, same as
    // EvaluateNodeWorldTransforms - transpose each one individually at the point of upload
    // (safe here since the GPU's per-vertex blend is a weighted sum, and transpose distributes
    // over addition; it would NOT be safe to transpose an intermediate factor before this
    // multiplication).
    std::vector<DirectX::XMMATRIX> ComputeSkinningMatrices(
        const Asset::Skin& skin, const std::vector<DirectX::XMMATRIX>& nodeWorldTransforms,
        const DirectX::XMMATRIX& meshWorldTransform);

    // Per-frame pose update: runs FK (EvaluateNodeWorldTransforms) and skinning
    // (ComputeSkinningMatrices) for one clip/time, then Buffer::Update()s item's own
    // bonePaletteBuffer in place - the persistent buffer Engine::CreateSkinnedMeshDrawItems
    // creates once at load. Deliberately separate from that one-time setup: calling this every
    // frame is exactly the point (time changes every frame), whereas re-running the one-time
    // setup every frame would re-upload the vertex/index buffers too, leaking new GPU resources
    // each time.
    //
    // rootTransform and meshWorldTransform are threaded straight through to
    // EvaluateNodeWorldTransforms and ComputeSkinningMatrices respectively - see
    // ComputeSkinningMatrices's own comment for why these are independent, caller-controlled
    // parameters rather than one shared value derived internally.
    //
    // Joints beyond skin.joints.size() (up to Renderer::kMaxSkinJoints) are left at Identity -
    // harmless since no vertex references them (see Renderer::kMaxSkinJoints's own comment for
    // the capacity/assert contract Engine::CreateSkinnedMeshDrawItems already enforces).
    void UpdateSkinnedPose(
        const Asset::Model& model, const Asset::Skin& skin, const Asset::Clip& clip, float timeSeconds,
        const DirectX::XMMATRIX& rootTransform, const DirectX::XMMATRIX& meshWorldTransform, Renderer::SkinnedMeshDrawItem& item);
}
