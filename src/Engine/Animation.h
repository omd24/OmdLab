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
    // One node's local-space transform, as separate T/R/S so poses can be blended
    // component-wise (lerp T/S, slerp R) - a matrix can't be meaningfully interpolated.
    // Layout matches Asset::Node's own fields.
    struct NodeLocal
    {
        DirectX::XMFLOAT3 translation = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f }; // Identity quaternion.
        DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };
    };

    // A full-skeleton local-space pose - one NodeLocal per Asset::Model node, indexed the same
    // way. The unit the animation runtime samples, blends, and layers before any hierarchy
    // composition happens. An empty `nodes` means "not sampled" (ComposePoseToWorld then falls
    // back to the model's rest pose).
    struct Pose
    {
        std::vector<NodeLocal> nodes;
    };

    // Sample every animation channel of `clip` at timeSeconds into a local-space Pose. A node
    // the clip doesn't animate keeps the model's own rest-pose local TRS; a node the clip
    // animates on only some components keeps its rest value for the others - same fallback rule
    // the fused walk always had. clip == nullptr yields the pure bind/rest pose. timeSeconds is
    // wrapped/looped against the clip's own duration, unchanged.
    //
    // Per-node independent (no hierarchy), so this is a flat loop over model.nodes.
    Pose SampleClipPose(const Asset::Model& model, const Asset::Clip* clip, float timeSeconds);

    // Per-node blend of two local-space poses: lerp translation/scale, shortest-path slerp
    // rotation, weight `t` in [0,1] (clamped; t<=0 returns a, t>=1 returns b exactly). The two
    // poses must be the same size (both indexed like Asset::Model::nodes); a mismatch returns
    // whichever is non-empty, or `a`. This is all a plain crossfade needs; per-node masking is
    // BlendMasked below.
    Pose Blend(const Pose& a, const Pose& b, float t);

    // Per-node blend where the weight varies by node: the effective weight for node i is
    // `clamp(weights[i] * weightScale, 0, 1)` - how much of `overlay` node i replaces `base`
    // node i (0 = keep base, 1 = take overlay, between = lerp T/S + shortest-path slerp R, same
    // math as Blend). `weights` must be indexed like Asset::Model::nodes and match `base`'s
    // size; a size mismatch (or empty weights) returns `base` unchanged. `weightScale` is a
    // single per-frame dial over the whole mask (e.g. ramp an overlay in/out). The primitive a
    // partial-body or additive layer is built on.
    Pose BlendMasked(const Pose& base, const Pose& overlay, const std::vector<float>& weights, float weightScale = 1.0f);

    // True only if every T/R/S component of every node is a finite number. A non-finite pose
    // (a NaN/Inf leaking in from a degenerate clip quaternion, a poisoned blend accumulator, a
    // bad crossfade source) composes to a garbage skinning palette and renders the mesh as a
    // collapsed point - i.e. invisible - and, if the bad value feeds back into an accumulator,
    // stays invisible. Callers use this as a last-line check before handing a pose to skinning,
    // substituting the rest pose for a frame instead (see EvaluateAnimator).
    bool PoseIsFinite(const Pose& pose);

    // Compose a local-space Pose down the node hierarchy: world_i = local_i * parentWorld, each
    // scene root starting from rootTransform. `pose` must be indexed like Asset::Model::nodes
    // (SampleClipPose's output); an empty or wrong-sized pose uses each node's rest-pose local
    // TRS instead. Same row-vector math and same NOT-transposed-for-GPU output contract as
    // EvaluateNodeWorldTransforms below (which is now literally
    // ComposePoseToWorld(model, SampleClipPose(model, clip, t), rootTransform)).
    std::vector<DirectX::XMMATRIX> ComposePoseToWorld(
        const Asset::Model& model, const Pose& pose, const DirectX::XMMATRIX& rootTransform = DirectX::XMMatrixIdentity());

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

    // Per-frame pose update: composes an already-sampled local-space Pose to world
    // (ComposePoseToWorld), runs skinning (ComputeSkinningMatrices), then Buffer::Update()s
    // item's own bonePaletteBuffer in place - the persistent buffer
    // Engine::CreateSkinnedMeshDrawItems creates once at load. Deliberately separate from that
    // one-time setup: calling this every frame is exactly the point, whereas re-running the
    // one-time setup would re-upload the vertex/index buffers too and leak GPU resources.
    //
    // Takes a Pose (from SampleClipPose or EvaluateAnimator) rather than a clip+time, so
    // blending/layering happens upstream in the animation runtime, not here. An empty Pose
    // composes the model's rest pose.
    //
    // rootTransform and meshWorldTransform are threaded straight through to ComposePoseToWorld
    // and ComputeSkinningMatrices respectively - see ComputeSkinningMatrices's own comment for
    // why these are independent, caller-controlled parameters, not one shared value.
    //
    // Joints beyond skin.joints.size() (up to Renderer::kMaxSkinJoints) are left at Identity -
    // harmless since no vertex references them (see Renderer::kMaxSkinJoints's own comment for
    // the capacity/assert contract Engine::CreateSkinnedMeshDrawItems already enforces).
    void UpdateSkinnedPose(
        const Asset::Model& model, const Asset::Skin& skin, const Pose& pose, const DirectX::XMMATRIX& rootTransform,
        const DirectX::XMMATRIX& meshWorldTransform, Renderer::SkinnedMeshDrawItem& item);
}
