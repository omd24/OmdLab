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
        std::vector<Renderer::StaticMeshDrawItem> items;     // One quad per tile.
        std::vector<Renderer::StaticMeshDrawItem> menuItems; // The same tiles with the dark, torch-lit picture.
        std::vector<Renderer::StaticMeshDrawItem> capItems;  // Plain wall colour above the picture (one wide quad).
        DirectX::XMFLOAT3 capColor = { 0.2f, 0.2f, 0.22f };
        bool loaded = false;
        bool visible = true;
        bool menuShown = false;
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

    // Switches between the normal picture and the dark one used behind the main menu (only has
    // an effect while the backdrop is visible).
    void SetBackdropMenu(Backdrop& backdrop, bool menu);

    // World-space centre and distance for a camera that frames the skull pile in the dark picture.
    DirectX::XMFLOAT3 BackdropMenuFocus(const Backdrop& backdrop);

    // Once per render frame: flames, smoke and glow on the torches that are in view.
    void UpdateBackdropFire(Backdrop& backdrop, VfxSystem& vfx, float cameraX, float deltaSeconds);
}

#endif // OMD_PROTOTYPE
