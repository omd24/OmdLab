#include "GroundPlane.h"

#include "GameConstants.h"
#include "Asset/Mesh.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace Game
{
    Renderer::StaticMeshDrawItem CreateGroundPlaneDrawItem()
    {
        // A single quad at Y=0 (the height every fighter is already fixed at), facing +Y - four
        // vertices, two triangles, no reason for more on a perfectly flat stage. StaticMeshPass
        // renders double-sided (see its own cullBackFaces = false), so winding order here only
        // needs to be consistent, not front-face-correct.
        const std::array<Asset::Vertex, 4> vertices = {
            Asset::Vertex{ { -kStageHalfWidth, 0.0f, -kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            Asset::Vertex{ { kStageHalfWidth, 0.0f, -kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
            Asset::Vertex{ { kStageHalfWidth, 0.0f, kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            Asset::Vertex{ { -kStageHalfWidth, 0.0f, kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
        };
        const std::array<uint32_t, 6> indices = { 0, 1, 2, 0, 2, 3 };

        // Checkerboard, generated directly rather than authored as a file - same "no art needed"
        // reasoning as the quad itself. One tile per world unit (kTileWorldSize) so the pattern
        // reads as a real scale reference instead of an arbitrary stretch; UVs span [0, 1] once
        // across the whole plane (baked into the texture, not tiled via sampler wrap mode, so
        // this doesn't depend on whatever addressing mode the shared sampler happens to use).
        constexpr float kTileWorldSize = 1.0f;
        constexpr unsigned int kPixelsPerTile = 8;
        const unsigned int tilesX = std::max(1u, static_cast<unsigned int>((kStageHalfWidth * 2.0f) / kTileWorldSize));
        const unsigned int tilesZ = std::max(1u, static_cast<unsigned int>((kStageHalfDepth * 2.0f) / kTileWorldSize));
        const unsigned int texWidth = tilesX * kPixelsPerTile;
        const unsigned int texHeight = tilesZ * kPixelsPerTile;

        std::vector<uint8_t> pixels(static_cast<size_t>(texWidth) * texHeight * 4);
        for (unsigned int y = 0; y < texHeight; ++y)
        {
            for (unsigned int x = 0; x < texWidth; ++x)
            {
                const unsigned int tileX = x / kPixelsPerTile;
                const unsigned int tileY = y / kPixelsPerTile;
                const uint8_t value = ((tileX + tileY) % 2 == 0) ? 200 : 60;
                const size_t offset = (static_cast<size_t>(y) * texWidth + x) * 4;
                pixels[offset + 0] = value;
                pixels[offset + 1] = value;
                pixels[offset + 2] = value;
                pixels[offset + 3] = 255;
            }
        }

        Renderer::StaticMeshDrawItem item;
        item.vertexBuffer = Renderer::Buffer::Create(vertices.data(), vertices.size() * sizeof(Asset::Vertex));
        item.vertexStride = sizeof(Asset::Vertex);
        item.indexBuffer = Renderer::Buffer::Create(indices.data(), indices.size() * sizeof(uint32_t));
        item.indexCount = static_cast<unsigned int>(indices.size());
        DirectX::XMFLOAT4X4 worldForGpu;
        DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
        item.worldBuffer = Renderer::Buffer::Create(&worldForGpu, sizeof(worldForGpu));
        item.baseColorTexture = Renderer::Texture::Create(pixels.data(), texWidth, texHeight);

        return item;
    }
}
