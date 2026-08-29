#pragma once

#include "Renderer/StaticMeshDrawItem.h"

namespace Game
{
    // Builds one small, flat disc (a round shape suits this character's own primitive-built,
    // roughly-cylindrical silhouette better than a rectangle) with a soft radial-gradient
    // alpha (opaque-ish center fading to fully transparent at the rim) - a cheap "grounding"
    // cue under a fighter's feet, not a real projected shadow. Uses the renderer's alpha-
    // blending support (StaticMeshDrawItem::transparent, LitTextured.hlsl's TintConstants) -
    // both the soft edge (baked into the texture's own alpha channel) and the height-based
    // fade (UpdateFighterShadowPosition's own tintBuffer update) ride on this. Created once
    // per fighter at startup, same pattern as GroundPlane.h's own quad;
    // UpdateFighterShadowPosition repositions/refades it every render frame.
    Renderer::StaticMeshDrawItem CreateFighterShadowDrawItem();

    // Moves an already-created shadow disc to sit under (worldX, worldZ) - Y is fixed just
    // above the ground plane's own Y=0 quad (a small epsilon, to avoid z-fighting between two
    // flat shapes that would otherwise sit at the exact same height); the shadow's own
    // position does not rise to meet the fighter. worldY (the fighter's own current height,
    // not the shadow's) drives how much the shadow additionally fades via its tintBuffer -
    // see GameConstants.h's kShadowFadeMaxHeight/kShadowMinAlpha.
    void UpdateFighterShadowPosition(Renderer::StaticMeshDrawItem& shadowItem, float worldX, float worldY, float worldZ);
}
