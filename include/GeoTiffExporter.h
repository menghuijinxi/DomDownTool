#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "DataType.h"
#include "GeographicPolygon.h"

namespace DownTool
{
class CopernicusDemData;

struct WebMercatorRasterGrid
{
    double west = 0.;
    double east = 0.;
    double south = 0.;
    double north = 0.;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct WebMercatorTileGrid
{
    WebMercatorRasterGrid raster;
    TileRange tileRange;
    uint32_t tileSize = 0;
};

using GeoTiffCancellationCallback = std::function<bool()>;
using GeoTiffProgressCallback = std::function<void(uint64_t completed, uint64_t total)>;

[[nodiscard]] std::optional<GeographicBounds> CalculateExportGeographicBounds(
    const LonLat& centerWgs84,
    double radiusMeters,
    const Wgs84Polygons& polygonsWgs84);

[[nodiscard]] std::optional<WebMercatorTileGrid> BuildWebMercatorTileGrid(
    const GeographicBounds& geographicBounds,
    uint32_t zoom,
    uint32_t tileSize);

[[nodiscard]] std::optional<WebMercatorRasterGrid> BuildWebMercatorSampleGrid(
    const GeographicBounds& geographicBounds,
    double sampleMeters);

[[nodiscard]] bool WriteDomGeoTiff(
    const std::filesystem::path& baseTileRoot,
    const std::filesystem::path& overlayTileRoot,
    const std::filesystem::path& outputPath,
    const WebMercatorTileGrid& grid,
    const GeoTiffCancellationCallback& cancellationRequested,
    const GeoTiffProgressCallback& progressCallback,
    std::string& error);

[[nodiscard]] bool WriteDemGeoTiff(
    const CopernicusDemData& elevationData,
    const std::filesystem::path& outputPath,
    const WebMercatorRasterGrid& grid,
    const Wgs84Polygons& polygonsWgs84,
    const GeoTiffCancellationCallback& cancellationRequested,
    const GeoTiffProgressCallback& progressCallback,
    std::string& error);
} // namespace DownTool
