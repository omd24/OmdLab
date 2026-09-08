#pragma once

#include <string>

namespace Engine::FrameCapture
{
    // Dev-only single-frame screenshot: writes the rendered back buffer to local/captures/ as
    // a PNG. Two-step because the readback the renderer does is only valid after the frame's
    // present + GPU wait:
    //   Grab(tag)        - queue a capture of the frame about to be rendered (before DoFrame).
    //   ResolvePending() - call once per render frame AFTER DoFrame; encodes + writes whatever
    //                      the renderer produced. No-op when nothing is queued.
    // Filenames: local/captures/omdlab_<runStamp>_<seq>_<tag>.png. Cheap and inert unless
    // Grab() is ever called, so it needs no OMD_DEV_TOOLS guard of its own - the callers gate.
    void Grab(const std::string& tag);
    void ResolvePending();
}
