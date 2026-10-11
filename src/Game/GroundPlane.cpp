#include "GroundPlane.h"

#include "GameConstants.h"
#include "Asset/Mesh.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Game
{
#if !defined(OMD_PROTOTYPE)
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
        // 64, not 8 - the original value produced a 96x24 texture (8px/tile) for the whole
        // stage, visibly blurry up close since there just wasn't much texture detail to sample.
        // Doesn't address shimmer/aliasing at a distance (this engine generates no mip chain for
        // any texture yet - see the TODO(OM) on Texture::Create), only the up-close softness.
        constexpr float kTileWorldSize = 1.0f;
        constexpr unsigned int kPixelsPerTile = 64;
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
#endif

#if defined(OMD_PROTOTYPE)
    namespace
    {
        // One repeat of the mud texture covers this many world units.
        constexpr float kMudTileWorldSize = 3.0f;
        // How far the floor continues past the stage on each side (it never visibly ends).
        constexpr float kFloorExtentX = 40.0f;
        constexpr float kWallHeight = 2.6f;

        float Hash01(int x, int y, int period, int seed)
        {
            x = ((x % period) + period) % period;
            y = ((y % period) + period) % period;
            uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            h ^= h >> 16;
            return static_cast<float>(h & 0xFFFFu) / 65535.0f;
        }

        // Value noise on a lattice of `period` cells that wraps in both axes, so the texture tiles.
        float PeriodicNoise(float u, float v, int period, int seed)
        {
            const float fx = u * static_cast<float>(period);
            const float fy = v * static_cast<float>(period);
            const int x0 = static_cast<int>(std::floor(fx));
            const int y0 = static_cast<int>(std::floor(fy));
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            const float sx = tx * tx * (3.0f - 2.0f * tx);
            const float sy = ty * ty * (3.0f - 2.0f * ty);
            const float a = Hash01(x0, y0, period, seed);
            const float b = Hash01(x0 + 1, y0, period, seed);
            const float c = Hash01(x0, y0 + 1, period, seed);
            const float d = Hash01(x0 + 1, y0 + 1, period, seed);
            const float top = a + (b - a) * sx;
            const float bottom = c + (d - c) * sx;
            return top + (bottom - top) * sy;
        }

        // TODO(OM): replace with authored floor art / a real stage.
        std::vector<uint8_t> BuildMudPixels(unsigned int size)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
            for (unsigned int y = 0; y < size; ++y)
            {
                for (unsigned int x = 0; x < size; ++x)
                {
                    const float u = static_cast<float>(x) / static_cast<float>(size);
                    const float v = static_cast<float>(y) / static_cast<float>(size);
                    // Three octaves: broad swells, mid blobs, fine grain.
                    const float n = PeriodicNoise(u, v, 4, 1) * 0.5f + PeriodicNoise(u, v, 8, 2) * 0.3f + PeriodicNoise(u, v, 16, 3) * 0.2f;
                    const float t = std::clamp((n - 0.25f) / 0.5f, 0.0f, 1.0f);
                    // Dark green -> mid green, with a pale crest on the highest ridges.
                    float r = 18.0f + t * 40.0f;
                    float g = 60.0f + t * 85.0f;
                    float b = 22.0f + t * 36.0f;
                    if (t > 0.78f)
                    {
                        const float crest = (t - 0.78f) / 0.22f;
                        r += crest * 55.0f;
                        g += crest * 40.0f;
                        b += crest * 35.0f;
                    }
                    const size_t offset = (static_cast<size_t>(y) * size + x) * 4;
                    pixels[offset + 0] = static_cast<uint8_t>(std::min(r, 255.0f));
                    pixels[offset + 1] = static_cast<uint8_t>(std::min(g, 255.0f));
                    pixels[offset + 2] = static_cast<uint8_t>(std::min(b, 255.0f));
                    pixels[offset + 3] = 255;
                }
            }
            return pixels;
        }

        // UVs come from world position, so neighbouring segments share one continuous texture
        // and scroll together.
        std::array<Asset::Vertex, 4> BuildFloorSegment(float x0, float x1, float scrollU, float scrollV)
        {
            const float y = kFloorSurfaceY;
            const auto vertex = [&](float x, float z)
            {
                return Asset::Vertex{ { x, y, z }, { 0.0f, 1.0f, 0.0f }, { x / kMudTileWorldSize + scrollU, z / kMudTileWorldSize + scrollV } };
            };
            return { vertex(x0, -kStageHalfDepth), vertex(x1, -kStageHalfDepth), vertex(x1, kStageHalfDepth), vertex(x0, kStageHalfDepth) };
        }

        struct FloorSegmentRange
        {
            float x0;
            float x1;
        };
        constexpr std::array<FloorSegmentRange, 3> kFloorSegments = { {
            { -kFloorExtentX, -kStageMovementHalfWidth },
            { -kStageMovementHalfWidth, kStageMovementHalfWidth },
            { kStageMovementHalfWidth, kFloorExtentX },
        } };

        Renderer::StaticMeshDrawItem CreateQuadItem(
            const Asset::Vertex* vertices, Renderer::TextureHandle texture, const DirectX::XMFLOAT4* tint, bool transparent)
        {
            const std::array<uint32_t, 6> indices = { 0, 1, 2, 0, 2, 3 };
            Renderer::StaticMeshDrawItem item;
            item.vertexBuffer = Renderer::Buffer::Create(vertices, 4 * sizeof(Asset::Vertex));
            item.vertexStride = sizeof(Asset::Vertex);
            item.indexBuffer = Renderer::Buffer::Create(indices.data(), indices.size() * sizeof(uint32_t));
            item.indexCount = static_cast<unsigned int>(indices.size());
            DirectX::XMFLOAT4X4 worldForGpu;
            DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
            item.worldBuffer = Renderer::Buffer::Create(&worldForGpu, sizeof(worldForGpu));
            item.baseColorTexture = texture;
            if (tint != nullptr)
            {
                item.tintBuffer = Renderer::Buffer::Create(tint, sizeof(*tint));
            }
            item.transparent = transparent;
            return item;
        }

        // Dashed boundary marker: bright at the floor, fading toward the top, dashed along its
        // length.
        std::vector<uint8_t> BuildWallPixels(unsigned int width, unsigned int height)
        {
            std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
            constexpr float kDashes = 8.0f;
            for (unsigned int y = 0; y < height; ++y)
            {
                const float v = static_cast<float>(y) / static_cast<float>(height - 1); // 0 = top, 1 = floor
                const float fade = std::pow(v, 1.15f);
                for (unsigned int x = 0; x < width; ++x)
                {
                    const float u = static_cast<float>(x) / static_cast<float>(width);
                    const float inDash = std::fmod(u * kDashes, 1.0f) < 0.62f ? 1.0f : 0.45f;
                    float alpha = fade * 0.62f * inDash;
                    if (v > 0.96f)
                    {
                        alpha = 0.9f; // bright line where the wall meets the floor
                    }
                    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
                    pixels[offset + 0] = 255;
                    pixels[offset + 1] = 70;
                    pixels[offset + 2] = 55;
                    pixels[offset + 3] = static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
                }
            }
            return pixels;
        }
    }

    StageDrawItems CreateStageDrawItems()
    {
        StageDrawItems stage;

        constexpr unsigned int kMudTextureSize = 256;
        const std::vector<uint8_t> mudPixels = BuildMudPixels(kMudTextureSize);
        const Renderer::TextureHandle mudTexture = Renderer::Texture::Create(mudPixels.data(), kMudTextureSize, kMudTextureSize);
        // The out-of-bounds floor is the same mud with its green suppressed and its red lifted.
        const DirectX::XMFLOAT4 outOfBoundsTint = { 2.2f, 0.08f, 0.06f, 1.0f };

        for (size_t i = 0; i < kFloorSegments.size(); ++i)
        {
            const bool outside = i != 1;
            const std::array<Asset::Vertex, 4> vertices = BuildFloorSegment(kFloorSegments[i].x0, kFloorSegments[i].x1, 0.0f, 0.0f);
            stage.floor.push_back(CreateQuadItem(vertices.data(), mudTexture, outside ? &outOfBoundsTint : nullptr, false));
        }

        constexpr unsigned int kWallTexW = 128;
        constexpr unsigned int kWallTexH = 64;
        const std::vector<uint8_t> wallPixels = BuildWallPixels(kWallTexW, kWallTexH);
        const Renderer::TextureHandle wallTexture = Renderer::Texture::Create(wallPixels.data(), kWallTexW, kWallTexH);
        for (const float x : { -kStageMovementHalfWidth, kStageMovementHalfWidth })
        {
            const float y0 = kFloorSurfaceY;
            const float y1 = kFloorSurfaceY + kWallHeight;
            // Normals point up so the flat lighting leaves the wall at full brightness.
            const std::array<Asset::Vertex, 4> vertices = {
                Asset::Vertex{ { x, y1, -kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
                Asset::Vertex{ { x, y1, kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
                Asset::Vertex{ { x, y0, kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
                Asset::Vertex{ { x, y0, -kStageHalfDepth }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } },
            };
            stage.walls.push_back(CreateQuadItem(vertices.data(), wallTexture, nullptr, true));
        }
        return stage;
    }

    void UpdateStageScroll(const StageDrawItems& stage, float timeSeconds)
    {
        // Two slightly different axis speeds so the pattern drifts diagonally instead of sliding.
        constexpr float kScrollSpeedU = 0.035f;
        constexpr float kScrollSpeedV = 0.02f;
        const float u = std::fmod(timeSeconds * kScrollSpeedU, 1.0f);
        const float v = std::fmod(timeSeconds * kScrollSpeedV, 1.0f);
        for (size_t i = 0; i < stage.floor.size() && i < kFloorSegments.size(); ++i)
        {
            const std::array<Asset::Vertex, 4> vertices = BuildFloorSegment(kFloorSegments[i].x0, kFloorSegments[i].x1, u, v);
            Renderer::Buffer::Update(stage.floor[i].vertexBuffer, vertices.data(), vertices.size() * sizeof(Asset::Vertex));
        }
    }
#endif
}
