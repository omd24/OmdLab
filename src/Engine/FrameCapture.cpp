#include "FrameCapture.h"

#include "FixedTimestep.h"

#include "Foundation/Image.h"
#include "Foundation/Log.h"
#include "Renderer/Device.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using Foundation::Log::Severity;

    // --- single-frame state ---
    bool g_pending = false;
    std::string g_pendingTag;
    unsigned g_sequence = 0;
    std::string g_runStamp;

    // --- per-frame context ---
    struct Context
    {
        std::uint32_t tick = 0;
        std::string state;
        std::uint32_t framesInMove = 0;
        float clipSeconds = 0.0f;
        std::string moveId;
        bool set = false;
    };
    Context g_ctx;
    std::string g_annotation;

    // --- skeleton-space signal staging (set once per tick, consumed by ResolvePending) ---
    struct SkelStage
    {
        bool valid = false;         // a captured frame ever fed skeleton data
        bool effectorValid = false; // this frame's effector is a resolved bone
        float eff[3] = { 0.0f, 0.0f, 0.0f };
        float effSpeed = 0.0f;
        float effReach = 0.0f;
        float com[3] = { 0.0f, 0.0f, 0.0f };
        float comSpeed = 0.0f;
    };
    SkelStage g_skel;

    // One captured frame's full record - motion.csv is emitted from these at EndSession, and
    // summary.json's derived numbers are computed by scanning them.
    struct FrameRecord
    {
        unsigned seq = 0;
        std::uint32_t tick = 0;
        std::string caption;
        std::string state;
        std::uint32_t framesInMove = 0;
        float clipSeconds = 0.0f;
        long long pixelDelta = 0;
        long long pixelDeltaCropped = 0;
        bool effectorValid = false;
        float eff[3] = { 0.0f, 0.0f, 0.0f };
        float effSpeed = 0.0f;
        float effReach = 0.0f;
        float com[3] = { 0.0f, 0.0f, 0.0f };
        float comSpeed = 0.0f;
    };

    // Flag thresholds for summary.json. Heuristic - they only drive the WARN/OK flags and the
    // dead/frozen counts; the raw per-frame series is always in motion.csv to check against.
    constexpr float kDeadEffectorSpeed = 0.15f;    // world units/sec - hand effectively still (a frozen pose reads ~0)
    constexpr long long kDeadPixelDelta = 300000;  // matches the ~130-220k of an observed frozen tail, with headroom
    constexpr float kSettleEffectorSpeed = 1.5f;   // "motion has stopped" for retract timing - above idle-breathing (~0.3-0.7)

    // --- session state ---
    bool g_sessionActive = false;
    std::string g_sessionDir;
    std::string g_sessionName;
    std::vector<FrameRecord> g_records;
    std::vector<unsigned char> g_prevFrame;
    unsigned g_prevW = 0;
    unsigned g_prevH = 0;

    const std::string kNoSession;

    std::string TimeStamp()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        char buffer[32];
        std::snprintf(
            buffer, sizeof(buffer), "%04d%02d%02d_%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
            local.tm_min, local.tm_sec);
        return buffer;
    }

    const std::string& RunStamp()
    {
        if (g_runStamp.empty())
        {
            g_runStamp = TimeStamp();
        }
        return g_runStamp;
    }

    std::string Sanitize(const std::string& in, const char* fallback)
    {
        std::string out;
        for (const char c : in)
        {
            const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
            out += ok ? c : '_';
        }
        return out.empty() ? std::string(fallback) : out;
    }

    // Sum of per-channel absolute differences (R,G,B; alpha ignored) over either the whole
    // image or the given inclusive-exclusive pixel rect. Frames are tightly packed width*4 RGBA.
    long long FrameDelta(
        const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, unsigned w, unsigned h, unsigned x0, unsigned y0,
        unsigned x1, unsigned y1)
    {
        if (a.size() != b.size() || a.size() != static_cast<size_t>(w) * h * 4)
        {
            return 0;
        }
        long long sum = 0;
        for (unsigned y = y0; y < y1 && y < h; ++y)
        {
            const size_t row = static_cast<size_t>(y) * w * 4;
            for (unsigned x = x0; x < x1 && x < w; ++x)
            {
                const size_t p = row + static_cast<size_t>(x) * 4;
                sum += std::abs(static_cast<int>(a[p + 0]) - static_cast<int>(b[p + 0]));
                sum += std::abs(static_cast<int>(a[p + 1]) - static_cast<int>(b[p + 1]));
                sum += std::abs(static_cast<int>(a[p + 2]) - static_cast<int>(b[p + 2]));
            }
        }
        return sum;
    }

    // --- summary.json derivation ---------------------------------------------------------------
    // A run's quality as a flat set of numbers, computed once at EndSession by scanning the
    // frame records. Tick fields that don't apply are -1 (documented, not null - keeps any
    // reader's parsing trivial). "Attack" is the state of interest for the *Ticks fields; a
    // non-attack script just leaves them at -1.
    struct Summary
    {
        int frames = 0;
        std::uint32_t firstTick = 0;
        std::uint32_t lastTick = 0;
        int nonIdleTicks = 0;
        long attackFirstTick = -1;
        long attackLastTick = -1;
        double peakEffectorSpeed = 0.0;
        long peakEffectorSpeedTick = -1;
        int timeToPeakTicks = -1;      // peak hand speed, relative to the first Attack tick
        double maxEffectorReach = 0.0;
        long maxEffectorReachTick = -1; // ~ the contact / full-extension frame
        int timeToContactTicks = -1;
        int retractTicks = -1;         // contact -> last tick the hand was still moving (>= kSettleEffectorSpeed)
        int deadFrames = 0;            // non-Idle frames with the hand near-still AND a tiny pixel delta
        int frozenTailTicks = 0;       // trailing run of dead frames in the last non-Idle stretch - the "freeze"
        double comTravelXZ = 0.0;
        double comPeakSpeed = 0.0;
        int jumpTicks = 0;      // frames in the "Jump" state - the airtime
        double comMaxY = 0.0;   // peak body height (com Y incl. the fighter's own Transform.y)
        long long pixelDeltaCropMax = 0;
        double pixelDeltaCropMean = 0.0;
        bool hasEffector = false;
        std::string verdict = "OK";
        std::string flags;
    };

    Summary ComputeSummary(const std::vector<FrameRecord>& recs)
    {
        Summary s;
        s.frames = static_cast<int>(recs.size());
        if (recs.empty())
        {
            return s;
        }
        s.firstTick = recs.front().tick;
        s.lastTick = recs.back().tick;

        double comFirstX = 0.0;
        double comFirstZ = 0.0;
        double comLastX = 0.0;
        double comLastZ = 0.0;
        bool comSeen = false;
        double pixSum = 0.0;
        int pixN = 0;
        long lastMovingEffTick = -1;

        for (size_t i = 0; i < recs.size(); ++i)
        {
            const FrameRecord& r = recs[i];
            if (r.state != "Idle")
            {
                ++s.nonIdleTicks;
            }
            if (r.state == "Jump")
            {
                ++s.jumpTicks;
            }
            if (static_cast<double>(r.com[1]) > s.comMaxY)
            {
                s.comMaxY = r.com[1];
            }
            if (r.state == "Attack")
            {
                if (s.attackFirstTick < 0)
                {
                    s.attackFirstTick = static_cast<long>(r.tick);
                }
                s.attackLastTick = static_cast<long>(r.tick);
            }
            // A frame whose state differs from the one before it is a crossfade boundary - the
            // effector "speed" there is the blend discontinuity, not real motion - so speed
            // metrics skip it. Reach is a static per-frame quantity, unaffected.
            const bool transitionFrame = i > 0 && r.state != recs[i - 1].state;
            if (r.effectorValid)
            {
                s.hasEffector = true;
                if (r.effReach > s.maxEffectorReach)
                {
                    s.maxEffectorReach = r.effReach;
                    s.maxEffectorReachTick = static_cast<long>(r.tick);
                }
                if (i > 0 && !transitionFrame) // seq 0 has no previous frame; transitions carry a blend spike
                {
                    if (r.effSpeed > s.peakEffectorSpeed)
                    {
                        s.peakEffectorSpeed = r.effSpeed;
                        s.peakEffectorSpeedTick = static_cast<long>(r.tick);
                    }
                    // "Still moving" is an in-Attack concept - idle breathing after the move
                    // shouldn't extend the retract measurement.
                    if (r.state == "Attack" && r.effSpeed >= kSettleEffectorSpeed)
                    {
                        lastMovingEffTick = static_cast<long>(r.tick);
                    }
                }
            }
            if (i > 0)
            {
                if (static_cast<double>(r.comSpeed) > s.comPeakSpeed)
                {
                    s.comPeakSpeed = r.comSpeed;
                }
                pixSum += static_cast<double>(r.pixelDeltaCropped);
                ++pixN;
                if (r.pixelDeltaCropped > s.pixelDeltaCropMax)
                {
                    s.pixelDeltaCropMax = r.pixelDeltaCropped;
                }
            }
            if (!comSeen)
            {
                comFirstX = r.com[0];
                comFirstZ = r.com[2];
                comSeen = true;
            }
            comLastX = r.com[0];
            comLastZ = r.com[2];
        }

        if (pixN > 0)
        {
            s.pixelDeltaCropMean = pixSum / pixN;
        }
        if (comSeen)
        {
            const double dx = comLastX - comFirstX;
            const double dz = comLastZ - comFirstZ;
            s.comTravelXZ = std::sqrt(dx * dx + dz * dz);
        }
        if (s.attackFirstTick >= 0 && s.peakEffectorSpeedTick >= 0)
        {
            s.timeToPeakTicks = static_cast<int>(s.peakEffectorSpeedTick - s.attackFirstTick);
        }
        if (s.attackFirstTick >= 0 && s.maxEffectorReachTick >= 0)
        {
            s.timeToContactTicks = static_cast<int>(s.maxEffectorReachTick - s.attackFirstTick);
        }
        if (s.attackFirstTick >= 0 && s.maxEffectorReachTick >= 0)
        {
            s.retractTicks = (lastMovingEffTick >= s.maxEffectorReachTick)
                                 ? static_cast<int>(lastMovingEffTick - s.maxEffectorReachTick)
                                 : 0;
        }

        long lastNonIdle = -1;
        for (size_t i = 0; i < recs.size(); ++i)
        {
            if (recs[i].state != "Idle")
            {
                lastNonIdle = static_cast<long>(i);
            }
        }
        const auto isDead = [](const FrameRecord& r)
        {
            const bool still = r.effectorValid ? (r.effSpeed < kDeadEffectorSpeed) : true;
            return r.state != "Idle" && still && r.pixelDeltaCropped < kDeadPixelDelta;
        };
        for (size_t i = 1; i < recs.size(); ++i)
        {
            if (isDead(recs[i]))
            {
                ++s.deadFrames;
            }
        }
        for (long i = lastNonIdle; i >= 1; --i)
        {
            if (isDead(recs[static_cast<size_t>(i)]))
            {
                ++s.frozenTailTicks;
            }
            else
            {
                break;
            }
        }

        if (s.frozenTailTicks >= 3)
        {
            s.verdict = "WARN";
            s.flags += "frozen-tail ";
        }
        if (s.deadFrames > 5)
        {
            s.verdict = "WARN";
            s.flags += "dead-frames ";
        }
        while (!s.flags.empty() && s.flags.back() == ' ')
        {
            s.flags.pop_back();
        }
        return s;
    }

    bool WriteSummaryJson(const std::filesystem::path& path, const std::string& runName, const Summary& s)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            return false;
        }
        constexpr float kDt = Engine::FixedTimestepAccumulator::kFixedDeltaSeconds;
        char buf[2048];
        std::snprintf(
            buf, sizeof(buf),
            "{\n"
            "  \"run\": \"%s\",\n"
            "  \"verdict\": \"%s\",\n"
            "  \"flags\": \"%s\",\n"
            "  \"frames\": %d,\n"
            "  \"firstTick\": %u,\n"
            "  \"lastTick\": %u,\n"
            "  \"nonIdleTicks\": %d,\n"
            "  \"attackFirstTick\": %ld,\n"
            "  \"attackLastTick\": %ld,\n"
            "  \"hasEffector\": %s,\n"
            "  \"peakEffectorSpeed\": %.4f,\n"
            "  \"peakEffectorSpeedTick\": %ld,\n"
            "  \"timeToPeakTicks\": %d,\n"
            "  \"timeToPeakSeconds\": %.4f,\n"
            "  \"maxEffectorReach\": %.4f,\n"
            "  \"maxEffectorReachTick\": %ld,\n"
            "  \"timeToContactTicks\": %d,\n"
            "  \"timeToContactSeconds\": %.4f,\n"
            "  \"retractTicks\": %d,\n"
            "  \"deadFrames\": %d,\n"
            "  \"frozenTailTicks\": %d,\n"
            "  \"comTravelXZ\": %.4f,\n"
            "  \"comPeakSpeed\": %.4f,\n"
            "  \"jumpTicks\": %d,\n"
            "  \"jumpAirtimeSeconds\": %.4f,\n"
            "  \"comMaxY\": %.4f,\n"
            "  \"pixelDeltaCropMax\": %lld,\n"
            "  \"pixelDeltaCropMean\": %.1f\n"
            "}\n",
            runName.c_str(), s.verdict.c_str(), s.flags.c_str(), s.frames, s.firstTick, s.lastTick, s.nonIdleTicks,
            s.attackFirstTick, s.attackLastTick, s.hasEffector ? "true" : "false", s.peakEffectorSpeed, s.peakEffectorSpeedTick,
            s.timeToPeakTicks, s.timeToPeakTicks >= 0 ? s.timeToPeakTicks * kDt : -1.0f, s.maxEffectorReach,
            s.maxEffectorReachTick, s.timeToContactTicks, s.timeToContactTicks >= 0 ? s.timeToContactTicks * kDt : -1.0f,
            s.retractTicks, s.deadFrames, s.frozenTailTicks, s.comTravelXZ, s.comPeakSpeed, s.jumpTicks, s.jumpTicks * kDt,
            s.comMaxY, s.pixelDeltaCropMax, s.pixelDeltaCropMean);
        out << buf;
        return true;
    }
}

namespace Engine::FrameCapture
{
    void SetContext(std::uint32_t tick, const char* fighterState, std::uint32_t framesInMove, float clipSeconds, const char* moveId)
    {
        g_ctx.tick = tick;
        g_ctx.state = fighterState ? fighterState : "";
        g_ctx.framesInMove = framesInMove;
        g_ctx.clipSeconds = clipSeconds;
        g_ctx.moveId = moveId ? moveId : "";
        g_ctx.set = true;

        char buffer[160];
        std::snprintf(
            buffer, sizeof(buffer), "t %u  %s  f %u  clip %.3f%s%s", g_ctx.tick, g_ctx.state.c_str(), g_ctx.framesInMove,
            g_ctx.clipSeconds, g_ctx.moveId.empty() ? "" : "  ", g_ctx.moveId.c_str());
        g_annotation = buffer;
    }

    const char* AnnotationText()
    {
        return g_ctx.set ? g_annotation.c_str() : "";
    }

    void SetSkeleton(
        bool effectorValid, const float effectorPos[3], float effectorSpeed, float effectorReach, const float comPos[3],
        float comSpeed)
    {
        g_skel.valid = true;
        g_skel.effectorValid = effectorValid;
        g_skel.eff[0] = effectorPos[0];
        g_skel.eff[1] = effectorPos[1];
        g_skel.eff[2] = effectorPos[2];
        g_skel.effSpeed = effectorSpeed;
        g_skel.effReach = effectorReach;
        g_skel.com[0] = comPos[0];
        g_skel.com[1] = comPos[1];
        g_skel.com[2] = comPos[2];
        g_skel.comSpeed = comSpeed;
    }

    void Grab(const std::string& tag)
    {
        Renderer::Device::RequestCapture();
        g_pending = true;
        g_pendingTag = Sanitize(tag, "shot");
    }

    void BeginSession(const std::string& name)
    {
        const std::string safe = Sanitize(name, "capture");
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path("local") / "captures" / (safe + "_" + TimeStamp());
        std::filesystem::create_directories(dir, ec);
        g_sessionDir = dir.string();
        g_sessionName = safe;
        g_sessionActive = true;
        g_sequence = 0;
        g_records.clear();
        g_skel = SkelStage{};
        g_prevFrame.clear();
        g_prevW = g_prevH = 0;
        Foundation::Log::Write(Severity::Info, "FrameCapture", "session '%s' -> %s", safe.c_str(), g_sessionDir.c_str());
    }

    void EndSession()
    {
        if (!g_sessionActive)
        {
            return;
        }
        const std::filesystem::path csvPath = std::filesystem::path(g_sessionDir) / "motion.csv";
        std::ofstream out(csvPath, std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            out << "seq,tick,caption,state,framesInMove,clipSeconds,pixelDelta,pixelDeltaCropped,"
                   "effX,effY,effZ,effSpeed,effReach,comX,comY,comZ,comSpeed\n";
            for (const FrameRecord& r : g_records)
            {
                char row[512];
                if (r.effectorValid)
                {
                    std::snprintf(
                        row, sizeof(row), "%u,%u,%s,%s,%u,%.4f,%lld,%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", r.seq,
                        r.tick, r.caption.c_str(), r.state.c_str(), r.framesInMove, r.clipSeconds, r.pixelDelta, r.pixelDeltaCropped,
                        r.eff[0], r.eff[1], r.eff[2], r.effSpeed, r.effReach, r.com[0], r.com[1], r.com[2], r.comSpeed);
                }
                else
                {
                    // Effector columns left empty when no strike bone is resolved this frame.
                    std::snprintf(
                        row, sizeof(row), "%u,%u,%s,%s,%u,%.4f,%lld,%lld,,,,,,%.4f,%.4f,%.4f,%.4f", r.seq, r.tick,
                        r.caption.c_str(), r.state.c_str(), r.framesInMove, r.clipSeconds, r.pixelDelta, r.pixelDeltaCropped,
                        r.com[0], r.com[1], r.com[2], r.comSpeed);
                }
                out << row << "\n";
            }
            out.close();
            Foundation::Log::Write(
                Severity::Info, "FrameCapture", "session done: %zu frame(s), %s", g_records.size(), csvPath.string().c_str());
        }
        else
        {
            Foundation::Log::Write(Severity::Error, "FrameCapture", "session: could not write %s", csvPath.string().c_str());
        }

        const Summary summary = ComputeSummary(g_records);
        const std::filesystem::path jsonPath = std::filesystem::path(g_sessionDir) / "summary.json";
        if (WriteSummaryJson(jsonPath, g_sessionName, summary))
        {
            Foundation::Log::Write(
                Severity::Info, "FrameCapture",
                "summary [%s]: %d frames  contact@t%ld (ttc %d)  peakHand %.2f@t%ld  retract %dt  dead %d  frozenTail %d  comTravel %.3f  -> %s%s%s",
                summary.verdict.c_str(), summary.frames, summary.maxEffectorReachTick, summary.timeToContactTicks,
                summary.peakEffectorSpeed, summary.peakEffectorSpeedTick, summary.retractTicks, summary.deadFrames,
                summary.frozenTailTicks, summary.comTravelXZ, summary.verdict.c_str(), summary.flags.empty() ? "" : " ",
                summary.flags.c_str());
        }

        g_sessionActive = false;
        g_prevFrame.clear();
    }

    const std::string& SessionDir()
    {
        return g_sessionActive ? g_sessionDir : kNoSession;
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
            return; // renderer hasn't produced it yet - keep the request pending
        }
        g_pending = false;

        std::filesystem::path outPath;
        if (g_sessionActive)
        {
            char name[128];
            std::snprintf(name, sizeof(name), "%04u_%s.png", g_sequence, g_pendingTag.c_str());
            outPath = std::filesystem::path(g_sessionDir) / name;
        }
        else
        {
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::path("local") / "captures";
            std::filesystem::create_directories(dir, ec);
            char name[128];
            std::snprintf(name, sizeof(name), "omdlab_%s_%04u_%s.png", RunStamp().c_str(), g_sequence, g_pendingTag.c_str());
            outPath = dir / name;
        }

        const bool wrote = Foundation::WritePng(outPath.string(), width, height, rgba.data(), width * 4u);
        if (wrote)
        {
            Foundation::Log::Write(Severity::Info, "FrameCapture", "wrote %s (%ux%u)", outPath.string().c_str(), width, height);
        }

        if (g_sessionActive)
        {
            // Centre-of-frame crop where the fighters live (fractions of the frame, so it holds
            // for any capture resolution). The whole-frame delta is also recorded.
            const unsigned cx0 = static_cast<unsigned>(width * 0.28f);
            const unsigned cx1 = static_cast<unsigned>(width * 0.72f);
            const unsigned cy0 = static_cast<unsigned>(height * 0.28f);
            const unsigned cy1 = static_cast<unsigned>(height * 0.95f);

            long long full = 0;
            long long crop = 0;
            if (!g_prevFrame.empty() && g_prevW == width && g_prevH == height)
            {
                full = FrameDelta(rgba, g_prevFrame, width, height, 0, 0, width, height);
                crop = FrameDelta(rgba, g_prevFrame, width, height, cx0, cy0, cx1, cy1);
            }

            FrameRecord rec;
            rec.seq = g_sequence;
            rec.tick = g_ctx.tick;
            rec.caption = g_pendingTag;
            rec.state = g_ctx.state;
            rec.framesInMove = g_ctx.framesInMove;
            rec.clipSeconds = g_ctx.clipSeconds;
            rec.pixelDelta = full;
            rec.pixelDeltaCropped = crop;
            rec.effectorValid = g_skel.effectorValid;
            rec.eff[0] = g_skel.eff[0];
            rec.eff[1] = g_skel.eff[1];
            rec.eff[2] = g_skel.eff[2];
            rec.effSpeed = g_skel.effSpeed;
            rec.effReach = g_skel.effReach;
            rec.com[0] = g_skel.com[0];
            rec.com[1] = g_skel.com[1];
            rec.com[2] = g_skel.com[2];
            rec.comSpeed = g_skel.comSpeed;
            g_records.push_back(std::move(rec));

            g_prevFrame = rgba;
            g_prevW = width;
            g_prevH = height;
        }

        ++g_sequence;
    }
}
