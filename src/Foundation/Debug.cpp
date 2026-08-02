#include "Debug.h"

#include <cstdarg>
#include <cstdio>

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace Foundation::Debug
{
    void AssertFailed(const char* condText, const char* file, int line, const char* format, ...)
    {
        char message[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        Log::Write(
            Log::Severity::Error,
            "Assert",
            "(%s) failed at %s:%d - %s",
            condText, file, line, message);
    }

#ifdef OMD_WINDOWS
    void DebugPrint(const char* format, ...)
    {
        char message[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
#endif
}
