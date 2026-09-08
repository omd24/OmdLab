#pragma once

#include <cstdint>
#include <string>

namespace Engine::FrameCapture
{
    // --- single-frame screenshot ------------------------------------------------------------
    // Grab(tag) queues a capture of the frame about to be rendered (call before DoFrame).
    // ResolvePending() encodes + writes whatever the renderer produced (call once per render
    // frame after DoFrame). No-op unless Grab() was called, so no OMD_DEV_TOOLS guard of its
    // own - the callers gate. Files: local/captures/omdlab_<runStamp>_<seq>_<tag>.png, or
    // <sessionDir>/<seq>_<tag>.png while a session is open.
    void Grab(const std::string& tag);
    void ResolvePending();

    // --- per-frame context ----------------------------------------------------------------
    // Call once per render frame (dev builds) before DoFrame. Feeds both the burned-in
    // annotation overlay and the motion.csv rows.
    void SetContext(std::uint32_t tick, const char* fighterState, std::uint32_t framesInMove, float clipSeconds, const char* moveId);
    // A formatted one-liner ("t 1234  Attack  f 14  clip 0.512  punch"); "" until SetContext is
    // called. Pass to RenderTasks::DoFrame so it's drawn into the frame (survives Ctrl+R).
    const char* AnnotationText();

    // --- skeleton-space signal (scripted sessions) ------------------------------------------
    // Call once per render frame in a capture run, before Grab(), with the player fighter's
    // composed world-space metrics for this tick: the strike effector (the active move's
    // hitbox bone, or a resolved default hand while not attacking), its speed in world
    // units/sec, its distance from the body centre (reach), and the body centre of mass +
    // its speed. Recorded as motion.csv columns and folded into summary.json's derived
    // numbers - this is the noise-free "is the move still moving" signal the pixel delta
    // can't isolate. effectorValid=false writes those columns empty and drops the frame from
    // effector-based summary metrics. No-op outside a session.
    void SetSkeleton(
        bool effectorValid, const float effectorPos[3], float effectorSpeed, float effectorReach, const float comPos[3],
        float comSpeed);

    // --- scripted capture session ------------------------------------------------------------
    // While a session is open, Grab() writes into local/captures/<name>_<stamp>/ and
    // ResolvePending() appends a motion.csv row: per-frame whole-image and centre-crop pixel
    // delta vs. the previous captured frame, the current context, and the skeleton-space
    // signal (SetSkeleton). EndSession() writes motion.csv plus summary.json - a one-object
    // rollup of derived numbers (time-to-contact, peak effector speed, retract ticks, dead /
    // frozen-tail frame counts, com travel, pixel-delta stats) so a run's quality is a set of
    // numbers to diff, not a CSV to eyeball - and logs a one-line digest.
    void BeginSession(const std::string& name);
    void EndSession();
    const std::string& SessionDir(); // "" if no session
}
