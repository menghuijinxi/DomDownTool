#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "GeographicPolygon.h"

namespace DownTool
{
struct KmlDownloadRange
{
    Wgs84Polygons polygonsWgs84;
    GeographicBounds bounds;
    LonLat centerWgs84;
    double boundingRadiusMeters = 0.;
    std::string fileName;
};

[[nodiscard]] std::optional<KmlDownloadRange> LoadKmlDownloadRange(
    const std::filesystem::path& path, std::string& error);
} // namespace DownTool
