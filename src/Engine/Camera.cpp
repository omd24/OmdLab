#include "Camera.h"

#include "Foundation/Input.h"

#include <cmath>

namespace
{
    DirectX::XMVECTOR ComputeForward(float yaw, float pitch)
    {
        return DirectX::XMVectorSet(sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch), 0.0f);
    }
}

namespace Engine
{
    void UpdateFreeFlyCamera(Camera& camera, HWND window, float deltaSeconds, bool allowMouseControl, bool allowKeyboardControl)
    {
        if (GetForegroundWindow() != window)
        {
            return;
        }

        constexpr float moveUnitsPerSecond = 3.0f;
        constexpr float turnRadiansPerSecond = 1.5f;

        if (allowKeyboardControl && Foundation::IsKeyDown(VK_LEFT))
        {
            camera.yaw -= turnRadiansPerSecond * deltaSeconds;
        }
        if (allowKeyboardControl && Foundation::IsKeyDown(VK_RIGHT))
        {
            camera.yaw += turnRadiansPerSecond * deltaSeconds;
        }
        if (allowKeyboardControl && Foundation::IsKeyDown(VK_UP))
        {
            camera.pitch += turnRadiansPerSecond * deltaSeconds;
        }
        if (allowKeyboardControl && Foundation::IsKeyDown(VK_DOWN))
        {
            camera.pitch -= turnRadiansPerSecond * deltaSeconds;
        }

        // Gamepad slot 0 mimics the keyboard/mouse axes above (right stick = look, left stick =
        // move further down) - read directly from Foundation::Input, not Engine::InputCommand,
        // for the same reason the keyboard/mouse polling above is: this is debug camera
        // tooling, not gameplay input, and InputCommand is deliberately shaped for the latter
        // only (see Input.h).
        const Foundation::GamepadState gamepad = Foundation::GetGamepadState(0);
        if (gamepad.connected)
        {
            camera.yaw += gamepad.rightStickX * turnRadiansPerSecond * deltaSeconds;
            camera.pitch += gamepad.rightStickY * turnRadiansPerSecond * deltaSeconds;
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
        if (allowKeyboardControl && Foundation::IsKeyDown('W'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(forward, moveStep));
        }
        if (allowKeyboardControl && Foundation::IsKeyDown('S'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(forward, moveStep));
        }
        if (allowKeyboardControl && Foundation::IsKeyDown('D'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(right, moveStep));
        }
        if (allowKeyboardControl && Foundation::IsKeyDown('A'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(right, moveStep));
        }
        if (allowKeyboardControl && Foundation::IsKeyDown('Q'))
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(up, moveStep));
        }
        if (allowKeyboardControl && Foundation::IsKeyDown('E'))
        {
            eye = DirectX::XMVectorSubtract(eye, DirectX::XMVectorScale(up, moveStep));
        }
        // Left stick mimics W/S (forward axis) and A/D (strafe axis).
        if (gamepad.connected)
        {
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(forward, gamepad.leftStickY * moveStep));
            eye = DirectX::XMVectorAdd(eye, DirectX::XMVectorScale(right, gamepad.leftStickX * moveStep));
        }
        DirectX::XMStoreFloat3(&camera.position, eye);

        // Mouse-drag navigation, mimicking the keyboard axes above rather than a full
        // DCC-style orbit/pan/zoom rig - see the header comment. lastCursor/wasDragging are
        // static rather than fields on Camera itself, since they're input-tracking state, not
        // camera state - fine as long as only one camera ever calls this, which is the only
        // case that exists today.
        static long lastCursorX = 0;
        static long lastCursorY = 0;
        static bool wasDragging = false;

        const Foundation::MouseState mouse = Foundation::GetMouseState();
        // Forced false rather than skipping this whole block when disallowed - lastCursorX/Y
        // below still need to track the real cursor position every frame regardless, so that
        // whenever mouse control resumes mid-drag, the existing "first frame of a drag has no
        // meaningful last position" guard (dragging && wasDragging) is what naturally prevents a
        // jump, rather than a second, separate mechanism.
        const bool leftDown = allowMouseControl && mouse.leftDown;
        const bool rightDown = allowMouseControl && mouse.rightDown;
        const bool middleDown = allowMouseControl && mouse.middleDown;
        const bool dragging = leftDown || rightDown || middleDown;

        // Only apply a delta once a full frame has already seen the button held - the first
        // frame of a drag has no meaningful "last position" yet, and applying one would jump
        // the camera by however far the cursor happened to be from wherever it last was.
        if (dragging && wasDragging)
        {
            const float deltaX = static_cast<float>(mouse.x - lastCursorX);
            const float deltaY = static_cast<float>(mouse.y - lastCursorY);

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

        lastCursorX = mouse.x;
        lastCursorY = mouse.y;
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

    CameraBasis ComputeCameraBasis(const Camera& camera)
    {
        using namespace DirectX;

        const XMVECTOR forward = ComputeForward(camera.yaw, camera.pitch);
        const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        // Same right/up derivation UpdateFreeFlyCamera/ComputeViewProjection's XMMatrixLookToLH
        // already use internally - kept consistent so this basis matches what's actually on
        // screen, not a second, independently-derived convention.
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(worldUp, forward));
        const XMVECTOR up = XMVector3Cross(forward, right);

        CameraBasis basis;
        XMStoreFloat3(&basis.right, right);
        XMStoreFloat3(&basis.up, up);
        XMStoreFloat3(&basis.forward, forward);
        return basis;
    }
}
