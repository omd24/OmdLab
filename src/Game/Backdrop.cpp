#if defined(OMD_PROTOTYPE)

#include "Backdrop.h"

#include "GameConstants.h"
#include "GroundPlane.h"
#include "Vfx.h"
#include "Asset/Mesh.h"
#include "Engine/ModelResources.h"
#include "Foundation/Log.h"
#include "Renderer/Buffer.h"

#include <algorithm>
#include <cmath>

namespace Game
{
    namespace
    {
        // The picture is 1915 x 821; the wall stands this tall so the fighters read at a
        // sensible size against it and its top edge sits above the default view.
        constexpr float kHeightUnits = 4.8f;
        constexpr float kAspect = 1915.0f / 821.0f;
        constexpr int kHalfTiles = 2; // Tiles on each side of the centre one.

        // The two wall torches, in picture coordinates (u across, v down) of the flame's base.
        constexpr float kTorchU[2] = { 0.375f, 0.643f };
        constexpr float kTorchV = 0.25f;

        float Hash01(float x)
        {
            const float s = std::sin(x * 12.9898f) * 43758.5453f;
            return s - std::floor(s);
        }

        // Smooth flicker in 0..1.
        float Flicker(float t, float seed)
        {
            return 0.5f + 0.5f * (std::sin(t * 13.0f + seed) * 0.5f + std::sin(t * 31.0f + seed * 2.3f) * 0.3f +
                                  std::sin(t * 7.0f + seed * 0.7f) * 0.2f);
        }
    }

    Backdrop CreateBackdrop(const std::string& imagePath)
    {
        Backdrop backdrop;
        const Renderer::TextureHandle texture = Engine::LoadTextureFile(imagePath);
        if (texture.index < 0)
        {
            Foundation::Log::Write(Foundation::Log::Severity::Warning, "Game", "Backdrop picture '%s' not loaded", imagePath.c_str());
            return backdrop;
        }

        backdrop.tileHeight = kHeightUnits;
        backdrop.tileWidth = kHeightUnits * kAspect;
        backdrop.baseY = kFloorSurfaceY;
        backdrop.z = kStageHalfDepth;
        backdrop.tileCount = kHalfTiles * 2 + 1;
        backdrop.torchAccumulator.assign(static_cast<size_t>(backdrop.tileCount) * 2, 0.0f);

        for (int tile = -kHalfTiles; tile <= kHalfTiles; ++tile)
        {
            const bool mirrored = (tile % 2) != 0;
            const float x0 = (static_cast<float>(tile) - 0.5f) * backdrop.tileWidth;
            const float x1 = x0 + backdrop.tileWidth;
            const float y0 = backdrop.baseY;
            const float y1 = backdrop.baseY + backdrop.tileHeight;
            const float uLeft = mirrored ? 1.0f : 0.0f;
            const float uRight = mirrored ? 0.0f : 1.0f;
            // Normals point up so the flat lighting leaves the picture at full brightness.
            const Asset::Vertex vertices[4] = {
                Asset::Vertex{ { x0, y1, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { uLeft, 0.0f } },
                Asset::Vertex{ { x1, y1, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { uRight, 0.0f } },
                Asset::Vertex{ { x1, y0, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { uRight, 1.0f } },
                Asset::Vertex{ { x0, y0, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { uLeft, 1.0f } },
            };
            const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
            Renderer::StaticMeshDrawItem item;
            item.vertexBuffer = Renderer::Buffer::Create(vertices, sizeof(vertices));
            item.vertexStride = sizeof(Asset::Vertex);
            item.indexBuffer = Renderer::Buffer::Create(indices, sizeof(indices));
            item.indexCount = 6;
            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
            item.worldBuffer = Renderer::Buffer::Create(&world, sizeof(world));
            item.baseColorTexture = texture;
            backdrop.items.push_back(item);
        }
        backdrop.loaded = true;
        return backdrop;
    }

    void SetBackdropVisible(Backdrop& backdrop, bool visible)
    {
        if (!backdrop.loaded || backdrop.visible == visible)
        {
            return;
        }
        backdrop.visible = visible;
        DirectX::XMFLOAT4X4 world;
        const DirectX::XMMATRIX matrix = visible ? DirectX::XMMatrixIdentity() : DirectX::XMMatrixScaling(0.0f, 0.0f, 0.0f);
        DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(matrix));
        for (const Renderer::StaticMeshDrawItem& item : backdrop.items)
        {
            Renderer::Buffer::Update(item.worldBuffer, &world, sizeof(world));
        }
    }

    void UpdateBackdropFire(Backdrop& backdrop, VfxSystem& vfx, float cameraX, float deltaSeconds)
    {
        if (!backdrop.loaded || !backdrop.visible)
        {
            return;
        }
        backdrop.clock += deltaSeconds;
        const float t = backdrop.clock;
        // Rates in particles per second.
        constexpr float kFlameRate = 70.0f;
        constexpr float kSmokeRate = 14.0f;
        constexpr float kEmberRate = 5.0f;

        for (int tile = -kHalfTiles; tile <= kHalfTiles; ++tile)
        {
            const bool mirrored = (tile % 2) != 0;
            for (int torch = 0; torch < 2; ++torch)
            {
                const float u = mirrored ? 1.0f - kTorchU[torch] : kTorchU[torch];
                const float x = static_cast<float>(tile) * backdrop.tileWidth + (u - 0.5f) * backdrop.tileWidth;
                if (std::fabs(x - cameraX) > 9.5f)
                {
                    continue;
                }
                const float y = backdrop.baseY + (1.0f - kTorchV) * backdrop.tileHeight;
                const DirectX::XMFLOAT3 base = { x, y, backdrop.z - 0.12f };
                const size_t index = static_cast<size_t>(tile + kHalfTiles) * 2 + static_cast<size_t>(torch);
                const float seed = static_cast<float>(index) * 3.7f;
                float& owed = backdrop.torchAccumulator[index];
                const float flicker = Flicker(t, seed);

                // Soft pulsing glow around the flame, a fresh one each frame.
                Engine::ParticleBurst glow;
                glow.count = 1;
                glow.spread = 0.0f;
                glow.speedMin = glow.speedMax = 0.0f;
                glow.lifetimeMin = glow.lifetimeMax = (std::max)(deltaSeconds * 1.5f, 0.02f);
                glow.size0 = glow.size1 = 1.5f + 0.35f * flicker;
                glow.color0 = { 1.8f, 0.45f, 0.06f, 0.10f + 0.08f * flicker };
                glow.color1 = glow.color0;
                vfx.particles.Burst(glow, { base.x, base.y + 0.18f, base.z });

                owed += deltaSeconds * kFlameRate;
                const int flames = static_cast<int>(owed);
                owed -= static_cast<float>(flames);
                if (flames > 0)
                {
                    Engine::ParticleBurst flame;
                    flame.count = flames;
                    flame.direction = { 0.0f, 1.0f, 0.0f };
                    flame.spread = 0.12f;
                    flame.speedMin = 0.8f;
                    flame.speedMax = 1.5f;
                    flame.lifetimeMin = 0.22f;
                    flame.lifetimeMax = 0.5f;
                    flame.size0 = 0.26f;
                    flame.size1 = 0.04f;
                    flame.drag = 0.8f;
                    flame.gravity = -0.5f; // Rises.
                    flame.color0 = { 1.7f, 0.55f, 0.07f, 0.8f };
                    flame.color1 = { 1.0f, 0.08f, 0.01f, 0.0f };
                    vfx.particles.EmitAlong(flame, { base.x - 0.05f, base.y, base.z }, { base.x + 0.05f, base.y + 0.03f, base.z });
                }

                // Smoke and the odd ember, by chance per frame so rates stay frame-rate independent.
                if (Hash01(t * 91.0f + seed) < kSmokeRate * deltaSeconds)
                {
                    Engine::ParticleBurst smoke;
                    smoke.count = 1;
                    smoke.direction = { 0.0f, 1.0f, 0.0f };
                    smoke.spread = 0.15f;
                    smoke.speedMin = 0.25f;
                    smoke.speedMax = 0.5f;
                    smoke.lifetimeMin = 0.9f;
                    smoke.lifetimeMax = 1.5f;
                    smoke.size0 = 0.15f;
                    smoke.size1 = 0.55f;
                    smoke.drag = 0.4f;
                    smoke.color0 = { 0.4f, 0.37f, 0.35f, 0.22f };
                    smoke.color1 = { 0.3f, 0.3f, 0.3f, 0.0f };
                    vfx.particles.Burst(smoke, { base.x, base.y + 0.25f, base.z });
                }
                if (Hash01(t * 57.0f + seed * 1.9f) < kEmberRate * deltaSeconds)
                {
                    Engine::ParticleBurst ember;
                    ember.count = 1;
                    ember.direction = { 0.0f, 1.0f, 0.0f };
                    ember.spread = 0.5f;
                    ember.speedMin = 0.5f;
                    ember.speedMax = 1.3f;
                    ember.lifetimeMin = 0.6f;
                    ember.lifetimeMax = 1.2f;
                    ember.size0 = 0.045f;
                    ember.size1 = 0.01f;
                    ember.gravity = -0.3f;
                    ember.color0 = { 3.0f, 1.6f, 0.4f, 1.0f };
                    ember.color1 = { 1.6f, 0.3f, 0.05f, 0.0f };
                    vfx.particles.Burst(ember, { base.x, base.y + 0.1f, base.z });
                }
            }
        }
    }
}

#endif // OMD_PROTOTYPE
