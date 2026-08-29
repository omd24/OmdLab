#include "UIPassDX12.h"

namespace Renderer
{
    // TODO(OM): real screen-space UI content (health bars, HUD) - orthographic projection,
    // pixel-vs-NDC convention, and a quad/text root signature shape are all still undecided;
    // see UIPass.h's own comment. This is purely the orchestration seam for now - no PSO,
    // shader, or root signature stood up, since inventing a vertex layout/topology with no
    // real vertex data would be speculative surface area the eventual real pass will likely
    // redesign anyway.
    void UIPassDX12::Init() {}
    void UIPassDX12::Shutdown() {}
    void UIPassDX12::Render() {}
}
