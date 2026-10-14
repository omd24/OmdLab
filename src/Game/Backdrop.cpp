#if defined(OMD_PROTOTYPE)

#include "Backdrop.h"

#include "GameConstants.h"
#include "GroundPlane.h"
#include "Vfx.h"
#include "Asset/Mesh.h"
#include "Engine/ModelResources.h"
#include "Foundation/Log.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#include <algorithm>
#include <cmath>

namespace Game
{
    namespace
    {
        // The picture is 1916 x 821; the wall stands this tall so the fighters read at a
        // sensible size against it and its top edge sits above the default view.
        constexpr float kHeightUnits = 4.8f;
        constexpr float kAspect = 1916.0f / 821.0f;
        constexpr int kHalfTiles = 2; // Tiles on each side of the centre one.

        // The dark picture: ambient level and a warm pool of light around each torch.
        constexpr float kMenuAmbient = 0.03f;
        constexpr float kMenuTorchGain = 1.25f;
        constexpr float kMenuTorchRadiusPixels = 300.0f;

        // The two wall torches, in picture coordinates (u across, v down) of the flame's base.
        constexpr float kTorchU[2] = { 0.404f, 0.603f };
        constexpr float kTorchV = 0.26f;

        // The picture lit only by its two torches, as a texture.
        Renderer::TextureHandle CreateDarkTexture(const std::string& imagePath, float torchU0, float torchU1, float torchV)
        {
            unsigned int width = 0;
            unsigned int height = 0;
            std::vector<unsigned char> pixels;
            if (!Engine::LoadImageRgba(imagePath, width, height, pixels))
            {
                return Renderer::TextureHandle{};
            }
            const float torchU[2] = { torchU0, torchU1 };
            for (unsigned int y = 0; y < height; ++y)
            {
                for (unsigned int x = 0; x < width; ++x)
                {
                    const float u = static_cast<float>(x) / static_cast<float>(width);
                    const float v = static_cast<float>(y) / static_cast<float>(height);
                    float light = 0.0f;
                    for (const float tu : torchU)
                    {
                        const float dx = (u - tu) * static_cast<float>(width);
                        const float dy = (v - torchV) * static_cast<float>(height);
                        const float d2 = (dx * dx + dy * dy) / (kMenuTorchRadiusPixels * kMenuTorchRadiusPixels);
                        light += 1.0f / std::pow(1.0f + d2, 1.5f);
                    }
                    light *= kMenuTorchGain;
                    // Cool, very dim ambient plus warm torch light.
                    const float r = kMenuAmbient * 0.8f + light * 1.00f;
                    const float g = kMenuAmbient * 0.9f + light * 0.58f;
                    const float b = kMenuAmbient * 1.2f + light * 0.26f;
                    unsigned char* p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                    p[0] = static_cast<unsigned char>(std::clamp(p[0] * r, 0.0f, 255.0f));
                    p[1] = static_cast<unsigned char>(std::clamp(p[1] * g, 0.0f, 255.0f));
                    p[2] = static_cast<unsigned char>(std::clamp(p[2] * b, 0.0f, 255.0f));
                }
            }
            return Renderer::Texture::Create(pixels.data(), width, height);
        }

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
        const Renderer::TextureHandle darkTexture = CreateDarkTexture(imagePath, kTorchU[0], kTorchU[1], kTorchV);
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
            if (darkTexture.index >= 0)
            {
                // Its own world buffer (collapsed to nothing until the menu is shown).
                Renderer::StaticMeshDrawItem dark = item;
                const DirectX::XMFLOAT4X4 hidden = [&]
                {
                    DirectX::XMFLOAT4X4 m;
                    DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(0.0f, 0.0f, 0.0f)));
                    return m;
                }();
                dark.worldBuffer = Renderer::Buffer::Create(&hidden, sizeof(hidden));
                dark.baseColorTexture = darkTexture;
                backdrop.menuItems.push_back(dark);
            }
        }
        // A plain band above the picture, in the colour of its top edge, so a camera that looks
        // a little higher (shake, a free-fly nudge) never shows the clear colour above the wall.
        {
            unsigned int width = 0;
            unsigned int height = 0;
            std::vector<unsigned char> pixels;
            if (Engine::LoadImageRgba(imagePath, width, height, pixels) && width > 0 && height > 0)
            {
                const unsigned int rows = (std::min)(6u, height);
                double sum[3] = { 0.0, 0.0, 0.0 };
                for (unsigned int y = 0; y < rows; ++y)
                {
                    for (unsigned int x = 0; x < width; ++x)
                    {
                        const unsigned char* p = &pixels[(static_cast<size_t>(y) * width + x) * 4];
                        sum[0] += p[0];
                        sum[1] += p[1];
                        sum[2] += p[2];
                    }
                }
                const double count = static_cast<double>(rows) * static_cast<double>(width) * 255.0;
                backdrop.capColor = { static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count),
                                      static_cast<float>(sum[2] / count) };
            }
            const float half = (static_cast<float>(kHalfTiles) + 0.5f) * backdrop.tileWidth;
            const float y0 = backdrop.baseY + backdrop.tileHeight - 0.02f;
            const float y1 = y0 + 8.0f;
            const Asset::Vertex vertices[4] = {
                Asset::Vertex{ { -half, y1, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } },
                Asset::Vertex{ { half, y1, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } },
                Asset::Vertex{ { half, y0, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } },
                Asset::Vertex{ { -half, y0, backdrop.z }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } },
            };
            const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
            const unsigned char white[4] = { 255, 255, 255, 255 };
            Renderer::StaticMeshDrawItem cap;
            cap.vertexBuffer = Renderer::Buffer::Create(vertices, sizeof(vertices));
            cap.vertexStride = sizeof(Asset::Vertex);
            cap.indexBuffer = Renderer::Buffer::Create(indices, sizeof(indices));
            cap.indexCount = 6;
            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
            cap.worldBuffer = Renderer::Buffer::Create(&world, sizeof(world));
            cap.baseColorTexture = Renderer::Texture::Create(white, 1, 1);
            const DirectX::XMFLOAT4 tint = { backdrop.capColor.x, backdrop.capColor.y, backdrop.capColor.z, 1.0f };
            cap.tintBuffer = Renderer::Buffer::Create(&tint, sizeof(tint));
            backdrop.capItems.push_back(cap);
        }
        backdrop.loaded = true;
        return backdrop;
    }

    namespace
    {
        void SetItemsShown(const std::vector<Renderer::StaticMeshDrawItem>& items, bool shown)
        {
            DirectX::XMFLOAT4X4 world;
            const DirectX::XMMATRIX matrix = shown ? DirectX::XMMatrixIdentity() : DirectX::XMMatrixScaling(0.0f, 0.0f, 0.0f);
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(matrix));
            for (const Renderer::StaticMeshDrawItem& item : items)
            {
                Renderer::Buffer::Update(item.worldBuffer, &world, sizeof(world));
            }
        }

        // Whichever of the two tile sets should be on screen given the two switches.
        void ApplyBackdropState(Backdrop& backdrop)
        {
            const bool useMenu = backdrop.menuShown && !backdrop.menuItems.empty();
            SetItemsShown(backdrop.items, backdrop.visible && !useMenu);
            SetItemsShown(backdrop.menuItems, backdrop.visible && useMenu);
            SetItemsShown(backdrop.capItems, backdrop.visible);
            // The band follows the picture's look: nearly black behind the menu.
            const float k = useMenu ? 0.05f : 1.0f;
            const DirectX::XMFLOAT4 tint = { backdrop.capColor.x * k, backdrop.capColor.y * k, backdrop.capColor.z * k, 1.0f };
            for (const Renderer::StaticMeshDrawItem& item : backdrop.capItems)
            {
                Renderer::Buffer::Update(item.tintBuffer, &tint, sizeof(tint));
            }
        }
    }

    void SetBackdropVisible(Backdrop& backdrop, bool visible)
    {
        if (!backdrop.loaded || backdrop.visible == visible)
        {
            return;
        }
        backdrop.visible = visible;
        ApplyBackdropState(backdrop);
    }

    void SetBackdropMenu(Backdrop& backdrop, bool menu)
    {
        if (!backdrop.loaded || backdrop.menuShown == menu)
        {
            return;
        }
        backdrop.menuShown = menu;
        ApplyBackdropState(backdrop);
    }

    DirectX::XMFLOAT3 BackdropMenuFocus(const Backdrop& backdrop)
    {
        // The skull pile sits in the left part of the picture, around u = 0.2, half way up.
        return { (0.2f - 0.5f) * backdrop.tileWidth, backdrop.baseY + 0.5f * backdrop.tileHeight + 0.15f, backdrop.z };
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
