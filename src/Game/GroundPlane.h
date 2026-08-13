#pragma once

#include "Renderer/StaticMeshDrawItem.h"

namespace Game
{
    // Builds the one procedural, checkerboard-textured quad fighters stand/fight on - no glTF
    // asset needed, matching this game's deliberately flat, single-stage scope (no terrain).
    // Sized from kStageHalfWidth/kStageHalfDepth (GameConstants.h).
    Renderer::StaticMeshDrawItem CreateGroundPlaneDrawItem();
}
