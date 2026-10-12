#if defined(OMD_PROTOTYPE)

#include "Vfx.h"

#include "FighterState.h"
#include "GroundPlane.h"
#include "Projectile.h"
#include "Asset/Mesh.h"
#include "Engine/Collision.h"
#include "Engine/Components.h"
#include "Renderer/Buffer.h"
#include "Renderer/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Game
{
    namespace
    {
        using DirectX::XMFLOAT3;
        using DirectX::XMFLOAT4;

        // Same directional light LitTextured.hlsl hardcodes - used to cancel its shading on
        // the camera-facing quads so a particle's colour reads as authored.
        constexpr XMFLOAT3 kLightDirection = { 0.4f, -1.0f, 0.3f };

        constexpr float kTowardCameraOffset = 0.35f; // Keeps effects in front of the fighter's mesh.

        Renderer::StaticMeshDrawItem CreateQuadDrawItem(Renderer::TextureHandle sharedTexture)
        {
            std::vector<Asset::Vertex> vertices(4);
            const float corners[4][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } };
            const float uvs[4][2] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
            for (int i = 0; i < 4; ++i)
            {
                vertices[i].position = { corners[i][0], corners[i][1], 0.0f };
                vertices[i].normal = { 0.0f, 0.0f, -1.0f }; // Faces the camera for rows (right, up, forward).
                vertices[i].uv0 = { uvs[i][0], uvs[i][1] };
            }
            const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };

            Renderer::StaticMeshDrawItem item;
            item.vertexBuffer = Renderer::Buffer::Create(vertices.data(), vertices.size() * sizeof(Asset::Vertex));
            item.vertexStride = sizeof(Asset::Vertex);
            item.indexBuffer = Renderer::Buffer::Create(indices, sizeof(indices));
            item.indexCount = 6;
            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(0.0f, 0.0f, 0.0f)));
            item.worldBuffer = Renderer::Buffer::Create(&world, sizeof(world));
            item.baseColorTexture = sharedTexture;
            const XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, 0.0f };
            item.tintBuffer = Renderer::Buffer::Create(&tint, sizeof(tint));
            item.transparent = true;
            return item;
        }

        // White glow with a soft radial alpha falloff, brightest at the centre.
        Renderer::TextureHandle CreateGlowTexture()
        {
            constexpr unsigned int kSize = 32;
            std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4);
            const float center = (static_cast<float>(kSize) - 1.0f) * 0.5f;
            for (unsigned int y = 0; y < kSize; ++y)
            {
                for (unsigned int x = 0; x < kSize; ++x)
                {
                    const float dx = (static_cast<float>(x) - center) / center;
                    const float dy = (static_cast<float>(y) - center) / center;
                    const float falloff = 1.0f - std::clamp(std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                    const float alpha = falloff * falloff * (3.0f - 2.0f * falloff);
                    const size_t offset = (static_cast<size_t>(y) * kSize + x) * 4;
                    pixels[offset + 0] = 255;
                    pixels[offset + 1] = 255;
                    pixels[offset + 2] = 255;
                    pixels[offset + 3] = static_cast<uint8_t>(alpha * 255.0f);
                }
            }
            return Renderer::Texture::Create(pixels.data(), kSize, kSize);
        }

        Engine::ParticleBurst Sparks(const VfxEvent& event, int count, const XMFLOAT4& hot, const XMFLOAT4& cool, float sizeScale)
        {
            Engine::ParticleBurst burst;
            burst.count = count;
            // Thrown forward along the attack and up a little, with a wide scatter.
            burst.direction = { event.direction.x, 0.35f, 0.0f };
            burst.spread = 0.75f;
            burst.speedMin = 2.2f + 1.5f * event.intensity;
            burst.speedMax = 5.0f + 3.5f * event.intensity;
            burst.lifetimeMin = 0.14f;
            burst.lifetimeMax = 0.30f;
            burst.size0 = 0.13f * sizeScale;
            burst.size1 = 0.03f * sizeScale;
            burst.stretch = 3.6f;
            burst.gravity = 5.0f;
            burst.drag = 2.5f;
            burst.color0 = hot;
            burst.color1 = cool;
            return burst;
        }

        Engine::ParticleBurst Flash(float size, const XMFLOAT4& color, float lifetime)
        {
            Engine::ParticleBurst burst;
            burst.count = 1;
            burst.spread = 0.0f;
            burst.speedMin = 0.0f;
            burst.speedMax = 0.0f;
            burst.lifetimeMin = lifetime;
            burst.lifetimeMax = lifetime;
            burst.size0 = size * 0.55f;
            burst.size1 = size;
            burst.color0 = color;
            burst.color1 = { color.x, color.y * 0.8f, color.z * 0.6f, 0.0f };
            return burst;
        }

        Engine::ParticleBurst Puff(int count, XMFLOAT3 direction, float spread, float sizeEnd, const XMFLOAT4& color)
        {
            Engine::ParticleBurst burst;
            burst.count = count;
            burst.direction = direction;
            burst.spread = spread;
            burst.speedMin = 0.4f;
            burst.speedMax = 1.3f;
            burst.lifetimeMin = 0.30f;
            burst.lifetimeMax = 0.55f;
            burst.size0 = sizeEnd * 0.35f;
            burst.size1 = sizeEnd;
            burst.drag = 3.0f;
            burst.gravity = -0.4f; // Drifts up.
            burst.color0 = color;
            burst.color1 = { color.x, color.y, color.z, 0.0f };
            return burst;
        }

        constexpr size_t kPoolCount = 4;
        constexpr float kPoolGiveUpSeconds = 8.0f; // Waiting for a landing that never comes.
        constexpr float kPoolSpreadSeconds = 2.4f; // Time constant of the ease-out.
        constexpr float kPoolRadiusX = 1.4f;       // Along the stage.
        constexpr float kPoolRadiusZ = 1.0f; 
        constexpr float kPoolHeight = 0.02f;       // Above the floor surface.

        // A flat blob with an irregular rim, unit radius, lying on y = 0.
        Renderer::StaticMeshDrawItem CreatePoolDrawItem(Renderer::TextureHandle texture, int seed)
        {
            constexpr int kSegments = 32;
            std::vector<Asset::Vertex> vertices;
            vertices.push_back(Asset::Vertex{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.5f, 0.5f } });
            const float phase = static_cast<float>(seed) * 1.7f;
            for (int i = 0; i < kSegments; ++i)
            {
                const float angle = DirectX::XM_2PI * static_cast<float>(i) / static_cast<float>(kSegments);
                const float wobble = 0.5f * std::sin(angle * 3.0f + phase) + 0.3f * std::sin(angle * 5.0f + phase * 2.3f) +
                                     0.2f * std::sin(angle * 9.0f + phase * 0.7f);
                const float radius = 0.88f + 0.12f * wobble;
                vertices.push_back(Asset::Vertex{ { radius * std::cos(angle), 0.0f, radius * std::sin(angle) },
                                                  { 0.0f, 1.0f, 0.0f },
                                                  { 0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle) } });
            }
            std::vector<uint32_t> indices;
            for (int i = 0; i < kSegments; ++i)
            {
                indices.push_back(0);
                indices.push_back(static_cast<uint32_t>(1 + i));
                indices.push_back(static_cast<uint32_t>(1 + (i + 1) % kSegments));
            }

            Renderer::StaticMeshDrawItem item;
            item.vertexBuffer = Renderer::Buffer::Create(vertices.data(), vertices.size() * sizeof(Asset::Vertex));
            item.vertexStride = sizeof(Asset::Vertex);
            item.indexBuffer = Renderer::Buffer::Create(indices.data(), indices.size() * sizeof(uint32_t));
            item.indexCount = static_cast<unsigned int>(indices.size());
            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixScaling(0.0f, 0.0f, 0.0f)));
            item.worldBuffer = Renderer::Buffer::Create(&world, sizeof(world));
            item.baseColorTexture = texture;
            const XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, 0.0f };
            item.tintBuffer = Renderer::Buffer::Create(&tint, sizeof(tint));
            item.transparent = true;
            return item;
        }

        // Dark red, a little lighter toward the middle (as if wet), edge fading out.
        Renderer::TextureHandle CreatePoolTexture()
        {
            constexpr unsigned int kSize = 64;
            std::vector<uint8_t> pixels(static_cast<size_t>(kSize) * kSize * 4);
            const float center = (static_cast<float>(kSize) - 1.0f) * 0.5f;
            for (unsigned int y = 0; y < kSize; ++y)
            {
                for (unsigned int x = 0; x < kSize; ++x)
                {
                    const float dx = (static_cast<float>(x) - center) / center;
                    const float dy = (static_cast<float>(y) - center) / center;
                    const float d = std::sqrt(dx * dx + dy * dy);
                    const float edge = 1.0f - std::clamp((d - 0.72f) / 0.28f, 0.0f, 1.0f);
                    const float alpha = edge * edge * (3.0f - 2.0f * edge) * 0.93f;
                    const float wet = 1.0f - std::clamp(d, 0.0f, 1.0f);
                    const size_t offset = (static_cast<size_t>(y) * kSize + x) * 4;
                    pixels[offset + 0] = static_cast<uint8_t>(95.0f + 45.0f * wet);
                    pixels[offset + 1] = static_cast<uint8_t>(4.0f + 8.0f * wet);
                    pixels[offset + 2] = static_cast<uint8_t>(8.0f + 6.0f * wet);
                    pixels[offset + 3] = static_cast<uint8_t>(alpha * 255.0f);
                }
            }
            return Renderer::Texture::Create(pixels.data(), kSize, kSize);
        }

        void Spawn(Engine::ParticleSystem& particles, const VfxEvent& event, const XMFLOAT3& towardCamera)
        {
            const XMFLOAT3 p = { event.position.x + towardCamera.x * kTowardCameraOffset,
                                 event.position.y + towardCamera.y * kTowardCameraOffset,
                                 event.position.z + towardCamera.z * kTowardCameraOffset };
            const float i = std::clamp(event.intensity, 0.0f, 1.0f);
            switch (event.type)
            {
            case VfxType::Hit:
            case VfxType::Knockout:
            {
                const bool knockout = event.type == VfxType::Knockout;
                const float scale = knockout ? 1.7f : 1.0f;
                particles.Burst(Flash((0.45f + 0.4f * i) * scale, { 2.8f, 1.9f, 0.8f, 0.85f }, knockout ? 0.16f : 0.11f), p);
                particles.Burst(Sparks(event, (knockout ? 26 : 8) + static_cast<int>(8.0f * i), { 2.6f, 1.6f, 0.5f, 1.0f },
                                       { 1.8f, 0.45f, 0.08f, 0.0f }, scale), p);
                if (knockout)
                {
                    Engine::ParticleBurst blood;
                    blood.count = 22;
                    blood.direction = { event.direction.x * 0.6f, 0.8f, 0.0f };
                    blood.spread = 0.55f;
                    blood.speedMin = 1.2f;
                    blood.speedMax = 4.6f;
                    blood.lifetimeMin = 0.35f;
                    blood.lifetimeMax = 0.75f;
                    blood.size0 = 0.07f;
                    blood.size1 = 0.04f;
                    blood.stretch = 2.2f;
                    blood.gravity = 9.0f;
                    blood.drag = 0.6f;
                    blood.color0 = { 0.55f, 0.02f, 0.03f, 1.0f };
                    blood.color1 = { 0.32f, 0.0f, 0.02f, 0.9f };
                    particles.Burst(blood, p);
                }
                particles.Burst(Puff(knockout ? 8 : 4, { event.direction.x * 0.4f, 0.5f, 0.0f }, 0.8f, 0.30f * scale,
                                     { 0.95f, 0.9f, 0.82f, 0.5f }), p);
                break;
            }
            case VfxType::Block:
                particles.Burst(Flash(0.26f + 0.1f * i, { 1.6f, 2.2f, 3.0f, 0.8f }, 0.08f), p);
                particles.Burst(Sparks(event, 6, { 1.6f, 2.2f, 3.0f, 1.0f }, { 0.5f, 0.9f, 2.4f, 0.0f }, 0.8f), p);
                break;
            case VfxType::BloodPool:
                break; // Handled by the pool list, not the particles.
            case VfxType::Landing:
                for (const float side : { -1.0f, 1.0f })
                {
                    particles.Burst(Puff(3, { side, 0.15f, 0.0f }, 0.25f, 0.26f, { 0.8f, 0.75f, 0.68f, 0.45f }), p);
                }
                break;
            case VfxType::JumpOff:
                particles.Burst(Puff(3, { 0.0f, 0.2f, 0.0f }, 0.7f, 0.20f, { 0.8f, 0.75f, 0.68f, 0.35f }), p);
                break;
            case VfxType::FireballImpact:
            {
                particles.Burst(Flash(0.9f, { 3.0f, 1.7f, 0.55f, 0.9f }, 0.18f), p);
                Engine::ParticleBurst embers = Sparks(event, 20, { 3.0f, 1.8f, 0.5f, 1.0f }, { 1.6f, 0.3f, 0.05f, 0.0f }, 1.3f);
                embers.spread = 1.0f;
                embers.lifetimeMax = 0.4f;
                particles.Burst(embers, p);
                particles.Burst(Puff(5, { 0.0f, 0.4f, 0.0f }, 0.9f, 0.36f, { 0.35f, 0.3f, 0.28f, 0.5f }), p);
                break;
            }
            case VfxType::FireballClash:
            {
                particles.Burst(Flash(0.8f, { 3.0f, 2.4f, 1.4f, 0.9f }, 0.16f), p);
                Engine::ParticleBurst embers = Sparks(event, 16, { 3.0f, 2.2f, 0.9f, 1.0f }, { 1.6f, 0.4f, 0.05f, 0.0f }, 1.1f);
                embers.spread = 1.0f;
                particles.Burst(embers, p);
                break;
            }
            }
        }
    }

    VfxSystem CreateVfxSystem(size_t capacity)
    {
        VfxSystem system(capacity);
        const Renderer::TextureHandle texture = CreateGlowTexture();
        for (size_t i = 0; i < capacity; ++i)
        {
            system.drawItems.push_back(CreateQuadDrawItem(texture));
        }
        system.visible.assign(capacity, 0);
        const Renderer::TextureHandle poolTexture = CreatePoolTexture();
        for (size_t i = 0; i < kPoolCount; ++i)
        {
            system.poolItems.push_back(CreatePoolDrawItem(poolTexture, static_cast<int>(i) + 1));
        }
        system.pools.assign(kPoolCount, BloodPool{});
        return system;
    }

    void PushVfxEvent(VfxSystem& system, const VfxEvent& event)
    {
        system.queue.push_back(event);
    }

    XMFLOAT3 ComputeContactPoint(entt::registry& registry, entt::entity attacker, entt::entity defender)
    {
        const Engine::Transform* defenderTransform = registry.valid(defender) ? registry.try_get<Engine::Transform>(defender) : nullptr;
        if (defenderTransform == nullptr)
        {
            return { 0.0f, 1.0f, 0.0f };
        }
        const Engine::Transform* attackerTransform = registry.valid(attacker) ? registry.try_get<Engine::Transform>(attacker) : nullptr;
        const float towardAttacker =
            attackerTransform != nullptr && attackerTransform->position.x < defenderTransform->position.x ? -1.0f : 1.0f;

        const Engine::Hitbox* hitbox = attackerTransform != nullptr ? registry.try_get<Engine::Hitbox>(attacker) : nullptr;
        const Engine::Hurtbox* hurtbox = registry.try_get<Engine::Hurtbox>(defender);
        if (hitbox != nullptr && hurtbox != nullptr)
        {
            XMFLOAT3 hitMin;
            XMFLOAT3 hitMax;
            Engine::ComputeWorldAabb(hitbox->box, *attackerTransform, hitMin, hitMax);
            for (const Engine::CollisionBox& box : hurtbox->boxes)
            {
                XMFLOAT3 hurtMin;
                XMFLOAT3 hurtMax;
                Engine::ComputeWorldAabb(box, *defenderTransform, hurtMin, hurtMax);
                const float loX = (std::max)(hitMin.x, hurtMin.x);
                const float hiX = (std::min)(hitMax.x, hurtMax.x);
                const float loY = (std::max)(hitMin.y, hurtMin.y);
                const float hiY = (std::min)(hitMax.y, hurtMax.y);
                if (loX <= hiX && loY <= hiY)
                {
                    return { (loX + hiX) * 0.5f, (loY + hiY) * 0.5f, defenderTransform->position.z };
                }
            }
        }
        return { defenderTransform->position.x + towardAttacker * 0.2f, defenderTransform->position.y + 1.0f,
                 defenderTransform->position.z };
    }

    void UpdateVfx(entt::registry& registry, VfxSystem& system, const Engine::Camera& camera, float deltaSeconds)
    {
        const Engine::CameraBasis basis = Engine::ComputeCameraBasis(camera);
        const XMFLOAT3 towardCamera = { -basis.forward.x, -basis.forward.y, -basis.forward.z };

        for (const VfxEvent& event : system.queue)
        {
            if (event.type == VfxType::BloodPool)
            {
                bool alreadyWaiting = false;
                for (const BloodPool& other : system.pools)
                {
                    alreadyWaiting = alreadyWaiting || (other.used && !other.placed && other.subject == event.subject);
                }
                if (alreadyWaiting)
                {
                    continue;
                }
                BloodPool& pool = system.pools[system.nextPool];
                system.nextPool = (system.nextPool + 1) % system.pools.size();
                pool = BloodPool{};
                pool.used = true;
                pool.subject = event.subject;
                pool.fallDirection = event.direction.x;
                pool.landedFrame = event.landedFrame;
                pool.position = event.position;
                continue;
            }
            Spawn(system.particles, event, towardCamera);
        }
        system.queue.clear();

        // Fireballs: a soft glow around the core every frame plus embers laid along the path
        // travelled since the last frame.
        constexpr size_t kMaxProjectileSlots = 8;
        if (system.projectileLast.size() < kMaxProjectileSlots)
        {
            system.projectileLast.resize(kMaxProjectileSlots);
            system.projectileLastValid.assign(kMaxProjectileSlots, 0);
        }
        std::vector<char> seen(kMaxProjectileSlots, 0);
        for (const entt::entity entity : registry.view<Projectile, Engine::Transform>())
        {
            const Projectile& projectile = registry.get<Projectile>(entity);
            const XMFLOAT3 position = registry.get<Engine::Transform>(entity).position;
            const XMFLOAT3 front = { position.x + towardCamera.x * 0.15f, position.y + towardCamera.y * 0.15f,
                                     position.z + towardCamera.z * 0.15f };

            Engine::ParticleBurst glow;
            glow.count = 1;
            glow.spread = 0.0f;
            glow.speedMin = 0.0f;
            glow.speedMax = 0.0f;
            glow.lifetimeMin = (std::max)(deltaSeconds * 1.5f, 0.02f);
            glow.lifetimeMax = glow.lifetimeMin;
            glow.size0 = 0.95f;
            glow.size1 = 0.95f;
            glow.color0 = { 3.0f, 1.5f, 0.45f, 0.5f };
            glow.color1 = glow.color0;
            system.particles.Burst(glow, front);
            glow.size0 = 0.42f;
            glow.size1 = 0.42f;
            glow.color0 = { 3.0f, 2.6f, 1.6f, 0.8f };
            glow.color1 = glow.color0;
            system.particles.Burst(glow, front);

            const size_t slot = projectile.slot < kMaxProjectileSlots ? projectile.slot : kMaxProjectileSlots - 1;
            seen[slot] = 1;
            const XMFLOAT3 from = system.projectileLastValid[slot] ? system.projectileLast[slot] : position;
            Engine::ParticleBurst trail;
            trail.count = 3;
            trail.direction = { projectile.velocityX > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f };
            trail.spread = 0.8f;
            trail.speedMin = 0.2f;
            trail.speedMax = 1.2f;
            trail.lifetimeMin = 0.18f;
            trail.lifetimeMax = 0.36f;
            trail.size0 = 0.16f;
            trail.size1 = 0.03f;
            trail.drag = 2.0f;
            trail.gravity = -1.2f;
            trail.color0 = { 3.0f, 1.5f, 0.4f, 0.9f };
            trail.color1 = { 1.4f, 0.25f, 0.05f, 0.0f };
            system.particles.EmitAlong(trail, from, front);
            system.projectileLast[slot] = front;
            system.projectileLastValid[slot] = 1;
        }
        for (size_t i = 0; i < kMaxProjectileSlots; ++i)
        {
            if (!seen[i])
            {
                system.projectileLastValid[i] = 0;
            }
        }

        system.particles.Update(deltaSeconds);
    }

    void UpdateVfxRender(VfxSystem& system, const Engine::Camera& camera)
    {
        using namespace DirectX;
        const Engine::CameraBasis basis = Engine::ComputeCameraBasis(camera);
        const XMVECTOR right = XMLoadFloat3(&basis.right);
        const XMVECTOR up = XMLoadFloat3(&basis.up);
        const XMVECTOR forward = XMLoadFloat3(&basis.forward);

        // Reciprocal of the shader's lighting for a camera-facing normal.
        const XMVECTOR toLight = XMVector3Normalize(XMVectorNegate(XMLoadFloat3(&kLightDirection)));
        const float diffuse = (std::max)(XMVectorGetX(XMVector3Dot(XMVectorNegate(forward), toLight)), 0.0f);
        const float lighting = std::clamp(0.25f + diffuse * 0.85f, 0.25f, 1.0f);
        const float compensation = 1.0f / lighting;

        const std::vector<Engine::Particle>& particles = system.particles.Particles();
        for (size_t i = 0; i < particles.size() && i < system.drawItems.size(); ++i)
        {
            const Engine::Particle& particle = particles[i];
            const bool alive = particle.lifetime > 0.0f;
            if (!alive)
            {
                if (system.visible[i])
                {
                    system.visible[i] = 0;
                    const XMFLOAT4 hidden = { 1.0f, 1.0f, 1.0f, 0.0f };
                    Renderer::Buffer::Update(system.drawItems[i].tintBuffer, &hidden, sizeof(hidden));
                }
                continue;
            }
            system.visible[i] = 1;

            const float t = std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f);
            const float size = particle.size0 + (particle.size1 - particle.size0) * t;

            // Orient the quad's long axis along the particle's screen-space velocity.
            XMVECTOR axisA = right;
            XMVECTOR axisB = up;
            float lengthScale = 1.0f;
            if (particle.stretch > 1.0f)
            {
                const XMVECTOR velocity = XMLoadFloat3(&particle.velocity);
                const float vx = XMVectorGetX(XMVector3Dot(velocity, right));
                const float vy = XMVectorGetX(XMVector3Dot(velocity, up));
                const float speed = std::sqrt(vx * vx + vy * vy);
                if (speed > 0.05f)
                {
                    const float c = vx / speed;
                    const float s = vy / speed;
                    axisA = XMVectorAdd(XMVectorScale(right, c), XMVectorScale(up, s));
                    axisB = XMVectorAdd(XMVectorScale(right, -s), XMVectorScale(up, c));
                    lengthScale = particle.stretch;
                }
            }
            const XMVECTOR a = XMVectorScale(axisA, size * lengthScale);
            const XMVECTOR b = XMVectorScale(axisB, size);
            const XMVECTOR f = XMVectorScale(forward, size);
            const XMVECTOR p = XMVectorSet(particle.position.x, particle.position.y, particle.position.z, 1.0f);
            XMMATRIX world(a, b, f, p);
            world.r[0] = XMVectorSetW(world.r[0], 0.0f);
            world.r[1] = XMVectorSetW(world.r[1], 0.0f);
            world.r[2] = XMVectorSetW(world.r[2], 0.0f);
            XMFLOAT4X4 worldForGpu;
            XMStoreFloat4x4(&worldForGpu, XMMatrixTranspose(world));
            Renderer::Buffer::Update(system.drawItems[i].worldBuffer, &worldForGpu, sizeof(worldForGpu));

            const XMFLOAT4 tint = {
                (particle.color0.x + (particle.color1.x - particle.color0.x) * t) * compensation,
                (particle.color0.y + (particle.color1.y - particle.color0.y) * t) * compensation,
                (particle.color0.z + (particle.color1.z - particle.color0.z) * t) * compensation,
                particle.color0.w + (particle.color1.w - particle.color0.w) * t,
            };
            Renderer::Buffer::Update(system.drawItems[i].tintBuffer, &tint, sizeof(tint));
        }
    }

    void UpdateVfxPools(entt::registry& registry, VfxSystem& system, float deltaSeconds)
    {
        for (size_t i = 0; i < system.pools.size(); ++i)
        {
            BloodPool& pool = system.pools[i];
            const Renderer::StaticMeshDrawItem& item = system.poolItems[i];
            if (!pool.used)
            {
                continue;
            }
            if (!pool.placed)
            {
                pool.waited += deltaSeconds;
                const FighterState* state = registry.valid(pool.subject) ? registry.try_get<FighterState>(pool.subject) : nullptr;
                const bool down = state != nullptr && state->currentState == "KO";
                pool.seenDown = pool.seenDown || down;
                // Gave up: never went down, got up again before landing, or vanished.
                if (state == nullptr || pool.waited > kPoolGiveUpSeconds || (pool.seenDown && !down))
                {
                    pool.used = false;
                    continue;
                }
                if (!down || state->framesInState < pool.landedFrame)
                {
                    continue;
                }
                // Under the fallen body: where the fighter has ended up, nudged along the fall.
                const XMFLOAT3 body = registry.get<Engine::Transform>(pool.subject).position;
                pool.position = { body.x + pool.fallDirection * 0.4f, 0.0f, body.z };
                pool.placed = true;
            }
            pool.age += deltaSeconds;
            const float t = pool.age;
            const float spread = 1.0f - std::exp(-t / kPoolSpreadSeconds);
            const float scaleX = 0.12f + (kPoolRadiusX - 0.12f) * spread;
            const float scaleZ = 0.10f + (kPoolRadiusZ - 0.10f) * spread;
            const DirectX::XMMATRIX world =
                DirectX::XMMatrixScaling(scaleX, 1.0f, scaleZ) *
                DirectX::XMMatrixTranslation(pool.position.x, kFloorSurfaceY + kPoolHeight, pool.position.z);
            DirectX::XMFLOAT4X4 worldForGpu;
            DirectX::XMStoreFloat4x4(&worldForGpu, DirectX::XMMatrixTranspose(world));
            Renderer::Buffer::Update(item.worldBuffer, &worldForGpu, sizeof(worldForGpu));
            const XMFLOAT4 tint = { 1.0f, 1.0f, 1.0f, std::clamp(t / 0.35f, 0.0f, 1.0f) };
            Renderer::Buffer::Update(item.tintBuffer, &tint, sizeof(tint));
        }
    }

    void ClearVfx(VfxSystem& system)
    {
        for (size_t i = 0; i < system.pools.size(); ++i)
        {
            if (system.pools[i].used)
            {
                const XMFLOAT4 hidden = { 1.0f, 1.0f, 1.0f, 0.0f };
                Renderer::Buffer::Update(system.poolItems[i].tintBuffer, &hidden, sizeof(hidden));
            }
            system.pools[i] = BloodPool{};
        }
        system.queue.clear();
        system.particles.Clear();
        std::fill(system.projectileLastValid.begin(), system.projectileLastValid.end(), static_cast<char>(0));
    }
}

#endif // OMD_PROTOTYPE
