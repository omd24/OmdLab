#include "Window.h"

#include "Log.h"

#include <cstdio>

#ifdef OMD_WINDOWS

namespace
{
    HWND g_gameWindow = nullptr;

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        switch (msg)
        {
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    // Runs on a dedicated thread the OS creates for console control events - not the main
    // thread. Console close (or Ctrl+C, logoff, shutdown) would otherwise bypass our own
    // window's WM_DESTROY entirely and have Windows terminate the process directly, skipping
    // Device::Shutdown() and leaking GPU resources reported at process teardown. Posting
    // WM_CLOSE routes it through the same graceful shutdown as closing the game window.
    BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
    {
        if (g_gameWindow != nullptr)
        {
            PostMessageA(g_gameWindow, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }
}

#endif

namespace Foundation
{
    void CreateDebugConsole(const char* title)
    {
#ifdef OMD_WINDOWS
        AllocConsole();
        SetConsoleTitleA(title);

        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        freopen_s(&dummy, "CONIN$", "r", stdin);
#endif
    }

#ifdef OMD_WINDOWS

    HWND CreateGameWindow(const char* title, int width, int height)
    {
        WNDCLASSEXA windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleA(nullptr);
        // IDC_ARROW resolves to a wide-string resource id under the project-wide UNICODE
        // define regardless of which suffix we call, so it can't be passed to LoadCursorA
        // directly - MAKEINTRESOURCEA keeps this call consistently ANSI.
        windowClass.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512));
        windowClass.lpszClassName = "OmdLabWindowClass";

        if (!RegisterClassExA(&windowClass))
        {
            Log::Write(Log::Severity::Error, "Window", "RegisterClassExA failed");
            return nullptr;
        }

        RECT rect = { 0, 0, width, height };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

        HWND hwnd = CreateWindowExA(
            0,
            windowClass.lpszClassName,
            title,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, windowClass.hInstance, nullptr);

        if (hwnd == nullptr)
        {
            Log::Write(Log::Severity::Error, "Window", "CreateWindowExA failed");
            return nullptr;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        g_gameWindow = hwnd;
        if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE))
        {
            Log::Write(Log::Severity::Warning, "Window", "SetConsoleCtrlHandler registration failed");
        }

        return hwnd;
    }

    bool PumpMessages()
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return true;
    }

#endif
}
