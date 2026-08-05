#include "ModelResources.h"

#include "Asset/Model.h"
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

    // Recurses the full node subtree rooted at nodeIndex, appending one StaticMeshDrawItem per
    // indexed primitive found along the way. parentWorld carries the accumulated ancestor
    // transform in DirectXMath's natural row-vector form (v' = v * M) - only the final
    // per-draw-item matrix gets transposed for GPU row_major storage, never an intermediate one,
    // since transposing mid-chain would break the composition.
    void CollectDrawItems(
        const Asset::Model& model,
        int32_t nodeIndex,
        const DirectX::XMMATRIX& parentWorld,
        const std::unordered_map<int32_t, Renderer::TextureHandle>& textureHandles,
        Renderer::TextureHandle fallbackTexture,
        std::vector<Renderer::StaticMeshDrawItem>& outItems)
    {
        const Asset::Node& node = model.nodes[nodeIndex];
        const DirectX::XMMATRIX local = DirectX::XMMatrixScaling(node.scale.x, node.scale.y, node.scale.z) *
                                         DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&node.rotation)) *
                                         DirectX::XMMatrixTranslation(node.translation.x, node.translation.y, node.translation.z);
        const DirectX::XMMATRIX world = local * parentWorld;

        if (node.meshIndex != Asset::kInvalidIndex)
        {
            DirectX::XMFLOAT4X4 worldForGpu;
            DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(world));

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

                Renderer::TextureHandle textureHandle = fallbackTexture;
                if (primitive.materialIndex != Asset::kInvalidIndex)
                {
                    const Asset::Material& material = model.materials[primitive.materialIndex];
                    auto it = textureHandles.find(material.baseColorTexture);
                    if (it != textureHandles.end())
                    {
                        textureHandle = it->second;
                    }
                }
                item.baseColorTexture = textureHandle;

                outItems.push_back(item);
            }
        }

        for (int32_t childIndex : node.childNodeIndices)
        {
            CollectDrawItems(model, childIndex, world, textureHandles, fallbackTexture, outItems);
        }
    }
}

namespace Engine
{
    std::vector<Renderer::StaticMeshDrawItem> CreateStaticMeshDrawItems(
        const Asset::Model& model, const std::string& textureDirectory, const DirectX::XMMATRIX& rootTransform)
    {
        // Shared fallback for any material with no base color texture (including a source
        // asset with zero textures at all, e.g. one authored with flat vertex/factor color).
        const unsigned char whitePixel[4] = { 255, 255, 255, 255 };
        const Renderer::TextureHandle fallbackTexture = Renderer::Texture::Create(whitePixel, 1, 1);

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

        std::vector<Renderer::StaticMeshDrawItem> drawItems;
        for (int32_t rootIndex : model.rootNodeIndices)
        {
            CollectDrawItems(model, rootIndex, rootTransform, textureHandles, fallbackTexture, drawItems);
        }

        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Engine", "Built %zu draw item(s), %zu unique texture(s) from model", drawItems.size(),
            textureHandles.size());
        return drawItems;
    }
}
