#pragma once

#if defined(OMD_PROTOTYPE)

#include "Renderer/StaticMeshDrawItem.h"

#include <DirectXMath.h>
#include <string>
#include <vector>

namespace Game
{
    struct VfxSystem;

    // TODO(OM): replace with authored stage art (layered parallax, real props, lit torches) and
    // a data-driven stage definition.

    // A picture standing at the back edge of the floor, tiled sideways (every other tile
    // mirrored, so the seams match) to cover however far the camera pans. Also owns the torch
    // fire drawn on the picture's two wall brackets.
    struct Backdrop
    {
        std::vector<Renderer::StaticMeshDrawItem> items; // One quad per tile.
        bool loaded = false;
        bool visible = true;
        float tileWidth = 0.0f;
        float tileHeight = 0.0f;
        float baseY = 0.0f;
        float z = 0.0f;
        int tileCount = 0;
        std::vector<float> torchAccumulator; // Per tile and torch: fractional particles owed.
        float clock = 0.0f;
    };

    // Loads the picture; `loaded` stays false (and nothing is drawn) when the file is missing.
    Backdrop CreateBackdrop(const std::string& imagePath);

    // Shows or hides the whole backdrop (the tiles are collapsed to nothing when hidden).
    void SetBackdropVisible(Backdrop& backdrop, bool visible);

    // Once per render frame: flames, smoke and glow on the torches that are in view.
    void UpdateBackdropFire(Backdrop& backdrop, VfxSystem& vfx, float cameraX, float deltaSeconds);
}

#endif // OMD_PROTOTYPE
