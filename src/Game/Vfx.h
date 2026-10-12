#pragma once

#if defined(OMD_PROTOTYPE)

#include "Engine/Camera.h"
#include "Engine/Particles.h"
#include "Renderer/StaticMeshDrawItem.h"

#include <DirectXMath.h>
#include <cstdint>
#include <entt.hpp>
#include <vector>

namespace Game
{
    // TODO(OM): replace with data-driven effect definitions (per-move vfx blocks, authored
    // emitters) triggered from move frames and hit data rather than hard-coded recipes.

    enum class VfxType
    {
        Hit,            // Contact sparks, flash and dust. intensity 0..1.
        Block,          // Cooler, smaller sparks.
        Knockout,       // Finishing blow: everything bigger.
        Landing,        // Dust either side of the feet.
        JumpOff,        // Small dust at the feet.
        FireballImpact, // Fireball connecting.
        FireballClash,  // Two fireballs cancelling.
        BloodPool,      // A pool that grows under a knocked-out fighter. `subject` is that fighter.
    };

    // What happened and where; the recipes live in Vfx.cpp. direction.x is the attack's
    // horizontal direction (+1 / -1) for contact effects.
    struct VfxEvent
    {
        VfxType type = VfxType::Hit;
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 direction = { 1.0f, 0.0f, 0.0f };
        float intensity = 0.5f;
        entt::entity subject = entt::null;
        uint32_t landedFrame = 0; // BloodPool: the subject's KO frame at which the body is on the floor.
    };

    // One spreading pool on the floor. Waits until `subject` has actually landed (its KO state
    // has run for `landedFrame` simulation ticks, so slow motion and hitstop are respected),
    // anchors under the body, then grows with an ease-out.
    struct BloodPool
    {
        bool used = false;
        bool placed = false;
        bool seenDown = false; // The subject has entered its KO state (it can lag the knockout by a hit-stun).
        entt::entity subject = entt::null;
        float fallDirection = 1.0f; // Which way the fighter fell (+1 / -1 along X).
        uint32_t landedFrame = 0;
        float waited = 0.0f; // Real seconds spent waiting, for the give-up timeout.
        float age = 0.0f;     // Real seconds since landing.
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    };

    // Particle simulation plus the pool of camera-facing quads that draws it, reusing the
    // static-mesh pass's alpha-blended path (no dedicated pass or shader).
    struct VfxSystem
    {
        explicit VfxSystem(size_t capacity) : particles(capacity) {}

        Engine::ParticleSystem particles;
        std::vector<Renderer::StaticMeshDrawItem> drawItems; // One per particle slot.
        std::vector<char> visible;                           // Whether a slot was drawn last frame.
        std::vector<VfxEvent> queue;
        std::vector<Renderer::StaticMeshDrawItem> poolItems; // Flat blobs, drawn before the particles.
        std::vector<BloodPool> pools;
        size_t nextPool = 0;
        // Last render-frame position per projectile slot, for laying a trail between frames.
        std::vector<DirectX::XMFLOAT3> projectileLast;
        std::vector<char> projectileLastValid;
    };

    VfxSystem CreateVfxSystem(size_t capacity);

    void PushVfxEvent(VfxSystem& system, const VfxEvent& event);

    // World-space point where `attacker`'s hit met `defender`: the centre of the overlap of the
    // attacker's hitbox with the defender's hurtbox where both still exist, else chest height
    // on the defender's side facing the attacker.
    DirectX::XMFLOAT3 ComputeContactPoint(entt::registry& registry, entt::entity attacker, entt::entity defender);

    // Once per render frame: turns queued events into particles, lays fireball trails and glow,
    // and advances the simulation.
    void UpdateVfx(entt::registry& registry, VfxSystem& system, const Engine::Camera& camera, float deltaSeconds);

    // Once per render frame, after UpdateVfx: positions/tints the quads for the given camera.
    void UpdateVfxRender(VfxSystem& system, const Engine::Camera& camera);

    // Places and grows the blood pools; call once per render frame.
    void UpdateVfxPools(entt::registry& registry, VfxSystem& system, float deltaSeconds);

    // Drops every live particle and pending event (round reset).
    void ClearVfx(VfxSystem& system);
}

#endif // OMD_PROTOTYPE
