#include "Collision.h"

namespace
{
    bool AabbOverlap(
        const DirectX::XMFLOAT3& aMin, const DirectX::XMFLOAT3& aMax, const DirectX::XMFLOAT3& bMin, const DirectX::XMFLOAT3& bMax)
    {
        return aMin.x <= bMax.x && aMax.x >= bMin.x && aMin.y <= bMax.y && aMax.y >= bMin.y && aMin.z <= bMax.z && aMax.z >= bMin.z;
    }

    // Decomposes one AABB into its 12 wireframe edges and appends them to lines, all colored the
    // same solid color - the one place DebugDrawLine's generic "just a segment" shape gets used
    // to actually draw a box.
    void AppendAabbWireframe(
        std::vector<Renderer::DebugDrawLine>& lines, const DirectX::XMFLOAT3& min, const DirectX::XMFLOAT3& max,
        const DirectX::XMFLOAT3& color)
    {
        const DirectX::XMFLOAT3 corners[8] = {
            { min.x, min.y, min.z }, { max.x, min.y, min.z }, { max.x, max.y, min.z }, { min.x, max.y, min.z },
            { min.x, min.y, max.z }, { max.x, min.y, max.z }, { max.x, max.y, max.z }, { min.x, max.y, max.z },
        };
        constexpr int kEdges[12][2] = {
            { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        };
        for (const auto& edge : kEdges)
        {
            lines.push_back({ corners[edge[0]], corners[edge[1]], color });
        }
    }
}

namespace Engine
{
    void ComputeWorldAabb(const CollisionBox& box, const Transform& transform, DirectX::XMFLOAT3& outMin, DirectX::XMFLOAT3& outMax)
    {
        const float scale = transform.scale;
        const DirectX::XMFLOAT3 center = {
            transform.position.x + box.offset.x * scale,
            transform.position.y + box.offset.y * scale,
            transform.position.z + box.offset.z * scale,
        };
        const DirectX::XMFLOAT3 half = { box.halfExtents.x * scale, box.halfExtents.y * scale, box.halfExtents.z * scale };
        outMin = { center.x - half.x, center.y - half.y, center.z - half.z };
        outMax = { center.x + half.x, center.y + half.y, center.z + half.z };
    }

    CollisionEvents ResolveCollisions(entt::registry& registry)
    {
        CollisionEvents events;

        const auto hitboxView = registry.view<Hitbox, Transform>();
        const auto hurtboxView = registry.view<Hurtbox, Transform>();

        for (const entt::entity attacker : hitboxView)
        {
            const Hitbox& hitbox = hitboxView.get<Hitbox>(attacker);
            const Transform& attackerTransform = hitboxView.get<Transform>(attacker);
            DirectX::XMFLOAT3 hitMin, hitMax;
            ComputeWorldAabb(hitbox.box, attackerTransform, hitMin, hitMax);

            for (const entt::entity defender : hurtboxView)
            {
                if (defender == attacker)
                {
                    continue;
                }
                const Hurtbox& hurtbox = hurtboxView.get<Hurtbox>(defender);
                const Transform& defenderTransform = hurtboxView.get<Transform>(defender);
                for (const CollisionBox& hurtboxBox : hurtbox.boxes)
                {
                    DirectX::XMFLOAT3 hurtMin, hurtMax;
                    ComputeWorldAabb(hurtboxBox, defenderTransform, hurtMin, hurtMax);
                    if (AabbOverlap(hitMin, hitMax, hurtMin, hurtMax))
                    {
                        events.hits.push_back({ attacker, defender, hitbox.moveId });
                        break;
                    }
                }
            }
        }

        const auto triggerView = registry.view<TriggerVolume, Transform>();
        for (const entt::entity triggerEntity : triggerView)
        {
            const TriggerVolume& trigger = triggerView.get<TriggerVolume>(triggerEntity);
            const Transform& triggerTransform = triggerView.get<Transform>(triggerEntity);
            DirectX::XMFLOAT3 trigMin, trigMax;
            ComputeWorldAabb(trigger.box, triggerTransform, trigMin, trigMax);

            for (const entt::entity body : hurtboxView)
            {
                if (body == triggerEntity)
                {
                    continue;
                }
                const Hurtbox& hurtbox = hurtboxView.get<Hurtbox>(body);
                const Transform& bodyTransform = hurtboxView.get<Transform>(body);
                for (const CollisionBox& hurtboxBox : hurtbox.boxes)
                {
                    DirectX::XMFLOAT3 hurtMin, hurtMax;
                    ComputeWorldAabb(hurtboxBox, bodyTransform, hurtMin, hurtMax);
                    if (AabbOverlap(trigMin, trigMax, hurtMin, hurtMax))
                    {
                        events.triggers.push_back({ body, trigger.triggerId });
                        break;
                    }
                }
            }
        }

        return events;
    }

    std::vector<Renderer::DebugDrawLine> BuildCollisionDebugLines(entt::registry& registry, const CollisionEvents& events)
    {
        std::vector<Renderer::DebugDrawLine> lines;

        constexpr DirectX::XMFLOAT3 kGreen = { 0.0f, 1.0f, 0.0f };
        constexpr DirectX::XMFLOAT3 kRed = { 1.0f, 0.0f, 0.0f };

        const auto isHitAttacker = [&events](entt::entity entity)
        {
            for (const HitEvent& hit : events.hits)
            {
                if (hit.attacker == entity)
                {
                    return true;
                }
            }
            return false;
        };
        const auto isHitDefender = [&events](entt::entity entity)
        {
            for (const HitEvent& hit : events.hits)
            {
                if (hit.defender == entity)
                {
                    return true;
                }
            }
            return false;
        };
        const auto isTriggeredBody = [&events](entt::entity entity)
        {
            for (const TriggerEvent& trigger : events.triggers)
            {
                if (trigger.entity == entity)
                {
                    return true;
                }
            }
            return false;
        };

        for (const entt::entity entity : registry.view<Hitbox, Transform>())
        {
            const Hitbox& hitbox = registry.get<Hitbox>(entity);
            const Transform& transform = registry.get<Transform>(entity);
            DirectX::XMFLOAT3 min, max;
            ComputeWorldAabb(hitbox.box, transform, min, max);
            AppendAabbWireframe(lines, min, max, isHitAttacker(entity) ? kRed : kGreen);
        }

        for (const entt::entity entity : registry.view<Hurtbox, Transform>())
        {
            const Hurtbox& hurtbox = registry.get<Hurtbox>(entity);
            const Transform& transform = registry.get<Transform>(entity);
            const DirectX::XMFLOAT3& color = (isHitDefender(entity) || isTriggeredBody(entity)) ? kRed : kGreen;
            for (const CollisionBox& box : hurtbox.boxes)
            {
                DirectX::XMFLOAT3 min, max;
                ComputeWorldAabb(box, transform, min, max);
                AppendAabbWireframe(lines, min, max, color);
            }
        }

        for (const entt::entity entity : registry.view<TriggerVolume, Transform>())
        {
            const TriggerVolume& trigger = registry.get<TriggerVolume>(entity);
            const Transform& transform = registry.get<Transform>(entity);
            bool triggered = false;
            for (const TriggerEvent& triggerEvent : events.triggers)
            {
                if (triggerEvent.triggerId == trigger.triggerId)
                {
                    triggered = true;
                    break;
                }
            }
            DirectX::XMFLOAT3 min, max;
            ComputeWorldAabb(trigger.box, transform, min, max);
            AppendAabbWireframe(lines, min, max, triggered ? kRed : kGreen);
        }

        return lines;
    }
}
