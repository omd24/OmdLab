#pragma once

#include "Clip.h"
#include "Material.h"
#include "Mesh.h"
#include "Node.h"
#include "Skin.h"
#include "Texture.h"

#include <cstdint>
#include <vector>

namespace Asset
{
    // Engine-agnostic CPU-side result of importing one source asset file (glTF today, other
    // formats later via their own importers - see the Asset format extensibility decision).
    // Deliberately shaped to describe more than the one character asset validated so far:
    // meshes/materials/textures/nodes are always present fields, even for a source file (like
    // the stick-figure test export) that populates some of them with nothing. Skins/clips are
    // the only genuinely optional pieces, since not every source file has animation.
    struct Model
    {
        std::vector<Mesh> meshes;
        std::vector<Material> materials;
        std::vector<Texture> textures;
        std::vector<Node> nodes;
        std::vector<int32_t> rootNodeIndices; // Indices into nodes.
        std::vector<Skin> skins;
        std::vector<Clip> clips;
    };
}
