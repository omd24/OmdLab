#pragma once

#include "PlatformMacros.h"
#include "TextureHandle.h"

#if defined(OMD_GFX_DX12)
    #include "dx12/TextureDX12.h"
#endif

namespace Renderer
{
    // Default-heap-backed 2D GPU texture, always RGBA8 (4 bytes/pixel, tightly packed) - the
    // one format every source this engine loads (glTF's PNG/JPG via stb_image) normalizes to.
    // Backend-agnostic front layer - see PlatformMacros.h. Unlike Buffer's simple upload-heap
    // path, this uses a real default-heap resource plus an upload-heap staging copy, since
    // textures are sampled far more often than the tiny one-shot vertex buffer that made
    // upload-heap-only acceptable there.
    //
    // TODO(OM): single mip level only (see TextureDX12::Create) - fine for textures viewed at a
    // roughly steady distance/angle (the character), but a surface viewed at a grazing angle
    // receding into the distance (the ground plane's checkerboard) visibly shimmers/aliases with
    // no mip chain to sample a pre-filtered average from. Generate a real mip chain (+ update the
    // SRV's MipLevels and the upload path to copy each level) once a surface actually needs it.
    struct Texture : public OMD_GFX_CLASS(Texture)
    {
        static TextureHandle Create(const void* pixels, unsigned int width, unsigned int height)
        {
            return OMD_GFX_CALL(Texture, Create(pixels, width, height));
        }

        static void Shutdown()
        {
            OMD_GFX_CALL(Texture, Shutdown());
        }
    };
}
