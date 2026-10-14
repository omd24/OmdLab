#pragma once

#if defined(OMD_PROTOTYPE)

#include "Engine/Camera.h"
#include "Renderer/StaticMeshDrawItem.h"

#include <vector>

namespace Game
{
    // TODO(OM): replace with a proper boot-screen sequence in the UI renderer (timed pages,
    // skippable, localisable text) instead of a black quad plus ImGui text.

    // The opening of the demo launch: two text pages (a rhyme, then a long disclaimer) on black,
    // which then fade out into the main menu. A black quad in front of the camera hides the scene.
    struct Splash
    {
        std::vector<Renderer::StaticMeshDrawItem> items; // The black cover.
        bool skipWas = true;                             // Previous-frame state of the skip keys.

        // Seconds since the start.
        static constexpr float kDuration = 17.3f;
    };

    Splash CreateSplash();

    // Places and fades the cover for time `t`; `active` false hides it.
    void UpdateSplash(Splash& splash, const Engine::Camera& camera, float aspectRatio, float t, bool active);

    // Draws the current page onto ImGui's background draw list. Enter, Space or a click moves on
    // to the next page's fade-out (ignored while `focused` is false). Returns the time to
    // continue from (t itself unless a skip happened).
    float DrawSplashText(Splash& splash, float t, bool focused);
}

#endif // OMD_PROTOTYPE
