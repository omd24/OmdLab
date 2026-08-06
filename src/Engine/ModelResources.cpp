#include "ModelResources.h"

#include "Animation.h"
#include "Asset/Model.h"
#include "Foundation/Debug.h"
#include "Foundation/Log.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <DirectXMath.h>
#include <cstdint>
#include <unordered_map>

namespace
{
    Renderer::TextureHandle LoadTexture(const std::string& directory, const std::string& fileName)
    {
        const std::string fullPath = directory + "/" + fileName;
        int width = 0;
        int height = 0;
        int sourceChannels = 0;
        unsigned char* pixels = stbi_load(fullPath.c_str(), &width, &height, &sourceChannels, 4);
        if (pixels == nullptr)
        {
            Foundation::Log::Write(Foundation::Log::Severity::Warning, "Engine", "Failed to decode texture '%s'", fullPath.c_str());
            return Renderer::TextureHandle{};
        }

        Renderer::TextureHandle handle = Renderer::Texture::Create(pixels, static_cast<unsigned int>(width), static_cast<unsigned int>(height));
        stbi_image_free(pixels);
        return handle;
    }

    // Shared fallback for any material with no base color texture (including a source asset
    // with zero textures at all, e.g. one authored with flat vertex/factor color).
    Renderer::TextureHandle CreateFallbackTexture()
    {
        const unsigned char whitePixel[4] = { 255, 255, 255, 255 };
        return Renderer::Texture::Create(whitePixel, 1, 1);
    }

    std::unordered_map<int32_t, Renderer::TextureHandle> BuildBaseColorTextureHandles(
        const Asset::Model& model, const std::string& textureDirectory)
    {
        std::unordered_map<int32_t, Renderer::TextureHandle> textureHandles;
        for (const Asset::Material& material : model.materials)
        {
            if (material.baseColorTexture == Asset::kInvalidIndex || textureHandles.count(material.baseColorTexture) != 0)
            {
                continue;
            }
            const Asset::Texture& texture = model.textures[material.baseColorTexture];
            textureHandles[material.baseColorTexture] = LoadTexture(textureDirectory, texture.filePath);
        }
        return textureHandles;
    }

    Renderer::TextureHandle ResolveBaseColorTexture(
        const Asset::Model& model, const Asset::Primitive& primitive,
        const std::unordered_map<int32_t, Renderer::TextureHandle>& textureHandles, Renderer::TextureHandle fallbackTexture)
    {
        if (primitive.materialIndex != Asset::kInvalidIndex)
        {
            const Asset::Material& material = model.materials[primitive.materialIndex];
            const auto it = textureHandles.find(material.baseColorTexture);
            if (it != textureHandles.end())
            {
                return it->second;
            }
        }
        return fallbackTexture;
    }

    // Must match SkinnedMeshPassDX12's own SkinnedMeshVertex layout - see StaticMeshDrawItem's
    // vertexStride contract (Renderer trusts the caller-declared stride, never sees this type).
    // No equivalent already-imported Asset struct to piggyback on this time (unlike
    // CreateStaticMeshDrawItems' use of Asset::Vertex directly) - joint indices/weights live in
    // a separate parallel Asset::SkinningData array, so this interleaves the two explicitly.
    struct SkinnedVertexForGpu
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv0;
        DirectX::XMFLOAT4 jointIndices;
        DirectX::XMFLOAT4 jointWeights;
    };
}

namespace Engine
{
    std::vector<Renderer::StaticMeshDrawItem> CreateStaticMeshDrawItems(
        const Asset::Model& model, const std::string& textureDirectory, const DirectX::XMMATRIX& rootTransform)
    {
        const Renderer::TextureHandle fallbackTexture = CreateFallbackTexture();
        const std::unordered_map<int32_t, Renderer::TextureHandle> textureHandles = BuildBaseColorTextureHandles(model, textureDirectory);

        // Bind pose: every node's own authored rest-pose local transform, no animation - the
        // same walk the animation runtime (Engine/Animation.h) reuses with animated local
        // transforms instead. Resolving every node's world transform up front and then
        // scanning linearly below (rather than building draw items during the walk itself, as
        // this used to do) means this function no longer needs its own hierarchy-walking code
        // at all - one shared implementation, not two that could drift apart.
        const std::vector<DirectX::XMMATRIX> nodeWorldTransforms = EvaluateNodeWorldTransforms(model, rootTransform);

        std::vector<Renderer::StaticMeshDrawItem> drawItems;
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        {
            const Asset::Node& node = model.nodes[nodeIndex];
            // A skinned mesh node (skinIndex set) is CreateSkinnedMeshDrawItems' job instead -
            // its vertices are expressed relative to a skin's joints, not directly drawable
            // with just this node's own rigid world transform the moment any clip plays.
            if (node.meshIndex == Asset::kInvalidIndex || node.skinIndex != Asset::kInvalidIndex)
            {
                continue;
            }

            DirectX::XMFLOAT4X4 worldForGpu;
            DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(nodeWorldTransforms[nodeIndex]));

            const Asset::Mesh& mesh = model.meshes[node.meshIndex];
            for (const Asset::Primitive& primitive : mesh.primitives)
            {
                if (primitive.indices.empty())
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Warning, "Engine", "Skipping non-indexed primitive in mesh '%s'", mesh.name.c_str());
                    continue;
                }

                Renderer::StaticMeshDrawItem item;
                item.vertexBuffer = Renderer::Buffer::Create(primitive.vertices.data(), primitive.vertices.size() * sizeof(Asset::Vertex));
                item.vertexStride = sizeof(Asset::Vertex);
                item.indexBuffer = Renderer::Buffer::Create(primitive.indices.data(), primitive.indices.size() * sizeof(uint32_t));
                item.indexCount = static_cast<unsigned int>(primitive.indices.size());
                // Own buffer, created once and never touched again - see StaticMeshDrawItem's
                // own comment for why a single shared/reused world buffer is unsafe.
                item.worldBuffer = Renderer::Buffer::Create(&worldForGpu, sizeof(worldForGpu));
                item.baseColorTexture = ResolveBaseColorTexture(model, primitive, textureHandles, fallbackTexture);

                drawItems.push_back(item);
            }
        }

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Engine", "Built %zu draw item(s), %zu unique texture(s) from model", drawItems.size(),
            textureHandles.size());
        return drawItems;
    }

    std::vector<Renderer::SkinnedMeshDrawItem> CreateSkinnedMeshDrawItems(
        const Asset::Model& model, const std::string& textureDirectory, const DirectX::XMMATRIX& rootTransform)
    {
        const Renderer::TextureHandle fallbackTexture = CreateFallbackTexture();
        const std::unordered_map<int32_t, Renderer::TextureHandle> textureHandles = BuildBaseColorTextureHandles(model, textureDirectory);

        const std::vector<DirectX::XMMATRIX> nodeWorldTransforms = EvaluateNodeWorldTransforms(model, rootTransform);

        DirectX::XMFLOAT4X4 identityBonePalette[Renderer::kMaxSkinJoints];
        for (DirectX::XMFLOAT4X4& boneMatrix : identityBonePalette)
        {
            DirectX::XMStoreFloat4x4(&boneMatrix, DirectX::XMMatrixIdentity());
        }

        std::vector<Renderer::SkinnedMeshDrawItem> drawItems;
        for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
        {
            const Asset::Node& node = model.nodes[nodeIndex];
            if (node.meshIndex == Asset::kInvalidIndex || node.skinIndex == Asset::kInvalidIndex)
            {
                continue;
            }

            DirectX::XMFLOAT4X4 worldForGpu;
            DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(nodeWorldTransforms[nodeIndex]));

            const Asset::Skin& skin = model.skins[node.skinIndex];
            OMD_ASSERT(
                skin.joints.size() <= Renderer::kMaxSkinJoints, "Skin '%s' has %zu joints, exceeds kMaxSkinJoints (%u)", skin.name.c_str(),
                skin.joints.size(), Renderer::kMaxSkinJoints);

            const Asset::Mesh& mesh = model.meshes[node.meshIndex];
            for (const Asset::Primitive& primitive : mesh.primitives)
            {
                if (primitive.indices.empty() || primitive.skinning.empty())
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Warning, "Engine", "Skipping non-indexed or non-skinned primitive in mesh '%s'",
                        mesh.name.c_str());
                    continue;
                }

                std::vector<SkinnedVertexForGpu> gpuVertices(primitive.vertices.size());
                for (size_t v = 0; v < primitive.vertices.size(); ++v)
                {
                    const Asset::Vertex& sourceVertex = primitive.vertices[v];
                    const Asset::SkinningData& skinning = primitive.skinning[v];
                    SkinnedVertexForGpu& gpuVertex = gpuVertices[v];
                    gpuVertex.position = sourceVertex.position;
                    gpuVertex.normal = sourceVertex.normal;
                    gpuVertex.uv0 = sourceVertex.uv0;
                    gpuVertex.jointIndices = DirectX::XMFLOAT4(
                        static_cast<float>(skinning.jointIndices[0]), static_cast<float>(skinning.jointIndices[1]),
                        static_cast<float>(skinning.jointIndices[2]), static_cast<float>(skinning.jointIndices[3]));
                    gpuVertex.jointWeights = DirectX::XMFLOAT4(
                        skinning.jointWeights[0], skinning.jointWeights[1], skinning.jointWeights[2], skinning.jointWeights[3]);
                }

                Renderer::SkinnedMeshDrawItem item;
                item.vertexBuffer = Renderer::Buffer::Create(gpuVertices.data(), gpuVertices.size() * sizeof(SkinnedVertexForGpu));
                item.vertexStride = sizeof(SkinnedVertexForGpu);
                item.indexBuffer = Renderer::Buffer::Create(primitive.indices.data(), primitive.indices.size() * sizeof(uint32_t));
                item.indexCount = static_cast<unsigned int>(primitive.indices.size());
                item.worldBuffer = Renderer::Buffer::Create(&worldForGpu, sizeof(worldForGpu));
                // Identity - see this function's own header comment for why. Engine::
                // UpdateSkinnedPose overwrites this in place once real per-frame posing exists.
                item.bonePaletteBuffer = Renderer::Buffer::Create(identityBonePalette, sizeof(identityBonePalette));
                item.baseColorTexture = ResolveBaseColorTexture(model, primitive, textureHandles, fallbackTexture);

                drawItems.push_back(item);
            }
        }

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Engine", "Built %zu skinned draw item(s), %zu unique texture(s) from model", drawItems.size(),
            textureHandles.size());
        return drawItems;
    }
}
