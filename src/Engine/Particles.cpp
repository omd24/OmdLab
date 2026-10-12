#if defined(OMD_PROTOTYPE)

#include "Particles.h"

#include <cmath>

namespace Engine
{
    ParticleSystem::ParticleSystem(size_t capacity) : m_particles(capacity) {}

    float ParticleSystem::Random01()
    {
        // xorshift32 - presentation-only randomness, deliberately independent of the simulation.
        m_rngState ^= m_rngState << 13;
        m_rngState ^= m_rngState >> 17;
        m_rngState ^= m_rngState << 5;
        return static_cast<float>(m_rngState & 0xFFFFFFu) / static_cast<float>(0x1000000u);
    }

    void ParticleSystem::Spawn(const ParticleBurst& desc, const DirectX::XMFLOAT3& position)
    {
        if (m_particles.empty())
        {
            return;
        }

        // Uniform random unit vector, blended toward the requested direction.
        const float z = Random01() * 2.0f - 1.0f;
        const float phi = Random01() * DirectX::XM_2PI;
        const float ring = std::sqrt(std::fmax(0.0f, 1.0f - z * z));
        DirectX::XMFLOAT3 random = { ring * std::cos(phi), ring * std::sin(phi), z };
        DirectX::XMFLOAT3 dir = {
            desc.direction.x + (random.x - desc.direction.x) * desc.spread,
            desc.direction.y + (random.y - desc.direction.y) * desc.spread,
            desc.direction.z + (random.z - desc.direction.z) * desc.spread,
        };
        const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (length > 1e-5f)
        {
            dir = { dir.x / length, dir.y / length, dir.z / length };
        }
        const float speed = desc.speedMin + (desc.speedMax - desc.speedMin) * Random01();

        Particle& particle = m_particles[m_cursor];
        m_cursor = (m_cursor + 1) % m_particles.size();
        particle.position = position;
        particle.velocity = { dir.x * speed, dir.y * speed, dir.z * speed };
        particle.size0 = desc.size0;
        particle.size1 = desc.size1;
        particle.stretch = desc.stretch;
        particle.gravity = desc.gravity;
        particle.drag = desc.drag;
        particle.color0 = desc.color0;
        particle.color1 = desc.color1;
        particle.age = 0.0f;
        particle.lifetime = desc.lifetimeMin + (desc.lifetimeMax - desc.lifetimeMin) * Random01();
        if (particle.lifetime <= 0.0f)
        {
            particle.lifetime = 0.001f;
        }
    }

    void ParticleSystem::Burst(const ParticleBurst& desc, const DirectX::XMFLOAT3& position)
    {
        for (int i = 0; i < desc.count; ++i)
        {
            Spawn(desc, position);
        }
    }

    void ParticleSystem::EmitAlong(const ParticleBurst& desc, const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
    {
        for (int i = 0; i < desc.count; ++i)
        {
            const float t = (static_cast<float>(i) + Random01()) / static_cast<float>(desc.count);
            Spawn(desc, { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t });
        }
    }

    void ParticleSystem::Update(float deltaSeconds)
    {
        for (Particle& particle : m_particles)
        {
            if (particle.lifetime <= 0.0f)
            {
                continue;
            }
            particle.age += deltaSeconds;
            if (particle.age >= particle.lifetime)
            {
                particle.lifetime = 0.0f;
                continue;
            }
            const float damping = std::exp(-particle.drag * deltaSeconds);
            particle.velocity.x *= damping;
            particle.velocity.y = particle.velocity.y * damping - particle.gravity * deltaSeconds;
            particle.velocity.z *= damping;
            particle.position.x += particle.velocity.x * deltaSeconds;
            particle.position.y += particle.velocity.y * deltaSeconds;
            particle.position.z += particle.velocity.z * deltaSeconds;
        }
    }

    void ParticleSystem::Clear()
    {
        for (Particle& particle : m_particles)
        {
            particle.lifetime = 0.0f;
        }
    }
}

#endif // OMD_PROTOTYPE
