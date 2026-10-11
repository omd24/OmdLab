#if defined(OMD_PROTOTYPE)

#include "Projectile.h"

#include "FighterState.h"
#include "GameConstants.h"
#include "Asset/Mesh.h"
#include "Engine/Collision.h"
#include "Engine/Components.h"
#include "Engine/FixedTimestep.h"
#include "Renderer/Buffer.h"
#include "Foundation/Log.h"
#include "Renderer/Texture.h"

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>
#include <utility>

namespace Game
{
    namespace
    {
        constexpr const char* kCastMoveId = "fireball";
        constexpr const char* kHitMoveId = "fireball_projectile";

        constexpr float kSpeed = 6.0f;          // World units per second.
        constexpr float kSpawnForward = 0.7f;   // Release point, in front of the caster...
        constexpr float kSpawnHeight = 0.8f;    // ...low enough that a well-timed jump clears it.
        constexpr float kVisualRadius = 0.22f;
        constexpr DirectX::XMFLOAT3 kHitboxHalfExtents = { 0.25f, 0.18f, 0.3f };
        constexpr uint32_t kMaxLifetimeTicks = 150;

        Renderer::StaticMeshDrawItem CreateSphereDrawItem()
        {
            constexpr int kSegments = 12;
            constexpr int kRings = 8;
            std::vector<Asset::Vertex> vertices;
            for (int ring = 0; ring <= kRings; ++ring)
            {
                const float v = static_cast<float>(ring) / static_cast<float>(kRings);
                const float phi = DirectX::XM_PI * v;
                for (int seg = 0; seg <= kSegments; ++seg)
                {
                    const float u = static_cast<float>(seg) / static_cast<float>(kSegments);
                    const float theta = DirectX::XM_2PI * u;
                    const DirectX::XMFLOAT3 n = { sinf(phi) * cosf(theta), cosf(phi), sinf(phi) * sinf(theta) };
                    Asset::Vertex vertex;
                    vertex.position = { n.x * kVisualRadius, n.y * kVisualRadius, n.z * kVisualRadius };
                    vertex.normal = n;
                    vertex.uv0 = { u, v };
                    vertices.push_back(vertex);
                }
            }
            std::vector<uint32_t> indices;
            for (int ring = 0; ring < kRings; ++ring)
            {
                for (int seg = 0; seg < kSegments; ++seg)
                {
                    const uint32_t a = static_cast<uint32_t>(ring * (kSegments + 1) + seg);
                    const uint32_t b = a + static_cast<uint32_t>(kSegments + 1);
                    indices.insert(indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
                }
            }

            const uint8_t whitePixels[4 * 4] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
            Renderer::StaticMeshDrawItem item;
            item.vertexBuffer = Renderer::Buffer::Create(vertices.data(), vertices.size() * sizeof(Asset::Vertex));
            item.vertexStride = sizeof(Asset::Vertex);
            item.indexBuffer = Renderer::Buffer::Create(indices.data(), indices.size() * sizeof(uint32_t));
            item.indexCount = static_cast<unsigned int>(indices.size());
            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixTranslation(0.0f, -100.0f, 0.0f)));
            item.worldBuffer = Renderer::Buffer::Create(&world, sizeof(world));
            item.baseColorTexture = Renderer::Texture::Create(whitePixels, 2, 2);
            const DirectX::XMFLOAT4 orange = { 1.0f, 0.5f, 0.12f, 1.0f };
            item.tintBuffer = Renderer::Buffer::Create(&orange, sizeof(orange));
            return item;
        }

        void Release(entt::registry& registry, ProjectileSystem& system, entt::entity entity)
        {
            const size_t slot = registry.get<Projectile>(entity).slot;
            if (slot < system.slotEntity.size())
            {
                system.slotEntity[slot] = entt::null;
            }
            registry.destroy(entity);
        }
    }

    ProjectileSystem CreateProjectileSystem(size_t capacity)
    {
        ProjectileSystem system;
        for (size_t i = 0; i < capacity; ++i)
        {
            system.drawItems.push_back(CreateSphereDrawItem());
        }
        system.slotEntity.assign(capacity, entt::null);
        return system;
    }

    void CopyFireballMoves(const MoveTable& source, MoveTable& target)
    {
        for (const char* id : { kCastMoveId, kHitMoveId })
        {
            const MoveDefinition* from = source.FindById(id);
            if (from == nullptr || target.FindById(id) != nullptr)
            {
                continue;
            }
            MoveDefinition copy;
            copy.id = from->id;
            copy.displayName = from->displayName;
            copy.animationClip = from->animationClip;
            copy.inputButton = from->inputButton;
            copy.stance = from->stance;
            copy.startupFrames = from->startupFrames;
            copy.activeFrames = from->activeFrames;
            copy.recoveryFrames = from->recoveryFrames;
            copy.onHitStunFrames = from->onHitStunFrames;
            copy.onBlockStunFrames = from->onBlockStunFrames;
            copy.damage = from->damage;
            copy.guardHeight = from->guardHeight;
            copy.hitboxes = from->hitboxes;
            copy.impact = from->impact;
            copy.timingContactFraction = from->timingContactFraction;
            copy.timingWindupFraction = from->timingWindupFraction;
            copy.timingSnapFrames = from->timingSnapFrames;
            copy.hitStopFrames = from->hitStopFrames;
            copy.lungeDistance = from->lungeDistance;
            copy.timingStartFraction = from->timingStartFraction;
            copy.timingOutFrames = from->timingOutFrames;
            copy.timingEndFraction = from->timingEndFraction;
            copy.timingCurveActive = from->timingCurveActive;
            target.moves.push_back(std::move(copy));
        }
    }

    void UpdateProjectiles(entt::registry& registry, ProjectileSystem& system, const Engine::CollisionEvents& events)
    {
        // Despawn: a projectile is spent once its victim has actually registered the hit (the
        // victim's own last-hit bookkeeping names it) - not merely on overlap, which a frozen
        // victim could miss while the projectile vanished.
        for (const Engine::HitEvent& hit : events.hits)
        {
            if (!registry.valid(hit.attacker) || !registry.all_of<Projectile>(hit.attacker) || !registry.valid(hit.defender))
            {
                continue;
            }
            const FighterState* victim = registry.try_get<FighterState>(hit.defender);
            if (victim != nullptr && victim->lastHitByAttacker == hit.attacker)
            {
                Release(registry, system, hit.attacker);
            }
        }

        // Advance / expire.
        constexpr float kDt = Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        std::vector<entt::entity> expired;
        for (const entt::entity entity : registry.view<Projectile, Engine::Transform>())
        {
            Projectile& projectile = registry.get<Projectile>(entity);
            Engine::Transform& transform = registry.get<Engine::Transform>(entity);
            transform.position.x += projectile.velocityX * kDt;
            if (projectile.ticksRemaining == 0 || --projectile.ticksRemaining == 0 ||
                std::fabs(transform.position.x) > kStageHalfWidth + 1.0f)
            {
                expired.push_back(entity);
            }
        }
        for (const entt::entity entity : expired)
        {
            Foundation::Log::Write(Foundation::Log::Severity::Info, "Projectile", "fireball missed (left the stage or timed out)");
            Release(registry, system, entity);
        }

        // Two fireballs from different owners that meet cancel each other.
        {
            std::vector<entt::entity> live;
            for (const entt::entity entity : registry.view<Projectile, Engine::Transform>())
            {
                live.push_back(entity);
            }
            std::vector<entt::entity> cancelled;
            for (size_t i = 0; i < live.size(); ++i)
            {
                for (size_t j = i + 1; j < live.size(); ++j)
                {
                    const Projectile& a = registry.get<Projectile>(live[i]);
                    const Projectile& b = registry.get<Projectile>(live[j]);
                    if (a.owner == b.owner)
                    {
                        continue;
                    }
                    const float dx = std::fabs(
                        registry.get<Engine::Transform>(live[i]).position.x - registry.get<Engine::Transform>(live[j]).position.x);
                    if (dx < kHitboxHalfExtents.x * 2.0f)
                    {
                        cancelled.push_back(live[i]);
                        cancelled.push_back(live[j]);
                    }
                }
            }
            for (const entt::entity entity : cancelled)
            {
                if (registry.valid(entity))
                {
                    Foundation::Log::Write(Foundation::Log::Severity::Info, "Projectile", "fireballs cancelled each other");
                    ++system.cancelledPairs; // Counted per projectile; two per pair.
                    Release(registry, system, entity);
                }
            }
        }

        // Spawn: at the cast's release frame (end of startup), one live projectile per owner.
        for (const entt::entity caster : registry.view<ProjectileCaster, FighterState, Engine::Transform, MoveSource>())
        {
            ProjectileCaster& casterData = registry.get<ProjectileCaster>(caster);
            const FighterState& state = registry.get<FighterState>(caster);
            const MoveSource& source = registry.get<MoveSource>(caster);
            if (state.currentState != "Attack" || state.activeMoveId != kCastMoveId || source.moveTable == nullptr)
            {
                continue;
            }
            const MoveDefinition* cast = source.moveTable->FindById(kCastMoveId);
            const int32_t hitMoveIndex = source.moveTable->IndexOf(kHitMoveId);
            // Released at the throw pose: the end of the wind-up plus, for a reshaped cast, the
            // snap into the throw.
            const uint32_t releaseFrame =
                cast != nullptr ? cast->startupFrames + (cast->timingCurveActive ? cast->timingSnapFrames : 0u) : 0u;
            if (cast == nullptr || hitMoveIndex < 0 || state.framesInMove != releaseFrame ||
                casterData.lastSpawnInstanceId == state.attackInstanceId)
            {
                continue;
            }

            bool ownerHasLive = false;
            for (const entt::entity entity : registry.view<Projectile>())
            {
                ownerHasLive = ownerHasLive || registry.get<Projectile>(entity).owner == caster;
            }
            size_t slot = system.slotEntity.size();
            for (size_t i = 0; i < system.slotEntity.size(); ++i)
            {
                if (system.slotEntity[i] == entt::null)
                {
                    slot = i;
                    break;
                }
            }
            if (ownerHasLive || slot == system.slotEntity.size())
            {
                continue;
            }
            casterData.lastSpawnInstanceId = state.attackInstanceId;
            Foundation::Log::Write(Foundation::Log::Severity::Info, "Projectile", "fireball released");

            const float direction = state.facingRight ? 1.0f : -1.0f;
            const Engine::Transform& casterTransform = registry.get<Engine::Transform>(caster);
            const entt::entity entity = registry.create();
            Engine::Transform transform;
            transform.position = { casterTransform.position.x + direction * kSpawnForward, casterTransform.position.y + kSpawnHeight,
                                   casterTransform.position.z };
            registry.emplace<Engine::Transform>(entity, transform);
            Engine::Hitbox hitbox;
            hitbox.box.halfExtents = kHitboxHalfExtents;
            hitbox.moveId = static_cast<uint32_t>(hitMoveIndex);
            hitbox.attackInstanceId = state.attackInstanceId;
            hitbox.ignoreEntity = caster;
            registry.emplace<Engine::Hitbox>(entity, hitbox);
            registry.emplace<MoveSource>(entity, source);
            registry.emplace<Projectile>(entity, Projectile{ caster, direction * kSpeed, kMaxLifetimeTicks, slot });
            system.slotEntity[slot] = entity;
        }
    }

    void ClearProjectiles(entt::registry& registry, ProjectileSystem& system)
    {
        std::vector<entt::entity> all;
        for (const entt::entity entity : registry.view<Projectile>())
        {
            all.push_back(entity);
        }
        for (const entt::entity entity : all)
        {
            Release(registry, system, entity);
        }
    }

    void UpdateProjectileRender(entt::registry& registry, ProjectileSystem& system)
    {
        for (size_t i = 0; i < system.drawItems.size(); ++i)
        {
            DirectX::XMFLOAT3 position = { 0.0f, -100.0f, 0.0f };
            const entt::entity entity = system.slotEntity[i];
            if (entity != entt::null && registry.valid(entity))
            {
                position = registry.get<Engine::Transform>(entity).position;
            }
            DirectX::XMFLOAT4X4 world;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixTranspose(DirectX::XMMatrixTranslation(position.x, position.y, position.z)));
            Renderer::Buffer::Update(system.drawItems[i].worldBuffer, &world, sizeof(world));
        }
    }
}

#endif // OMD_PROTOTYPE
