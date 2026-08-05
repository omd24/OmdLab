#pragma once

#include "Renderer/PlatformMacros.h"

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

#include <DirectXMath.h>

namespace Engine
{
    // Position/orientation/FOV, owned by Engine per the camera ownership convention -
    // Renderer never sees this type, only the view/projection matrix ComputeViewProjection()
    // produces, threaded into a pass's RenderDesc the same way draw items are. The fighting
    // game's locked 2D-follow mode isn't built yet (no fighter entities exist to follow) -
    // this currently only drives the free-fly debug mode that every earlier Renderer milestone
    // (from the triangle onward) has used directly from Game/main.cpp.
    struct Camera
    {
        DirectX::XMFLOAT3 position = { 0.0f, 1.5f, -4.0f };
        float yaw = 0.0f;   // Radians; 0 looks down +Z, rotates around world +Y.
        float pitch = 0.0f; // Radians; positive looks up.

        float fovYRadians = DirectX::XMConvertToRadians(60.0f);
        float nearPlane = 0.1f;
        float farPlane = 500.0f;
    };

    // Free-fly debug movement from held keyboard state: W/S forward/back, A/D strafe, Q/E
    // up/down (all relative to the current yaw/pitch), Left/Right arrow yaw around world +Y,
    // Up/Down arrow pitch. Gated to when the given window is focused - GetAsyncKeyState reads
    // global keyboard state regardless of which window has focus, so without this gate held
    // movement keys would keep driving the camera even while e.g. alt-tabbed away.
    //
    // Also supports mouse navigation, mimicking the same keyboard axes above rather than a
    // full DCC-style orbit/pan/zoom rig: left-button drag rotates (mimics the arrow keys),
    // right-button drag pans (mimics A/D/Q/E, "grab and drag the world" direction - dragging
    // right moves the view as if pulling the scene right, i.e. the camera moves left),
    // middle-button drag zooms vertically (mimics W/S). All three read via GetAsyncKeyState
    // polling, same as the keyboard above - deliberately not the mouse wheel, which only
    // arrives via WM_MOUSEWHEEL window messages requiring true keyboard focus (confirmed via
    // logging that it doesn't reliably reach this app, unlike focus-independent button-state
    // polling). Deliberately the simple mapping, not a from-scratch camera rig.
    void UpdateFreeFlyCamera(Camera& camera, HWND window, float deltaSeconds);

    // Combined view * projection matrix, transposed for the row_major cbuffer convention every
    // shader in this project declares (see Triangle.hlsl/LitTextured.hlsl).
    DirectX::XMFLOAT4X4 ComputeViewProjection(const Camera& camera, float aspectRatio);
}
