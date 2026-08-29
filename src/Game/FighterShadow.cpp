#include "FighterShadow.h"

#include "GameConstants.h"
#include "Asset/Mesh.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Game
{
    Renderer::StaticMeshDrawItem CreateFighterShadowDrawItem()
    {
        // A small flat disc, local Y=0, facing +Y - same shape as CreateGroundPlaneDrawItem's
        // own quad, just fighter-sized and round instead of stage-sized and rectangular.
        constexpr float kRadius = 0.3f;
        constexpr int kSegments = 16;
        std::vector<Asset::Vertex> vertices;
        vertices.reserve(static_cast<size_t>(kSegments) + 1);
        vertices.push_back(Asset::Vertex{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } });
        for (int i = 0; i < kSegments; ++i)
        {
            const float angle = DirectX::XM_2PI * static_cast<float>(i) / static_cast<float>(kSegments);
            const float x = kRadius * cosf(angle);
            const float z = kRadius * sinf(angle);
            vertices.push_back(Asset::Vertex{ { x, 0.0f, z }, { 0.0f, 1.0f, 0.0f }, { 0.5f + 0.5f * cosf(angle), 0.5f + 0.5f * sinf(angle) } });
        }
        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(kSegments) * 3);
        for (int i = 0; i < kSegments; ++i)
        {
            indices.push_back(0);
            indices.push_back(static_cast<uint32_t>(1 + i));
            indices.push_back(static_cast<uint32_t>(1 + (i + 1) % kSegments));
        }

        // Radial-gradient alpha - opaque-ish through most of the disc, softening only near the
        // rim (smoothstep falloff starting at 60% of the radius) - a soft edge without any
        // shader/pipeline work beyond enabling blending, since LitTextured.hlsl's PSMain
        // already passes a bound texture's own alpha straight through. RGB stays a constant
        // dark gray. Alpha capped at 200 (not 255) so even the ground-level shadow reads as a
        // soft patch, not a hard-opaque disc. Same "generate on the CPU, upload once" pattern
        // the ground plane's own checkerboard texture already uses.
        constexpr unsigned int kTexSize = 32;
        std::vector<uint8_t> pixels(static_cast<size_t>(kTexSize) * kTexSize * 4);
        const float center = (static_cast<float>(kTexSize) - 1.0f) * 0.5f;
        for (unsigned int y = 0; y < kTexSize; ++y)
        {
            for (unsigned int x = 0; x < kTexSize; ++x)
            {
                const float dx = (static_cast<float>(x) - center) / center;
                const float dy = (static_cast<float>(y) - center) / center;
                const float dist = sqrtf(dx * dx + dy * dy); // 0 at center, 1 at rim.
                const float alpha01 = 1.0f - std::clamp((dist - 0.6f) / 0.4f, 0.0f, 1.0f);
                const float smoothAlpha = alpha01 * alpha01 * (3.0f - 2.0f * alpha01); // smoothstep
                const size_t offset = (static_cast<size_t>(y) * kTexSize + x) * 4;
                pixels[offset + 0] = 30;
                pixels[offset + 1] = 30;
                pixels[offset + 2] = 30;
                pixels[offset + 3] = static_cast<uint8_t>(smoothAlpha * 200.0f);
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
        item.baseColorTexture = Renderer::Texture::Create(pixels.data(), kTexSize, kTexSize);
        // A real per-instance tint buffer (not the pass's shared default) since this item's
        // alpha genuinely changes every frame with jump height - see
        // UpdateFighterShadowPosition.
        constexpr DirectX::XMFLOAT4 kInitialTint = { 1.0f, 1.0f, 1.0f, 1.0f };
        item.tintBuffer = Renderer::Buffer::Create(&kInitialTint, sizeof(kInitialTint));
        item.transparent = true;

        return item;
    }

    void UpdateFighterShadowPosition(Renderer::StaticMeshDrawItem& shadowItem, float worldX, float worldY, float worldZ)
    {
        constexpr float kGroundEpsilon = 0.01f;
        const DirectX::XMMATRIX worldTransform = DirectX::XMMatrixTranslation(worldX, kGroundEpsilon, worldZ);
        DirectX::XMFLOAT4X4 worldForGpu;
        DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(worldTransform));
        Renderer::Buffer::Update(shadowItem.worldBuffer, &worldForGpu, sizeof(worldForGpu));

        // Fades further as the fighter jumps higher (worldY, not the shadow's own fixed
        // ground-epsilon height), fading back in as they land - never fully to zero, so a
        // faint grounding cue stays visible even at peak height.
        const float heightFraction = std::clamp(worldY / Game::kShadowFadeMaxHeight, 0.0f, 1.0f);
        const float alphaMultiplier = 1.0f - heightFraction * (1.0f - Game::kShadowMinAlpha);
        const DirectX::XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, alphaMultiplier };
        Renderer::Buffer::Update(shadowItem.tintBuffer, &tint, sizeof(tint));
    }
}
