#pragma once

namespace Foundation::Log
{
    enum class Severity
    {
        Info,
        Warning,
        Error,
    };

    // Opens the log file for writing, truncating any previous run's log. Call once at startup.
    void Init(const char* logFilePath);

    // Flushes and closes the log file. Call once at shutdown.
    void Shutdown();

    // Writes one line to every active sink (console, log file, and the debugger's Output
    // window when running on Windows). Thread-safe.
    void Write(Severity severity, const char* category, const char* format, ...);
}
