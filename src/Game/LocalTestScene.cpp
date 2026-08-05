#include "LocalTestScene.h"

#include "Asset/GltfImporter.h"
#include "Foundation/Log.h"
#include "Renderer/Buffer.h"
#include "Renderer/StaticMeshDrawItem.h"
#include "Renderer/StaticMeshPass.h"
#include "Renderer/Texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <DirectXMath.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr const char* kScenePath = "local/test_scene_source/Models/Scene/glTF/Scene.gltf";

    Renderer::TextureHandle LoadTexture(const std::string& directory, const std::string& fileName)
    {
        const std::string fullPath = directory + "/" + fileName;
        int width = 0;
        int height = 0;
        int sourceChannels = 0;
        unsigned char* pixels = stbi_load(fullPath.c_str(), &width, &height, &sourceChannels, 4);
        if (pixels == nullptr)
        {
            Foundation::Log::Write(Foundation::Log::Severity::Warning, "Game", "Failed to decode texture '%s'", fullPath.c_str());
            return Renderer::TextureHandle{};
        }

        Renderer::TextureHandle handle = Renderer::Texture::Create(pixels, static_cast<unsigned int>(width), static_cast<unsigned int>(height));
        stbi_image_free(pixels);
        return handle;
    }
}

namespace LocalTestScene
{
    void LoadIfAvailable()
    {
        if (!std::filesystem::exists(kScenePath))
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Info, "Game",
                "Local test scene not found at '%s' - skipping (expected on a fresh clone; see local/ in .gitignore)", kScenePath);
            return;
        }

        Asset::Model model;
        if (!Asset::ImportGltf(kScenePath, model))
        {
            return;
        }

        const std::string directory = std::filesystem::path(kScenePath).parent_path().string();

        // One shared 1x1 white fallback for any material with no base color texture.
        const unsigned char whitePixel[4] = { 255, 255, 255, 255 };
        const Renderer::TextureHandle fallbackTexture = Renderer::Texture::Create(whitePixel, 1, 1);

        // Load only the textures actually referenced as some material's base color, each
        // once - this scene's other textures (normal/metallic-roughness maps) aren't sampled
        // by this "not full PBR" pass yet (see StaticMeshPass's own scoping note).
        std::unordered_map<int32_t, Renderer::TextureHandle> textureHandles;
        for (const Asset::Material& material : model.materials)
        {
            if (material.baseColorTexture == Asset::kInvalidIndex || textureHandles.count(material.baseColorTexture) != 0)
            {
                continue;
            }
            const Asset::Texture& texture = model.textures[material.baseColorTexture];
            textureHandles[material.baseColorTexture] = LoadTexture(directory, texture.filePath);
        }

        // Only root nodes are walked, not a full hierarchy - correct for this particular test
        // scene (one root node, no children) but not a general node-hierarchy traversal;
        // that's Engine's real connective resource layer's job once it exists, not this
        // temporary loader's.
        std::vector<Renderer::StaticMeshDrawItem> drawItems;
        for (int32_t rootIndex : model.rootNodeIndices)
        {
            const Asset::Node& node = model.nodes[rootIndex];
            if (node.meshIndex == Asset::kInvalidIndex)
            {
                continue;
            }

            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(
                &world, DirectX::XMMatrixTranspose(
                            DirectX::XMMatrixScaling(node.scale.x, node.scale.y, node.scale.z) *
                            DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&node.rotation)) *
                            DirectX::XMMatrixTranslation(node.translation.x, node.translation.y, node.translation.z)));

            const Asset::Mesh& mesh = model.meshes[node.meshIndex];
            for (const Asset::Primitive& primitive : mesh.primitives)
            {
                if (primitive.indices.empty())
                {
                    Foundation::Log::Write(
                        Foundation::Log::Severity::Warning, "Game", "Skipping non-indexed primitive in mesh '%s'", mesh.name.c_str());
                    continue;
                }

                Renderer::StaticMeshDrawItem item;
                item.vertexBuffer = Renderer::Buffer::Create(primitive.vertices.data(), primitive.vertices.size() * sizeof(Asset::Vertex));
                item.vertexStride = sizeof(Asset::Vertex);
                item.indexBuffer = Renderer::Buffer::Create(primitive.indices.data(), primitive.indices.size() * sizeof(uint32_t));
                item.indexCount = static_cast<unsigned int>(primitive.indices.size());
                item.world = world;

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

                drawItems.push_back(item);
            }
        }

        Renderer::StaticMeshPass::SetDrawItems(drawItems);
        Foundation::Log::Write(
            Foundation::Log::Severity::Info, "Game", "Local test scene loaded: %zu draw item(s), %zu unique texture(s)", drawItems.size(),
            textureHandles.size());
    }
}
