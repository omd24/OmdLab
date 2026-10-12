#pragma once

#if defined(OMD_PROTOTYPE)

#include <DirectXMath.h>
#include <cstdint>
#include <vector>

namespace Engine
{
    // TODO(OM): replace with a GPU-driven particle pass (additive blending, data-driven emitter
    // definitions, per-move effect blocks, depth-sorted or soft particles).

    // One CPU particle. Size and colour interpolate linearly over normalised age; everything
    // else is plain integration.
    struct Particle
    {
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 velocity = { 0.0f, 0.0f, 0.0f };
        float size0 = 0.1f;
        float size1 = 0.1f;
        float stretch = 1.0f; // Length/width ratio along the screen-space velocity (1 = round).
        float gravity = 0.0f; // Downward acceleration, world units per second squared.
        float drag = 0.0f;    // Exponential velocity decay per second.
        DirectX::XMFLOAT4 color0 = { 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 color1 = { 1.0f, 1.0f, 1.0f, 0.0f };
        float age = 0.0f;
        float lifetime = 0.0f; // 0 = slot is free.
    };

    // Parameters of a radial burst. Directions are picked between `direction` (spread 0) and a
    // uniformly random direction (spread 1).
    struct ParticleBurst
    {
        int count = 8;
        DirectX::XMFLOAT3 direction = { 0.0f, 1.0f, 0.0f };
        float spread = 1.0f;
        float speedMin = 1.0f;
        float speedMax = 2.0f;
        float lifetimeMin = 0.2f;
        float lifetimeMax = 0.4f;
        float size0 = 0.1f;
        float size1 = 0.02f;
        float stretch = 1.0f;
        float gravity = 0.0f;
        float drag = 0.0f;
        DirectX::XMFLOAT4 color0 = { 1.0f, 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT4 color1 = { 1.0f, 1.0f, 1.0f, 0.0f };
    };

    // Fixed pool of particles, pure CPU, no renderer types. A full pool overwrites its oldest
    // cursor slot rather than refusing a spawn.
    class ParticleSystem
    {
    public:
        explicit ParticleSystem(size_t capacity);

        // `count` particles from `position`.
        void Burst(const ParticleBurst& desc, const DirectX::XMFLOAT3& position);

        // `count` particles spread evenly (with jitter) along the segment a -> b: a trail
        // laid down by something that moved this frame.
        void EmitAlong(const ParticleBurst& desc, const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b);

        void Update(float deltaSeconds);
        void Clear();

        const std::vector<Particle>& Particles() const { return m_particles; }

    private:
        void Spawn(const ParticleBurst& desc, const DirectX::XMFLOAT3& position);
        float Random01();

        std::vector<Particle> m_particles;
        size_t m_cursor = 0;
        uint32_t m_rngState = 0x9E3779B9u;
    };
}

#endif // OMD_PROTOTYPE
