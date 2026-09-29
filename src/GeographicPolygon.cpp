#include "GeographicPolygon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>

#include "GeographicLib/Geodesic.hpp"

namespace
{
constexpr double kGeometryEpsilon = 1.e-12;

bool IsFiniteCoordinate(const DownTool::LonLat& coordinate)
{
    return std::isfinite(coordinate.lon) && std::isfinite(coordinate.lat) &&
        coordinate.lon >= -180. && coordinate.lon <= 180. &&
        coordinate.lat >= -90. && coordinate.lat <= 90.;
}

bool SamePoint(const DownTool::LonLat& left, const DownTool::LonLat& right)
{
    return std::abs(left.lon - right.lon) <= kGeometryEpsilon &&
        std::abs(left.lat - right.lat) <= kGeometryEpsilon;
}

size_t CountDistinctPoints(const DownTool::Wgs84Ring& ring)
{
    std::vector<DownTool::LonLat> distinctPoints;
    distinctPoints.reserve(ring.size());
    for (const DownTool::LonLat& point : ring)
    {
        if (std::ranges::none_of(
                distinctPoints,
                [&point](const DownTool::LonLat& existing)
                {
                    return SamePoint(point, existing);
                }))
        {
            distinctPoints.push_back(point);
        }
    }
    return distinctPoints.size();
}

bool PointInsideBounds(
    const DownTool::LonLat& point, const DownTool::GeographicBounds& bounds)
{
    return point.lon >= bounds.west - kGeometryEpsilon &&
        point.lon <= bounds.east + kGeometryEpsilon &&
        point.lat >= bounds.south - kGeometryEpsilon &&
        point.lat <= bounds.north + kGeometryEpsilon;
}

double Cross(
    const DownTool::LonLat& first,
    const DownTool::LonLat& second,
    const DownTool::LonLat& third)
{
    return (second.lon - first.lon) * (third.lat - first.lat) -
        (second.lat - first.lat) * (third.lon - first.lon);
}

bool PointOnSegment(
    const DownTool::LonLat& point,
    const DownTool::LonLat& start,
    const DownTool::LonLat& end)
{
    return std::abs(Cross(start, end, point)) <= kGeometryEpsilon &&
        point.lon >= std::min(start.lon, end.lon) - kGeometryEpsilon &&
        point.lon <= std::max(start.lon, end.lon) + kGeometryEpsilon &&
        point.lat >= std::min(start.lat, end.lat) - kGeometryEpsilon &&
        point.lat <= std::max(start.lat, end.lat) + kGeometryEpsilon;
}

int Orientation(
    const DownTool::LonLat& first,
    const DownTool::LonLat& second,
    const DownTool::LonLat& third)
{
    const double cross = Cross(first, second, third);
    if (std::abs(cross) <= kGeometryEpsilon)
    {
        return 0;
    }
    return cross > 0. ? 1 : -1;
}

bool SegmentsIntersect(
    const DownTool::LonLat& firstStart,
    const DownTool::LonLat& firstEnd,
    const DownTool::LonLat& secondStart,
    const DownTool::LonLat& secondEnd)
{
    const int firstOrientation =
        Orientation(firstStart, firstEnd, secondStart);
    const int secondOrientation =
        Orientation(firstStart, firstEnd, secondEnd);
    const int thirdOrientation =
        Orientation(secondStart, secondEnd, firstStart);
    const int fourthOrientation =
        Orientation(secondStart, secondEnd, firstEnd);
    if (firstOrientation != secondOrientation &&
        thirdOrientation != fourthOrientation)
    {
        return true;
    }
    return (firstOrientation == 0 &&
            PointOnSegment(secondStart, firstStart, firstEnd)) ||
        (secondOrientation == 0 &&
         PointOnSegment(secondEnd, firstStart, firstEnd)) ||
        (thirdOrientation == 0 &&
         PointOnSegment(firstStart, secondStart, secondEnd)) ||
        (fourthOrientation == 0 &&
         PointOnSegment(firstEnd, secondStart, secondEnd));
}

bool PointInsideRing(
    const DownTool::LonLat& point, const DownTool::Wgs84Ring& ring)
{
    bool inside = false;
    for (size_t index = 0, previous = ring.size() - 1;
         index < ring.size();
         previous = index++)
    {
        const DownTool::LonLat& start = ring[previous];
        const DownTool::LonLat& end = ring[index];
        if (PointOnSegment(point, start, end))
        {
            return true;
        }
        const bool crossesLatitude =
            (start.lat > point.lat) != (end.lat > point.lat);
        if (!crossesLatitude)
        {
            continue;
        }
        const double longitude =
            (end.lon - start.lon) * (point.lat - start.lat) /
                (end.lat - start.lat) +
            start.lon;
        if (point.lon < longitude)
        {
            inside = !inside;
        }
    }
    return inside;
}

bool RingIntersectsBounds(
    const DownTool::Wgs84Ring& ring,
    const DownTool::GeographicBounds& bounds)
{
    if (std::ranges::any_of(
            ring,
            [&bounds](const DownTool::LonLat& point)
            {
                return PointInsideBounds(point, bounds);
            }))
    {
        return true;
    }

    const std::array<DownTool::LonLat, 4> corners = {{
        {bounds.west, bounds.north},
        {bounds.east, bounds.north},
        {bounds.east, bounds.south},
        {bounds.west, bounds.south}
    }};
    if (std::ranges::any_of(
            corners,
            [&ring](const DownTool::LonLat& point)
            {
                return PointInsideRing(point, ring);
            }))
    {
        return true;
    }

    for (size_t index = 0; index < ring.size(); ++index)
    {
        const DownTool::LonLat& start = ring[index];
        const DownTool::LonLat& end = ring[(index + 1) % ring.size()];
        for (size_t edge = 0; edge < corners.size(); ++edge)
        {
            if (SegmentsIntersect(
                    start,
                    end,
                    corners[edge],
                    corners[(edge + 1) % corners.size()]))
            {
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool DownTool::ValidateWgs84Polygons(
    const Wgs84Polygons& polygons, std::string& error)
{
    if (polygons.empty())
    {
        error = "KML 中没有可用的多边形。";
        return false;
    }
    for (const Wgs84Ring& ring : polygons)
    {
        if (ring.size() < 3 || CountDistinctPoints(ring) < 3)
        {
            error = "KML 多边形至少需要三个不同坐标点。";
            return false;
        }
        if (!std::ranges::all_of(ring, IsFiniteCoordinate))
        {
            error = "KML 多边形包含无效的 WGS84 经纬度。";
            return false;
        }
    }

    const std::optional<GeographicBounds> bounds =
        CalculateGeographicBounds(polygons);
    if (!bounds.has_value())
    {
        error = "无法计算 KML 多边形范围。";
        return false;
    }
    if (bounds->east - bounds->west > 180.)
    {
        error = "暂不支持跨越 180 度经线的 KML 多边形。";
        return false;
    }
    return true;
}

std::optional<DownTool::GeographicBounds>
DownTool::CalculateGeographicBounds(const Wgs84Polygons& polygons)
{
    GeographicBounds bounds;
    bounds.west = std::numeric_limits<double>::infinity();
    bounds.east = -std::numeric_limits<double>::infinity();
    bounds.south = std::numeric_limits<double>::infinity();
    bounds.north = -std::numeric_limits<double>::infinity();
    for (const Wgs84Ring& ring : polygons)
    {
        for (const LonLat& coordinate : ring)
        {
            if (!IsFiniteCoordinate(coordinate))
            {
                return std::nullopt;
            }
            bounds.west = std::min(bounds.west, coordinate.lon);
            bounds.east = std::max(bounds.east, coordinate.lon);
            bounds.south = std::min(bounds.south, coordinate.lat);
            bounds.north = std::max(bounds.north, coordinate.lat);
        }
    }
    if (!std::isfinite(bounds.west) || bounds.west > bounds.east ||
        bounds.south > bounds.north)
    {
        return std::nullopt;
    }
    return bounds;
}

std::optional<DownTool::GeographicBounds>
DownTool::CalculateGeographicBounds(
    const LonLat& centerWgs84, const double radiusMeters)
{
    if (!IsFiniteCoordinate(centerWgs84) ||
        centerWgs84.lon < -180. || centerWgs84.lon > 180. ||
        centerWgs84.lat < -90. || centerWgs84.lat > 90. ||
        !std::isfinite(radiusMeters) || radiusMeters <= 0.)
    {
        return std::nullopt;
    }

    GeographicBounds bounds;
    GeographicLib::Math::real unused;
    const GeographicLib::Geodesic& geodesic =
        GeographicLib::Geodesic::WGS84();
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        0.,
        radiusMeters,
        bounds.north,
        unused);
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        180.,
        radiusMeters,
        bounds.south,
        unused);
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        90.,
        radiusMeters,
        unused,
        bounds.east);
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        270.,
        radiusMeters,
        unused,
        bounds.west);

    if (bounds.west > bounds.east)
    {
        bounds.west = -180.;
        bounds.east = 180.;
    }
    return bounds;
}

DownTool::LonLat DownTool::GeographicBoundsCenter(
    const GeographicBounds& bounds)
{
    return {
        (bounds.west + bounds.east) * 0.5,
        (bounds.south + bounds.north) * 0.5
    };
}

double DownTool::GeographicBoundsRadiusMeters(
    const GeographicBounds& bounds, const LonLat& center)
{
    const GeographicLib::Geodesic& geodesic =
        GeographicLib::Geodesic::WGS84();
    const std::array<LonLat, 4> corners = {{
        {bounds.west, bounds.north},
        {bounds.east, bounds.north},
        {bounds.east, bounds.south},
        {bounds.west, bounds.south}
    }};
    double radius = 0.;
    for (const LonLat& corner : corners)
    {
        double distance = 0.;
        geodesic.Inverse(
            center.lat,
            center.lon,
            corner.lat,
            corner.lon,
            distance);
        radius = std::max(radius, distance);
    }
    return radius;
}

bool DownTool::GeographicPolygonsIntersectBounds(
    const Wgs84Polygons& polygons, const GeographicBounds& bounds)
{
    if (polygons.empty())
    {
        return true;
    }
    return std::ranges::any_of(
        polygons,
        [&bounds](const Wgs84Ring& ring)
        {
            return ring.size() >= 3 && RingIntersectsBounds(ring, bounds);
        });
}
