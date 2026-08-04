#pragma once

#include "Model.h"

namespace Asset
{
    // Parses a glTF 2.0 file (.gltf or .glb) into engine-agnostic CPU structs via cgltf.
    // Returns false (and logs the reason) on any read/parse failure; outModel is left
    // default-constructed (empty) in that case.
    bool ImportGltf(const char* filePath, Model& outModel);
}
