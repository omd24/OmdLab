#include "Log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>

#ifdef OMD_WINDOWS
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace Foundation::Log
{
    namespace
    {
        std::mutex g_mutex;
        FILE* g_logFile = nullptr;

        const char* SeverityToString(Severity severity)
        {
            switch (severity)
            {
                case Severity::Info:    return "Info";
                case Severity::Warning: return "Warning";
                case Severity::Error:   return "Error";
            }
            return "Unknown";
        }
    }

    void Init(const char* logFilePath)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_logFile != nullptr)
        {
            std::fclose(g_logFile);
        }

        const std::filesystem::path path(logFilePath);
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        fopen_s(&g_logFile, logFilePath, "w");
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_logFile != nullptr)
        {
            std::fclose(g_logFile);
            g_logFile = nullptr;
        }
    }

    void Write(Severity severity, const char* category, const char* format, ...)
    {
        char message[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);

        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const std::time_t nowTimeT = std::chrono::system_clock::to_time_t(now);

        std::tm localTime;
        localtime_s(&localTime, &nowTimeT);

        char line[1200];
        std::snprintf(
            line, sizeof(line),
            "[%02d:%02d:%02d.%03d][%s][%s] %s\n",
            localTime.tm_hour, localTime.tm_min, localTime.tm_sec, static_cast<int>(ms.count()),
            SeverityToString(severity), category, message);

        std::lock_guard<std::mutex> lock(g_mutex);

        std::fputs(line, stdout);

        if (g_logFile != nullptr)
        {
            std::fputs(line, g_logFile);
            std::fflush(g_logFile);
        }

#ifdef OMD_WINDOWS
        OutputDebugStringA(line);
#endif
    }
}
