#pragma once

#include "Log.h"

#ifdef OMD_WINDOWS
    #include <intrin.h>
#endif

namespace Foundation::Debug
{
    // Formats and logs an assertion failure at Error severity under the "Assert" category.
    // Called by OMD_ASSERT before it breaks into the debugger.
    void AssertFailed(const char* condText, const char* file, int line, const char* format, ...);

#ifdef OMD_WINDOWS
    // Writes a formatted string straight to the debugger's Output window. Not routed through
    // Log (no severity, category, or file write) - for throwaway manual debugging only.
    void DebugPrint(const char* format, ...);
#endif
}

// Set to 0 (via this define or a build-level override) to strip OMD_ASSERT entirely.
#ifndef OMD_ASSERT_ENABLED
    #define OMD_ASSERT_ENABLED 1
#endif

#if OMD_ASSERT_ENABLED
    // Wrapped in __pragma(warning) because assert conditions are frequently compile-time
    // constants (e.g. sanity checks), which would otherwise trigger C4127 at every call site.
    #define OMD_ASSERT(cond, format, ...) \
        do \
        { \
            __pragma(warning(push)) \
            __pragma(warning(disable: 4127)) \
            if (!(cond)) \
            { \
                Foundation::Debug::AssertFailed(#cond, __FILE__, __LINE__, format, ##__VA_ARGS__); \
                __debugbreak(); \
            } \
            __pragma(warning(pop)) \
        } while (0)
#else
    #define OMD_ASSERT(cond, format, ...) ((void)0)
#endif

#if defined(OMD_DEBUG) && defined(OMD_WINDOWS)
    #define OMD_DEBUG_PRINT(format, ...) Foundation::Debug::DebugPrint(format, ##__VA_ARGS__)
#else
    #define OMD_DEBUG_PRINT(format, ...) ((void)0)
#endif
