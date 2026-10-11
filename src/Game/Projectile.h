#pragma once

#if defined(OMD_PROTOTYPE)

#include "MoveTable.h"
#include "Renderer/StaticMeshDrawItem.h"

#include <cstdint>
#include <entt.hpp>
#include <vector>

namespace Engine
{
    struct CollisionEvents;
}

namespace Game
{
    // TODO(OM): replace with a general projectile system (collision layers, multi-hit, trading,
    // per-projectile data, motion-input launch) and particle-based visuals.

    // A live projectile entity. Carries a Hitbox, a MoveSource (so hit resolution reads its
    // damage/stun from the owner's move table like any other attacker) and a Transform.
    struct Projectile
    {
        entt::entity owner = entt::null;
        float velocityX = 0.0f;
        uint32_t ticksRemaining = 0;
        size_t slot = 0; // Index into ProjectileSystem::drawItems.
    };

    // Present on any fighter that can launch a projectile.
    struct ProjectileCaster
    {
        // attackInstanceId of the cast that last spawned one - guards against spawning twice
        // while hitstop holds framesInMove on the spawn frame.
        uint32_t lastSpawnInstanceId = 0xFFFFFFFFu;
    };

    // Fixed pool of placeholder visuals. Unused slots are parked below the stage.
    struct ProjectileSystem
    {
        std::vector<Renderer::StaticMeshDrawItem> drawItems;
        std::vector<entt::entity> slotEntity; // entt::null when the slot is free.
        uint32_t cancelledPairs = 0; // Running count of fireball-vs-fireball cancellations.
    };

    ProjectileSystem CreateProjectileSystem(size_t capacity);

    // Copies the fireball cast + its projectile data entries from `source` into `target`
    // (field-by-field, since a move holding cancels is not copyable).
    void CopyFireballMoves(const MoveTable& source, MoveTable& target);

    // Once per fixed tick, after fighters have updated and before collisions resolve:
    // despawns projectiles whose hit was consumed (or that expired/left the stage), advances
    // the rest, and spawns new ones for fighters whose fireball cast just reached its release
    // frame. `events` is the previous tick's collision result, as the fighters just saw it.
    void UpdateProjectiles(entt::registry& registry, ProjectileSystem& system, const Engine::CollisionEvents& events);

    // Removes every live projectile (round reset).
    void ClearProjectiles(entt::registry& registry, ProjectileSystem& system);

    // Once per render frame: positions the visuals.
    void UpdateProjectileRender(entt::registry& registry, ProjectileSystem& system);
}

#endif // OMD_PROTOTYPE
