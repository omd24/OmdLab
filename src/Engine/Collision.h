#pragma once

#include "Components.h"
#include "Renderer/DebugDrawLine.h"

#include <DirectXMath.h>
#include <cstdint>
#include <entt.hpp>
#include <vector>

namespace Engine
{
    // Shared shape representation for hitbox/hurtbox/trigger volumes alike - an axis-aligned box
    // in the owning entity's local space (offset + half-extents, both scaled by the entity's
    // Transform::scale). Deliberately ignores Transform::rotation for now: 2D fighting games
    // author hitboxes as axis-aligned rectangles, not rotated ones, so ignoring rotation matches
    // the actual need rather than building general OBB-vs-OBB math nothing currently uses. Bone/
    // socket attachment (for real per-move hitbox data, once the moveset/state-machine steps need
    // it) is a natural future extension of this same shape - nothing today attaches a volume to
    // anything more specific than "this entity."
    struct CollisionBox
    {
        DirectX::XMFLOAT3 offset = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f };
    };

    // Offense volume - present on an entity only while some move's hitbox is currently active;
    // adding/removing this component as a move's active-frame window opens and closes is the
    // caller's job (Game), not this module's. moveId is opaque to Engine - Game's own move-
    // identifier space (a short string per the moveset design) is not this module's concern; a
    // caller-supplied numeric handle is all a hit event needs to report which move landed.
    struct Hitbox
    {
        CollisionBox box;
        uint32_t moveId = 0;
    };

    // Defense volume(s) - normally present on every entity that can be hit, for its whole
    // lifetime (unlike Hitbox). A list, not a single box: one entity's hurtbox is often more than
    // one box (e.g. torso + legs) even before per-state profiles (Standing/Crouching/Airborne)
    // exist to swap which boxes are active.
    struct Hurtbox
    {
        std::vector<CollisionBox> boxes;
    };

    // Non-damaging volume (stage bounds, corner detection, ...) - tested the same way as a
    // hitbox/hurtbox pair, but against Hurtbox (an entity's body) rather than another Hitbox, and
    // not gated to any move's active frames. triggerId is expected to be unique per trigger
    // volume entity (one authoring convention, same spirit as moveId identifying a move).
    struct TriggerVolume
    {
        CollisionBox box;
        uint32_t triggerId = 0;
    };

    struct HitEvent
    {
        entt::entity attacker = entt::null;
        entt::entity defender = entt::null;
        uint32_t moveId = 0;
    };

    struct TriggerEvent
    {
        entt::entity entity = entt::null;
        uint32_t triggerId = 0;
    };

    struct CollisionEvents
    {
        std::vector<HitEvent> hits;
        std::vector<TriggerEvent> triggers;
    };

    // World-space AABB of one CollisionBox, given the owning entity's Transform. Exposed
    // separately from ResolveCollisions so callers (debug draw) can compute the exact box the
    // resolver tested without duplicating the offset/scale math.
    void ComputeWorldAabb(const CollisionBox& box, const Transform& transform, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax);

    // Pure function: reads every Hitbox/Hurtbox/TriggerVolume currently in the registry (already
    // reflecting whichever are active this tick - Game's job, not this function's, to add/remove
    // them as moves/states change), tests every hitbox against every OTHER entity's hurtbox (an
    // entity's own hitbox is never tested against its own hurtbox - overlap is always
    // asymmetric) and every trigger volume against every entity's hurtbox, and returns what
    // overlapped this call. Does not decide what an overlap means -
    // reporting overlaps as events and nothing else is the entire job; the caller decides what
    // each event means (damage, hitstun, a trigger's effect). No randomness, no render-rate
    // dependence - safe to call once per fixed tick.
    CollisionEvents ResolveCollisions(entt::registry& registry);

    // Builds a colored wireframe line list for every Hitbox/Hurtbox/TriggerVolume box currently
    // in the registry, ready to hand to Renderer::DebugDrawPass::SetLines - green by default, red
    // for any box whose owning entity appears in events (an attacker/defender pair from a hit, or
    // an entity/trigger from a trigger event). Colors an entity's whole Hurtbox red on any hit
    // to it, not just the specific box that overlapped - a deliberate simplification, since
    // ResolveCollisions itself only records "this entity was hit," not which of its boxes.
    // Lives in Engine (not Game) since deciding "does this box belong to an event" only needs
    // the same registry/event data ResolveCollisions already works with.
    std::vector<Renderer::DebugDrawLine> BuildCollisionDebugLines(entt::registry& registry, const CollisionEvents& events);
}
