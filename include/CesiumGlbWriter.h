#pragma once

#include <filesystem>
#include <string>

struct aiScene;

namespace DownTool::cesium
{
[[nodiscard]] bool writeGlb(
    const aiScene& scene, const std::filesystem::path& path,
    std::string& error, bool textureMipmaps = false);
}
