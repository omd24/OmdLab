#pragma once

#if defined(OMD_PROTOTYPE)

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace Game
{
    // TODO(OM): replace with a proper feedback system (per-move hit-flash/shake data, a camera
    // rig that owns shake and zoom, screen effects, controller rumble).

    // Presentation-only reaction to a hit: a brief tint flash on the fighter that was hit and a
    // decaying camera shake. Nothing here feeds back into the simulation.
    struct JuiceState
    {
        static constexpr float kFlashSeconds = 0.14f;

        // Per fighter (0 = first, 1 = second): remaining flash time and whether it was a block.
        float flashSeconds[2] = { 0.0f, 0.0f };
        bool flashBlocked[2] = { false, false };

        float shakeAmplitude = 0.0f; // World units at the camera.
        float shakeClock = 0.0f;

        void OnHit(int defenderSide, int damage, bool blocked, bool knockout)
        {
            flashSeconds[defenderSide] = kFlashSeconds * (knockout ? 1.8f : 1.0f);
            flashBlocked[defenderSide] = blocked;
            float amplitude = blocked ? 0.015f : 0.035f + 0.0035f * static_cast<float>((std::min)(damage, 30));
            if (knockout)
            {
                amplitude = 0.22f;
            }
            shakeAmplitude = (std::max)(shakeAmplitude, amplitude);
        }

        // Once per render frame, with real elapsed seconds.
        void Update(float deltaSeconds)
        {
            for (float& seconds : flashSeconds)
            {
                seconds = (std::max)(0.0f, seconds - deltaSeconds);
            }
            shakeClock += deltaSeconds;
            shakeAmplitude *= std::exp(-deltaSeconds * 11.0f);
            if (shakeAmplitude < 0.0005f)
            {
                shakeAmplitude = 0.0f;
            }
        }

        // The fighter's base tint moved toward a flash colour: white-hot for a hit, a cool
        // blue-white for a block.
        DirectX::XMFLOAT4 TintFor(int side, const DirectX::XMFLOAT4& base) const
        {
            if (flashSeconds[side] <= 0.0f)
            {
                return base;
            }
            const float t = (std::min)(flashSeconds[side] / kFlashSeconds, 1.0f);
            const DirectX::XMFLOAT3 flash = flashBlocked[side] ? DirectX::XMFLOAT3{ 1.0f, 1.5f, 2.2f } : DirectX::XMFLOAT3{ 2.6f, 2.4f, 2.2f };
            return { base.x + (flash.x - base.x) * t, base.y + (flash.y - base.y) * t, base.z + (flash.z - base.z) * t, base.w };
        }

        // Camera displacement (X, Y) for this frame.
        DirectX::XMFLOAT2 ShakeOffset() const
        {
            if (shakeAmplitude <= 0.0f)
            {
                return { 0.0f, 0.0f };
            }
            return { shakeAmplitude * std::sin(shakeClock * 83.0f), shakeAmplitude * 0.7f * std::cos(shakeClock * 67.0f) };
        }
    };
}

#endif // OMD_PROTOTYPE
