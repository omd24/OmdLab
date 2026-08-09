#pragma once

#include <DirectXMath.h>

namespace Renderer
{
    // One colored line segment in world space - the one primitive DebugDrawPass knows how to
    // draw. Generic on purpose (not "box"-shaped): any wireframe shape (a box today, a capsule
    // or path later) decomposes into a list of these, so DebugDrawPass itself never needs to
    // know what it's drawing a wireframe of.
    struct DebugDrawLine
    {
        DirectX::XMFLOAT3 start = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 end = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f }; // RGB, 0-1, unlit.
    };
}
