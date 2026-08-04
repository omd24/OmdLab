#pragma once

#include <string>

namespace Asset
{
    // A reference to an image file backing a material texture slot. Loading pixel data and
    // uploading it to the GPU is Renderer's job, not Asset's - this is just "which file, for
    // which slot" (see Model.h for why this exists even for import targets with zero textures).
    struct Texture
    {
        std::string filePath;
    };
}
