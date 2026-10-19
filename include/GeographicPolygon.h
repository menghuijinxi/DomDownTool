#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include "DataType.h"

namespace DownTool
{
using Wgs84Ring = std::vector<LonLat>;
using Wgs84Polygons = std::vector<Wgs84Ring>;

struct GeographicBounds
{
    double west = 0.;
    double east = 0.;
    double south = 0.;
    double north = 0.;
};

[[nodiscard]] bool ValidateWgs84Polygons(
    const Wgs84Polygons& polygons, std::string& error);

[[nodiscard]] bool IsSimpleWgs84Ring(const Wgs84Ring& ring);
[[nodiscard]] bool GeographicRingContainsPoint(const Wgs84Ring& ring, const LonLat& point);

[[nodiscard]] std::optional<GeographicBounds> CalculateGeographicBounds(
    const Wgs84Polygons& polygons);

[[nodiscard]] std::optional<GeographicBounds> CalculateGeographicBounds(
    const LonLat& centerWgs84, double radiusMeters);

[[nodiscard]] LonLat GeographicBoundsCenter(const GeographicBounds& bounds);

[[nodiscard]] double GeographicBoundsRadiusMeters(
    const GeographicBounds& bounds, const LonLat& center);

[[nodiscard]] bool GeographicPolygonsIntersectBounds(
    const Wgs84Polygons& polygons, const GeographicBounds& bounds);
} // namespace DownTool
