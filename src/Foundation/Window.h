#pragma once

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace Foundation
{
    // Allocates a console window and redirects stdin/stdout/stderr onto it. Without this,
    // stdout is not a valid handle under the Windows subsystem, so Log's console sink would
    // silently go nowhere.
    void CreateDebugConsole(const char* title);

#ifdef OMD_WINDOWS
    // Creates and shows a top-level game window. Returns null on failure.
    HWND CreateGameWindow(const char* title, int width, int height);

    // Pumps pending Win32 messages without blocking. Returns false once WM_QUIT has been
    // posted (e.g. the window was closed), true otherwise.
    bool PumpMessages();

    // Optional hook invoked first in the window procedure, before any of this project's own
    // message handling. Returning non-zero means the message was fully handled - the window
    // procedure returns that value immediately instead of continuing its own switch statement.
    // Generic on purpose: Foundation has no in-repo dependencies, so it can't know about
    // whatever higher layer wants raw message access (currently Renderer's ImGui backend).
    using WindowMessageHook = LRESULT (*)(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void SetWindowMessageHook(WindowMessageHook hook);
#endif
}
