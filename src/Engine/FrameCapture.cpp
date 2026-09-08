#include "FrameCapture.h"

#include "Foundation/Image.h"
#include "Foundation/Log.h"
#include "Renderer/Device.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

namespace
{
    using Foundation::Log::Severity;

    bool g_pending = false;
    std::string g_pendingTag;
    unsigned g_sequence = 0;
    std::string g_runStamp;

    const std::string& RunStamp()
    {
        if (g_runStamp.empty())
        {
            const std::time_t now = std::time(nullptr);
            std::tm local = {};
            localtime_s(&local, &now);
            char buffer[32];
            std::snprintf(
                buffer, sizeof(buffer), "%04d%02d%02d_%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                local.tm_hour, local.tm_min, local.tm_sec);
            g_runStamp = buffer;
        }
        return g_runStamp;
    }

    std::string SanitizeTag(const std::string& tag)
    {
        std::string out;
        for (const char c : tag)
        {
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
            out += ok ? c : '_';
        }
        return out.empty() ? std::string("shot") : out;
    }
}

namespace Engine::FrameCapture
{
    void Grab(const std::string& tag)
    {
        Renderer::Device::RequestCapture();
        g_pending = true;
        g_pendingTag = SanitizeTag(tag);
    }

    void ResolvePending()
    {
        if (!g_pending)
        {
            return;
        }

        std::vector<unsigned char> rgba;
        unsigned width = 0;
        unsigned height = 0;
        if (!Renderer::Device::TakeCapture(rgba, width, height))
        {
            // The renderer hasn't produced the capture yet (a DoFrame path that skipped the
            // EndFrame readback hook). Keep the request pending rather than dropping it.
            return;
        }
        g_pending = false;

        std::error_code ec;
        const std::filesystem::path directory = std::filesystem::path("local") / "captures";
        std::filesystem::create_directories(directory, ec);

        char name[128];
        std::snprintf(name, sizeof(name), "omdlab_%s_%04u_%s.png", RunStamp().c_str(), g_sequence++, g_pendingTag.c_str());
        const std::filesystem::path outPath = directory / name;

        if (Foundation::WritePng(outPath.string(), width, height, rgba.data(), width * 4u))
        {
            Foundation::Log::Write(
                Severity::Info, "FrameCapture", "wrote %s (%ux%u)", outPath.string().c_str(), width, height);
        }
    }
}
