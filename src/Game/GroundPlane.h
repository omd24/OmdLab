#pragma once

#include "Renderer/StaticMeshDrawItem.h"

#include <vector>

namespace Game
{
    // Height of the visible floor surface. 0 for the plain stage; the prototype floor sits
    // slightly above the characters' foot tips so they stand in it rather than on it.
#if defined(OMD_PROTOTYPE)
    constexpr float kFloorSurfaceY = 0.07f;
#else
    constexpr float kFloorSurfaceY = 0.0f;
#endif

#if !defined(OMD_PROTOTYPE)
    // Builds the one procedural, checkerboard-textured quad fighters stand/fight on - no glTF
    // asset needed, matching this game's deliberately flat, single-stage scope (no terrain).
    // Sized from kStageHalfWidth/kStageHalfDepth (GameConstants.h).
    Renderer::StaticMeshDrawItem CreateGroundPlaneDrawItem();
#endif

#if defined(OMD_PROTOTYPE)
    // The prototype stage: a scrolling green mud floor that continues far past the playable
    // area (a red-tinted variant beyond the movement limits) plus a translucent dashed marker
    // standing on each limit.
    // TODO(OM): replace with a real stage (authored floor, boundary treatment, backdrop).
    struct StageDrawItems
    {
        std::vector<Renderer::StaticMeshDrawItem> floor; // Left out-of-bounds, playable, right out-of-bounds.
        std::vector<Renderer::StaticMeshDrawItem> walls; // Transparent; draw after the floor.
    };

    StageDrawItems CreateStageDrawItems();

    // Scrolls the floor texture: call once per render frame with a monotonically increasing
    // time. Rewrites each floor quad's UVs (the sampler wraps).
    void UpdateStageScroll(const StageDrawItems& stage, float timeSeconds);
#endif
}
