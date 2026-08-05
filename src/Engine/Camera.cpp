#include "Camera.h"

#include <cmath>

namespace
{
    bool KeyDown(int virtualKey)
    {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    DirectX::XMVECTOR ComputeForward(float yaw, float pitch)
    {
        return DirectX::XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch), 0.0f);
    }
}

namespace Engine
{
    void UpdateFreeFlyCamera(Camera& camera, HWND window, float deltaSeconds)
    {
        if (GetForegroundWindow() != window)
        {
            return;
        }

        constexpr float moveUnitsPerSecond = 3.0f;
        constexpr float turnRadiansPerSecond = 1.5f;

        if (KeyDown(VK_LEFT))
        {
            camera.yaw -= turnRadiansPerSecond * deltaSeconds;
        }
        if (KeyDown(VK_RIGHT))
        {
            camera.yaw += turnRadiansPerSecond * deltaSeconds;
        }
        if (KeyDown(VK_UP))
        {
            camera.pitch += turnRadiansPerSecond * deltaSeconds;
        }
        if (KeyDown(VK_DOWN))
        {
            camera.pitch -= turnRadiansPerSecond * deltaSeconds;
        }
        // Clamped just short of straight up/down - forward and world-up become parallel there,
        // which degenerates XMMatrixLookToLH's internally-derived basis.
        constexpr float kMaxPitch = DirectX::XM_PIDIV2 - 0.01f;
        camera.pitch = camera.pitch < -kMaxPitch ? -kMaxPitch : (camera.pitch > kMaxPitch ? kMaxPitch : camera.pitch);

        const DirectX::XMVECTOR forward = ComputeForward(camera.yaw, camera.pitch);
        const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const DirectX::XMVECTOR right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, forward));

        const float moveStep = moveUnitsPerSecond * deltaSeconds;
        DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&camera.position);
        if (KeyDown('W'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(forward, moveStep));
        }
        if (KeyDown('S'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(forward, moveStep));
        }
        if (KeyDown('D'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(right, moveStep));
        }
        if (KeyDown('A'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(right, moveStep));
        }
        if (KeyDown('Q'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(up, moveStep));
        }
        if (KeyDown('E'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(up, moveStep));
        }
        DirectX::XMStoreFloat3(&camera.position, eye);

        // Mouse-drag navigation, mimicking the keyboard axes above rather than a full
        // DCC-style orbit/pan/zoom rig - see the header comment. lastCursor/wasDragging are
        // static rather than fields on Camera itself, since they're input-tracking state, not
        // camera state - fine as long as only one camera ever calls this, which is the only
        // case that exists today.
        static POINT lastCursor = { 0, 0 };
        static bool wasDragging = false;

        POINT cursor;
        GetCursorPos(&cursor);
        const bool leftDown = KeyDown(VK_LBUTTON);
        const bool rightDown = KeyDown(VK_RBUTTON);
        const bool middleDown = KeyDown(VK_MBUTTON);
        const bool dragging = leftDown || rightDown || middleDown;

        // Only apply a delta once a full frame has already seen the button held - the first
        // frame of a drag has no meaningful "last position" yet, and applying one would jump
        // the camera by however far the cursor happened to be from wherever it last was.
        if (dragging && wasDragging)
        {
            const float deltaX = static_cast<float>(cursor.x - lastCursor.x);
            const float deltaY = static_cast<float>(cursor.y - lastCursor.y);

            constexpr float mouseTurnRadiansPerPixel = 0.005f;
            constexpr float mousePanUnitsPerPixel = 0.01f;

            if (leftDown)
            {
                camera.yaw += deltaX * mouseTurnRadiansPerPixel;
                camera.pitch -= deltaY * mouseTurnRadiansPerPixel;
                camera.pitch = camera.pitch < -kMaxPitch ? -kMaxPitch : (camera.pitch > kMaxPitch ? kMaxPitch : camera.pitch);
            }

            if (rightDown)
            {
                // Recomputed rather than reusing the keyboard section's forward/right above -
                // those predate any rotation just applied this frame by a left-button drag.
                const DirectX::XMVECTOR mouseForward = ComputeForward(camera.yaw, camera.pitch);
                const DirectX::XMVECTOR mouseRight = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up, mouseForward));
                DirectX::XMVECTOR mouseEye = DirectX::XMLoadFloat3(&camera.position);

                // Inverted relative to a naive "camera moves toward the drag" mapping - this is
                // the "grab and drag the world" feel most viewport pan controls use: dragging
                // right should make the scene appear to follow the cursor to the right, which
                // means the camera itself moves left.
                mouseEye = DirectX::XMVectorSubtract(mouseEye, DirectX::XMVectorScale(mouseRight, deltaX * mousePanUnitsPerPixel));
                mouseEye = DirectX::XMVectorAdd(mouseEye, DirectX::XMVectorScale(up, deltaY * mousePanUnitsPerPixel));
                DirectX::XMStoreFloat3(&camera.position, mouseEye);
            }

            if (middleDown)
            {
                // Vertical movement only, mimicking W/S - a mouse wheel would be the more
                // conventional zoom trigger, but wheel delta only arrives via window messages
                // (WM_MOUSEWHEEL), which requires the window to hold true keyboard focus, not
                // just be the foreground window - confirmed via logging that it never actually
                // reached this app in practice, while middle-button *state* (polled exactly
                // like the left/right buttons above) reliably does. Polling over messages,
                // consistent with every other input this function reads.
                constexpr float mouseZoomUnitsPerPixel = 0.02f;
                const DirectX::XMVECTOR mouseForward = ComputeForward(camera.yaw, camera.pitch);
                DirectX::XMVECTOR mouseEye = DirectX::XMLoadFloat3(&camera.position);
                mouseEye = DirectX::XMVectorSubtract(mouseEye, DirectX::XMVectorScale(mouseForward, deltaY * mouseZoomUnitsPerPixel));
                DirectX::XMStoreFloat3(&camera.position, mouseEye);
            }
        }

        lastCursor = cursor;
        wasDragging = dragging;
    }

    DirectX::XMFLOAT4X4 ComputeViewProjection(const Camera& camera, float aspectRatio)
    {
        using namespace DirectX;

        const XMVECTOR forward = ComputeForward(camera.yaw, camera.pitch);
        const XMVECTOR eye = XMLoadFloat3(&camera.position);
        const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        const XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
        const XMMATRIX projection = XMMatrixPerspectiveFovLH(camera.fovYRadians, aspectRatio, camera.nearPlane, camera.farPlane);

        // DirectXMath matrices are constructed for row-vector use (v' = v * M) and stored
        // row-major in memory. HLSL shaders declare this cbuffer's matrix as `row_major`
        // explicitly (see Triangle.hlsl/LitTextured.hlsl) - no default-packing guessing - so
        // the bytes below are read by the GPU exactly as laid out here, with no implicit
        // reinterpretation. Transposing once here, then using a matrix-first mul(M, vector) in
        // the shader, correctly reproduces v * M: mul(M^T, v) == (v * M) as a column result.
        XMFLOAT4X4 viewProjection;
        XMStoreFloat4x4(&viewProjection, XMMatrixTranspose(XMMatrixMultiply(view, projection)));
        return viewProjection;
    }
}
