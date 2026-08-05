#include "LocalTestScene.h"

#include "Asset/GltfImporter.h"
#include "Engine/ModelResources.h"
#include "Foundation/Log.h"

#include <filesystem>
#include <string>

namespace
{
    constexpr const char* kScenePath = "local/test_scene_source/Models/Scene/glTF/Scene.gltf";
}

namespace LocalTestScene
{
    std::vector<Renderer::StaticMeshDrawItem> LoadIfAvailable()
    {
        if (!std::filesystem::exists(kScenePath))
        {
            Foundation::Log::Write(
                Foundation::Log::Severity::Info, "Game",
                "Local test scene not found at '%s' - skipping (expected on a fresh clone; see local/ in .gitignore)", kScenePath);
            return {};
        }

        Asset::Model model;
        if (!Asset::ImportGltf(kScenePath, model))
        {
            return {};
        }

        const std::string directory = std::filesystem::path(kScenePath).parent_path().string();
        return Engine::CreateStaticMeshDrawItems(model, directory);
    }
}
