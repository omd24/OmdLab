#include "Animation.h"

#include "Asset/Clip.h"
#include "Asset/Model.h"
#include "Asset/Skin.h"
#include "Renderer/Buffer.h"

#include <cmath>
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

    struct AnimationContext
    {
        const Asset::Clip* clip = nullptr;
        float timeSeconds = 0.0f;
        const std::unordered_map<int32_t, std::vector<const Asset::AnimationChannel*>>* channelsByNode = nullptr;
    };

    // Same "local = Scale*Rotate*Translate, world = local*parentWorld" composition
    // CreateStaticMeshDrawItems used to do inline - kept identical for bind pose (animContext
    // with a null clip), which is what this refactor was verified against. With a clip active,
    // each TRS component is taken from the clip's own channel targeting this node, if any -
    // any component the clip doesn't animate still falls back to the node's own rest pose.
    void EvaluateNode(
        const Asset::Model& model, int32_t nodeIndex, const DirectX::XMMATRIX& parentWorld, const AnimationContext& animContext,
        std::vector<DirectX::XMMATRIX>& outWorldTransforms)
    {
        const Asset::Node& node = model.nodes[nodeIndex];

        DirectX::XMVECTOR scale = DirectX::XMLoadFloat3(&node.scale);
        DirectX::XMVECTOR rotation = DirectX::XMLoadFloat4(&node.rotation);
        DirectX::XMVECTOR translation = DirectX::XMLoadFloat3(&node.translation);

        if (animContext.clip != nullptr)
        {
            const auto channelsForThisNode = animContext.channelsByNode->find(nodeIndex);
            if (channelsForThisNode != animContext.channelsByNode->end())
            {
                const float time = WrapTime(animContext.timeSeconds, animContext.clip->durationSeconds);
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

        const DirectX::XMMATRIX local =
            DirectX::XMMatrixScalingFromVector(scale) * DirectX::XMMatrixRotationQuaternion(rotation) *
            DirectX::XMMatrixTranslationFromVector(translation);
        const DirectX::XMMATRIX world = local * parentWorld;
        outWorldTransforms[static_cast<size_t>(nodeIndex)] = world;

        for (int32_t childIndex : node.childNodeIndices)
        {
            EvaluateNode(model, childIndex, world, animContext, outWorldTransforms);
        }
    }
}

namespace Engine
{
    std::vector<DirectX::XMMATRIX> EvaluateNodeWorldTransforms(
        const Asset::Model& model, const DirectX::XMMATRIX& rootTransform, const Asset::Clip* clip, float timeSeconds)
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

        const AnimationContext animContext{ clip, timeSeconds, &channelsByNode };

        std::vector<DirectX::XMMATRIX> worldTransforms(model.nodes.size(), DirectX::XMMatrixIdentity());
        for (int32_t rootIndex : model.rootNodeIndices)
        {
            EvaluateNode(model, rootIndex, rootTransform, animContext, worldTransforms);
        }
        return worldTransforms;
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
        const Asset::Model& model, const Asset::Skin& skin, const Asset::Clip& clip, float timeSeconds,
        const DirectX::XMMATRIX& rootTransform, const DirectX::XMMATRIX& meshWorldTransform, Renderer::SkinnedMeshDrawItem& item)
    {
        const std::vector<DirectX::XMMATRIX> nodeWorldTransforms = EvaluateNodeWorldTransforms(model, rootTransform, &clip, timeSeconds);
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
