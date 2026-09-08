#include "Animation.h"

#include "BlendSpace.h"

#include "Asset/Clip.h"
#include "Asset/Model.h"
#include "Asset/Skin.h"
#include "Renderer/Buffer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace
{
    float WrapTime(float timeSeconds, float durationSeconds)
    {
        if (durationSeconds <= 0.0f)
        {
            return 0.0f;
        }
        float wrapped = fmodf(timeSeconds, durationSeconds);
        if (wrapped < 0.0f)
        {
            wrapped += durationSeconds;
        }
        return wrapped;
    }

    // Finds the pair of keyframe indices bracketing time (and the [0,1] blend factor between
    // them). Linear scan rather than binary search - channel key counts here are in the dozens,
    // not thousands, so this isn't worth the added complexity. Clamps rather than extrapolates
    // for time outside the channel's own key range (its last key can sit slightly before the
    // clip's own overall duration).
    struct KeyBracket
    {
        size_t index0 = 0;
        size_t index1 = 0;
        float blend = 0.0f;
    };

    KeyBracket FindBracket(const std::vector<float>& keyTimes, float time)
    {
        if (keyTimes.size() <= 1 || time <= keyTimes.front())
        {
            return KeyBracket{ 0, 0, 0.0f };
        }
        if (time >= keyTimes.back())
        {
            const size_t last = keyTimes.size() - 1;
            return KeyBracket{ last, last, 0.0f };
        }
        for (size_t i = 0; i + 1 < keyTimes.size(); ++i)
        {
            if (time >= keyTimes[i] && time <= keyTimes[i + 1])
            {
                const float span = keyTimes[i + 1] - keyTimes[i];
                const float blend = span > 0.0f ? (time - keyTimes[i]) / span : 0.0f;
                return KeyBracket{ i, i + 1, blend };
            }
        }
        const size_t last = keyTimes.size() - 1;
        return KeyBracket{ last, last, 0.0f };
    }

    DirectX::XMVECTOR SampleVector(const Asset::AnimationChannel& channel, float time)
    {
        const KeyBracket bracket = FindBracket(channel.keyTimes, time);
        const DirectX::XMVECTOR a = DirectX::XMLoadFloat4(&channel.keyValues[bracket.index0]);
        const DirectX::XMVECTOR b = DirectX::XMLoadFloat4(&channel.keyValues[bracket.index1]);
        return DirectX::XMVectorLerp(a, b, bracket.blend);
    }

    DirectX::XMVECTOR SampleQuaternion(const Asset::AnimationChannel& channel, float time)
    {
        const KeyBracket bracket = FindBracket(channel.keyTimes, time);
        const DirectX::XMVECTOR a = DirectX::XMLoadFloat4(&channel.keyValues[bracket.index0]);
        DirectX::XMVECTOR b = DirectX::XMLoadFloat4(&channel.keyValues[bracket.index1]);
        // Quaternion double cover: q and -q represent the identical rotation, so adjacent
        // keyframes can flip sign for any rotation delta, not just a near-antipodal one.
        // Blending across an unnoticed sign flip interpolates "the long way around" and
        // produces a visible snap - this check is mandatory for every pair of keys sampled,
        // not an edge case to special-case only when it looks wrong.
        if (DirectX::XMVectorGetX(DirectX::XMVector4Dot(a, b)) < 0.0f)
        {
            b = DirectX::XMVectorNegate(b);
        }
        return DirectX::XMQuaternionNormalize(DirectX::XMVectorLerp(a, b, bracket.blend));
    }

    // Rest-pose local TRS for one node, as the XMVECTORs the sampling code works in. Every
    // sampled component overwrites the matching lane below.
    void LoadRestLocal(const Asset::Node& node, DirectX::XMVECTOR& scale, DirectX::XMVECTOR& rotation, DirectX::XMVECTOR& translation)
    {
        scale = DirectX::XMLoadFloat3(&node.scale);
        rotation = DirectX::XMLoadFloat4(&node.rotation);
        translation = DirectX::XMLoadFloat3(&node.translation);
    }

    // Recursive hierarchy composition: world_i = local_i * parentWorld. Reads local TRS from
    // `pose` (indexed by node), or the node's rest pose when `pose` is empty/wrong-sized. Same
    // "local = Scale*Rotate*Translate, world = local*parentWorld" row-vector math the fused
    // walk always used - this is that walk minus the sampling.
    void ComposeNode(
        const Asset::Model& model, int32_t nodeIndex, const DirectX::XMMATRIX& parentWorld, const Engine::Pose& pose,
        bool poseUsable, std::vector<DirectX::XMMATRIX>& outWorldTransforms)
    {
        const Asset::Node& node = model.nodes[nodeIndex];

        DirectX::XMVECTOR scale;
        DirectX::XMVECTOR rotation;
        DirectX::XMVECTOR translation;
        if (poseUsable)
        {
            const Engine::NodeLocal& nl = pose.nodes[static_cast<size_t>(nodeIndex)];
            scale = DirectX::XMLoadFloat3(&nl.scale);
            rotation = DirectX::XMLoadFloat4(&nl.rotation);
            translation = DirectX::XMLoadFloat3(&nl.translation);
        }
        else
        {
            LoadRestLocal(node, scale, rotation, translation);
        }

        const DirectX::XMMATRIX local =
            DirectX::XMMatrixScalingFromVector(scale) * DirectX::XMMatrixRotationQuaternion(rotation) *
            DirectX::XMMatrixTranslationFromVector(translation);
        const DirectX::XMMATRIX world = local * parentWorld;
        outWorldTransforms[static_cast<size_t>(nodeIndex)] = world;

        for (int32_t childIndex : node.childNodeIndices)
        {
            ComposeNode(model, childIndex, world, pose, poseUsable, outWorldTransforms);
        }
    }
}

namespace Engine
{
    Pose SampleClipPose(const Asset::Model& model, const Asset::Clip* clip, float timeSeconds)
    {
        std::unordered_map<int32_t, std::vector<const Asset::AnimationChannel*>> channelsByNode;
        if (clip != nullptr)
        {
            for (const Asset::AnimationChannel& channel : clip->channels)
            {
                if (channel.targetNodeIndex != Asset::kInvalidIndex)
                {
                    channelsByNode[channel.targetNodeIndex].push_back(&channel);
                }
            }
        }

        const float time = clip != nullptr ? WrapTime(timeSeconds, clip->durationSeconds) : 0.0f;

        Pose pose;
        pose.nodes.resize(model.nodes.size());
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        {
            const Asset::Node& node = model.nodes[nodeIndex];

            DirectX::XMVECTOR scale;
            DirectX::XMVECTOR rotation;
            DirectX::XMVECTOR translation;
            LoadRestLocal(node, scale, rotation, translation);

            if (clip != nullptr)
            {
                const auto channelsForThisNode = channelsByNode.find(static_cast<int32_t>(nodeIndex));
                if (channelsForThisNode != channelsByNode.end())
                {
                    for (const Asset::AnimationChannel* channel : channelsForThisNode->second)
                    {
                        switch (channel->path)
                        {
                            case Asset::AnimationPath::Translation: translation = SampleVector(*channel, time); break;
                            case Asset::AnimationPath::Scale: scale = SampleVector(*channel, time); break;
                            case Asset::AnimationPath::Rotation: rotation = SampleQuaternion(*channel, time); break;
                        }
                    }
                }
            }

            NodeLocal& out = pose.nodes[nodeIndex];
            DirectX::XMStoreFloat3(&out.translation, translation);
            DirectX::XMStoreFloat4(&out.rotation, rotation);
            DirectX::XMStoreFloat3(&out.scale, scale);
        }
        return pose;
    }

    Pose Blend(const Pose& a, const Pose& b, float t)
    {
        if (a.nodes.empty())
        {
            return b;
        }
        if (b.nodes.empty() || a.nodes.size() != b.nodes.size())
        {
            return a;
        }
        if (t <= 0.0f)
        {
            return a;
        }
        if (t >= 1.0f)
        {
            return b;
        }

        Pose out;
        out.nodes.resize(a.nodes.size());
        for (size_t i = 0; i < a.nodes.size(); ++i)
        {
            const NodeLocal& na = a.nodes[i];
            const NodeLocal& nb = b.nodes[i];

            const DirectX::XMVECTOR ta = DirectX::XMLoadFloat3(&na.translation);
            const DirectX::XMVECTOR tb = DirectX::XMLoadFloat3(&nb.translation);
            const DirectX::XMVECTOR sa = DirectX::XMLoadFloat3(&na.scale);
            const DirectX::XMVECTOR sb = DirectX::XMLoadFloat3(&nb.scale);
            DirectX::XMVECTOR ra = DirectX::XMLoadFloat4(&na.rotation);
            DirectX::XMVECTOR rb = DirectX::XMLoadFloat4(&nb.rotation);

            // Shortest-path: XMQuaternionSlerp does not flip for the double cover itself, so
            // negate one input when the pair points to opposite hemispheres (same fix
            // SampleQuaternion applies to its own lerp).
            if (DirectX::XMVectorGetX(DirectX::XMVector4Dot(ra, rb)) < 0.0f)
            {
                rb = DirectX::XMVectorNegate(rb);
            }

            NodeLocal& no = out.nodes[i];
            DirectX::XMStoreFloat3(&no.translation, DirectX::XMVectorLerp(ta, tb, t));
            DirectX::XMStoreFloat3(&no.scale, DirectX::XMVectorLerp(sa, sb, t));
            DirectX::XMStoreFloat4(&no.rotation, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(ra, rb, t)));
        }
        return out;
    }

    Pose BlendMasked(const Pose& base, const Pose& overlay, const std::vector<float>& weights, float weightScale)
    {
        if (base.nodes.empty() || overlay.nodes.size() != base.nodes.size() || weights.size() != base.nodes.size())
        {
            return base;
        }

        Pose out;
        out.nodes.resize(base.nodes.size());
        for (size_t i = 0; i < base.nodes.size(); ++i)
        {
            const float w = std::clamp(weights[i] * weightScale, 0.0f, 1.0f);
            if (w <= 0.0f)
            {
                out.nodes[i] = base.nodes[i];
                continue;
            }
            if (w >= 1.0f)
            {
                out.nodes[i] = overlay.nodes[i];
                continue;
            }

            const NodeLocal& na = base.nodes[i];
            const NodeLocal& nb = overlay.nodes[i];

            const DirectX::XMVECTOR ta = DirectX::XMLoadFloat3(&na.translation);
            const DirectX::XMVECTOR tb = DirectX::XMLoadFloat3(&nb.translation);
            const DirectX::XMVECTOR sa = DirectX::XMLoadFloat3(&na.scale);
            const DirectX::XMVECTOR sb = DirectX::XMLoadFloat3(&nb.scale);
            DirectX::XMVECTOR ra = DirectX::XMLoadFloat4(&na.rotation);
            DirectX::XMVECTOR rb = DirectX::XMLoadFloat4(&nb.rotation);

            if (DirectX::XMVectorGetX(DirectX::XMVector4Dot(ra, rb)) < 0.0f)
            {
                rb = DirectX::XMVectorNegate(rb);
            }

            NodeLocal& no = out.nodes[i];
            DirectX::XMStoreFloat3(&no.translation, DirectX::XMVectorLerp(ta, tb, w));
            DirectX::XMStoreFloat3(&no.scale, DirectX::XMVectorLerp(sa, sb, w));
            DirectX::XMStoreFloat4(&no.rotation, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(ra, rb, w)));
        }
        return out;
    }

    bool PoseIsFinite(const Pose& pose)
    {
        for (const NodeLocal& n : pose.nodes)
        {
            if (!std::isfinite(n.translation.x) || !std::isfinite(n.translation.y) || !std::isfinite(n.translation.z) ||
                !std::isfinite(n.rotation.x) || !std::isfinite(n.rotation.y) || !std::isfinite(n.rotation.z) ||
                !std::isfinite(n.rotation.w) || !std::isfinite(n.scale.x) || !std::isfinite(n.scale.y) || !std::isfinite(n.scale.z))
            {
                return false;
            }
        }
        return true;
    }

    namespace
    {
        const Asset::Clip* ClipAt(const std::vector<Asset::Clip>& clips, int32_t index)
        {
            return (index >= 0 && index < static_cast<int32_t>(clips.size())) ? &clips[static_cast<size_t>(index)] : nullptr;
        }

        // Index of the first sample with param >= `param`, i.e. the upper end of the bracket.
        // 0 means "at or below the first sample", samples.size() means "at or above the last".
        size_t UpperBracket(const BlendSpace1D& space, float param)
        {
            size_t i = 0;
            while (i < space.samples.size() && space.samples[i].param < param)
            {
                ++i;
            }
            return i;
        }
    }

    Pose EvaluateBlendSpace1D(
        const BlendSpace1D& space, float param, float phase01, const Asset::Model& model, const std::vector<Asset::Clip>& clips)
    {
        if (space.samples.empty())
        {
            return SampleClipPose(model, nullptr, 0.0f);
        }

        const auto sampleAt = [&](const BlendSpace1DSample& s) -> Pose
        {
            const Asset::Clip* clip = ClipAt(clips, s.clipIndex);
            const float duration = clip != nullptr ? clip->durationSeconds : 0.0f;
            return SampleClipPose(model, clip, phase01 * duration);
        };

        const size_t upper = UpperBracket(space, param);
        if (upper == 0)
        {
            return sampleAt(space.samples.front());
        }
        if (upper >= space.samples.size())
        {
            return sampleAt(space.samples.back());
        }

        const BlendSpace1DSample& lo = space.samples[upper - 1];
        const BlendSpace1DSample& hi = space.samples[upper];
        const float span = hi.param - lo.param;
        const float w = span > 0.0f ? std::clamp((param - lo.param) / span, 0.0f, 1.0f) : 0.0f;
        return Blend(sampleAt(lo), sampleAt(hi), w);
    }

    float BlendSpace1DPhaseRate(const BlendSpace1D& space, float param, const std::vector<Asset::Clip>& clips)
    {
        const auto rateOf = [&](const BlendSpace1DSample& s) -> float
        {
            const Asset::Clip* clip = ClipAt(clips, s.clipIndex);
            const float duration = clip != nullptr ? clip->durationSeconds : 0.0f;
            return duration > 0.0f ? 1.0f / duration : 0.0f;
        };

        if (space.samples.empty())
        {
            return 0.0f;
        }

        const size_t upper = UpperBracket(space, param);
        if (upper == 0)
        {
            return rateOf(space.samples.front());
        }
        if (upper >= space.samples.size())
        {
            return rateOf(space.samples.back());
        }

        const BlendSpace1DSample& lo = space.samples[upper - 1];
        const BlendSpace1DSample& hi = space.samples[upper];
        const float span = hi.param - lo.param;
        const float w = span > 0.0f ? std::clamp((param - lo.param) / span, 0.0f, 1.0f) : 0.0f;
        return rateOf(lo) + (rateOf(hi) - rateOf(lo)) * w;
    }

    std::vector<DirectX::XMMATRIX> ComposePoseToWorld(
        const Asset::Model& model, const Pose& pose, const DirectX::XMMATRIX& rootTransform)
    {
        const bool poseUsable = pose.nodes.size() == model.nodes.size();

        std::vector<DirectX::XMMATRIX> worldTransforms(model.nodes.size(), DirectX::XMMatrixIdentity());
        for (int32_t rootIndex : model.rootNodeIndices)
        {
            ComposeNode(model, rootIndex, rootTransform, pose, poseUsable, worldTransforms);
        }
        return worldTransforms;
    }

    std::vector<DirectX::XMMATRIX> EvaluateNodeWorldTransforms(
        const Asset::Model& model, const DirectX::XMMATRIX& rootTransform, const Asset::Clip* clip, float timeSeconds)
    {
        return ComposePoseToWorld(model, SampleClipPose(model, clip, timeSeconds), rootTransform);
    }

    std::vector<DirectX::XMMATRIX> ComputeSkinningMatrices(
        const Asset::Skin& skin, const std::vector<DirectX::XMMATRIX>& nodeWorldTransforms, const DirectX::XMMATRIX& meshWorldTransform)
    {
        const DirectX::XMMATRIX inverseMeshWorld = DirectX::XMMatrixInverse(nullptr, meshWorldTransform);

        std::vector<DirectX::XMMATRIX> skinningMatrices(skin.joints.size());
        for (size_t i = 0; i < skin.joints.size(); ++i)
        {
            const Asset::Joint& joint = skin.joints[i];
            const DirectX::XMMATRIX inverseBindMatrix = DirectX::XMLoadFloat4x4(&joint.inverseBindMatrix);
            const DirectX::XMMATRIX jointWorld = nodeWorldTransforms[static_cast<size_t>(joint.nodeIndex)];
            // Left-to-right: IBM first, then joint world, then inverse mesh world - see this
            // function's own header comment for why the order is reversed from the glTF spec's
            // column-vector transcription.
            skinningMatrices[i] = inverseBindMatrix * jointWorld * inverseMeshWorld;
        }
        return skinningMatrices;
    }

    void UpdateSkinnedPose(
        const Asset::Model& model, const Asset::Skin& skin, const Pose& pose, const DirectX::XMMATRIX& rootTransform,
        const DirectX::XMMATRIX& meshWorldTransform, Renderer::SkinnedMeshDrawItem& item)
    {
        const std::vector<DirectX::XMMATRIX> nodeWorldTransforms = ComposePoseToWorld(model, pose, rootTransform);
        const std::vector<DirectX::XMMATRIX> skinningMatrices = ComputeSkinningMatrices(skin, nodeWorldTransforms, meshWorldTransform);

        DirectX::XMFLOAT4X4 bonePalette[Renderer::kMaxSkinJoints];
        for (DirectX::XMFLOAT4X4& boneMatrix : bonePalette)
        {
            DirectX::XMStoreFloat4x4(&boneMatrix, DirectX::XMMatrixIdentity());
        }
        for (size_t i = 0; i < skinningMatrices.size() && i < Renderer::kMaxSkinJoints; ++i)
        {
            // Transpose each joint's matrix individually for GPU row_major storage - see
            // ComputeSkinningMatrices's own comment for why this is safe here.
            DirectX::XMStoreFloat4x4(&bonePalette[i], DirectX::XMMatrixTranspose(skinningMatrices[i]));
        }
        Renderer::Buffer::Update(item.bonePaletteBuffer, bonePalette, sizeof(bonePalette));
    }
}
