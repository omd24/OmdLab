#pragma once

#include <cstdint>
#include <string>

namespace Foundation
{
    // Encodes a 32bpp RGBA image (R,G,B,A byte order, top-down rows) to a PNG file via WIC
    // (Windows Imaging Component - part of Windows, no third-party dependency). rowPitchBytes is
    // the byte stride between rows in `rgba` and must be >= width*4; pass width*4 for a tightly
    // packed buffer. Creating parent directories is the caller's job. Returns false and logs the
    // HRESULT on any failure. Windows-only (WIC); a no-op returning false elsewhere.
    bool WritePng(const std::string& path, unsigned width, unsigned height, const std::uint8_t* rgba, unsigned rowPitchBytes);
}
