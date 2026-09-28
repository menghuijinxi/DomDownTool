#include "AmapVectorTileGrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

#include "GeographicLib/Geodesic.hpp"

#include "GCJ02GoogleDownBase.h"

namespace
{
    constexpr uint32_t kMaximumAmapVectorZoom = 30;

    double NormalizeLongitude(double longitude)
    {
        while (longitude < -180.)
        {
            longitude += 360.;
        }
        while (longitude > 180.)
        {
            longitude -= 360.;
        }
        return longitude;
    }

    void ValidateLonLat(const DownTool::LonLat& lonLat)
    {
        if (!std::isfinite(lonLat.lon) || !std::isfinite(lonLat.lat) ||
            lonLat.lon < -180. || lonLat.lon > 180. ||
            lonLat.lat < -90. || lonLat.lat > 90.)
        {
            throw std::invalid_argument("高德矢量瓦片坐标必须是有效的经纬度。");
        }
    }

    uint32_t TileIndex(double normalizedValue, uint64_t tileCount)
    {
        const double index = std::floor(normalizedValue * static_cast<double>(tileCount));
        if (index <= 0.)
        {
            return 0;
        }
        if (index >= static_cast<double>(tileCount))
        {
            return static_cast<uint32_t>(tileCount - 1);
        }
        return static_cast<uint32_t>(index);
    }

    void AppendTileRange(
        std::vector<DownTool::AmapVectorTile>& tiles,
        uint32_t minX,
        uint32_t maxX,
        uint32_t minY,
        uint32_t maxY,
        uint32_t zoom)
    {
        for (uint32_t y = minY; y <= maxY; y++)
        {
            for (uint32_t x = minX; x <= maxX; x++)
            {
                tiles.push_back({ x, y, zoom });
            }
        }
    }
}

uint32_t DownTool::AmapVectorTileGrid::ResolveDataZoom(uint32_t renderZoom)
{
    // 高德在线底图用离散 PBF 层级服务连续显示层级。
    if (renderZoom > kMaximumAmapVectorZoom)
    {
        throw std::invalid_argument("高德矢量瓦片缩放级别超出支持范围。");
    }
    if (renderZoom <= 5)
    {
        return 3;
    }
    if (renderZoom <= 7)
    {
        return 6;
    }
    if (renderZoom <= 9)
    {
        return 8;
    }
    if (renderZoom <= 12)
    {
        return 10;
    }
    if (renderZoom <= 14)
    {
        return 12;
    }
    return 14;
}

std::vector<DownTool::AmapVectorTile> DownTool::AmapVectorTileGrid::SelectTilesForWgs84Radius(
    const LonLat& centerWgs84,
    double radiusMeters,
    uint32_t zoom)
{
    ValidateLonLat(centerWgs84);
    if (!std::isfinite(radiusMeters) || radiusMeters < 0.)
    {
        throw std::invalid_argument("高德矢量下载半径必须是非负有限米数。");
    }
    if (zoom > kMaximumAmapVectorZoom)
    {
        throw std::invalid_argument("高德矢量瓦片缩放级别超出支持范围。");
    }

    GeographicLib::Math::real ignoredLongitude = 0.;
    GeographicLib::Math::real ignoredLatitude = 0.;
    const GeographicLib::Geodesic& geodesic = GeographicLib::Geodesic::WGS84();

    LonLat northWgs84 = centerWgs84;
    LonLat southWgs84 = centerWgs84;
    LonLat eastWgs84 = centerWgs84;
    LonLat westWgs84 = centerWgs84;
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        0.,
        radiusMeters,
        northWgs84.lat,
        ignoredLongitude);
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        180.,
        radiusMeters,
        southWgs84.lat,
        ignoredLongitude);
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        90.,
        radiusMeters,
        ignoredLatitude,
        eastWgs84.lon);
    geodesic.Direct(
        centerWgs84.lat,
        centerWgs84.lon,
        270.,
        radiusMeters,
        ignoredLatitude,
        westWgs84.lon);

    northWgs84.lon = NormalizeLongitude(northWgs84.lon);
    southWgs84.lon = NormalizeLongitude(southWgs84.lon);
    eastWgs84.lon = NormalizeLongitude(eastWgs84.lon);
    westWgs84.lon = NormalizeLongitude(westWgs84.lon);

    const LonLat northGcj02 = GCJ02GoogleDownBase::WGS84ToGCJ02(northWgs84);
    const LonLat southGcj02 = GCJ02GoogleDownBase::WGS84ToGCJ02(southWgs84);
    const LonLat eastGcj02 = GCJ02GoogleDownBase::WGS84ToGCJ02(eastWgs84);
    const LonLat westGcj02 = GCJ02GoogleDownBase::WGS84ToGCJ02(westWgs84);

    const double northLatitude = std::max(northGcj02.lat, southGcj02.lat);
    const double southLatitude = std::min(northGcj02.lat, southGcj02.lat);
    const AmapVectorTile northWest = LonLatToTile(
        { NormalizeLongitude(westGcj02.lon), northLatitude }, zoom);
    const AmapVectorTile southEast = LonLatToTile(
        { NormalizeLongitude(eastGcj02.lon), southLatitude }, zoom);

    const uint64_t tileCount = uint64_t{ 1 } << zoom;
    const uint64_t rowCount = static_cast<uint64_t>(southEast.y) - northWest.y + 1;
    const uint64_t columnCount = northWest.x <= southEast.x ?
        static_cast<uint64_t>(southEast.x) - northWest.x + 1 :
        tileCount - northWest.x + southEast.x + 1;

    std::vector<AmapVectorTile> tiles;
    tiles.reserve(static_cast<size_t>(rowCount * columnCount));
    if (northWest.x <= southEast.x)
    {
        AppendTileRange(
            tiles, northWest.x, southEast.x, northWest.y, southEast.y, zoom);
    }
    else
    {
        AppendTileRange(
            tiles,
            northWest.x,
            static_cast<uint32_t>(tileCount - 1),
            northWest.y,
            southEast.y,
            zoom);
        AppendTileRange(tiles, 0, southEast.x, northWest.y, southEast.y, zoom);
    }

    return tiles;
}

std::vector<DownTool::AmapVectorTile> DownTool::AmapVectorTileGrid::SelectTilesForWgs84Bounds(
    const LonLat& northWestWgs84,
    const LonLat& southEastWgs84,
    uint32_t zoom)
{
    ValidateLonLat(northWestWgs84);
    ValidateLonLat(southEastWgs84);
    if (northWestWgs84.lon > southEastWgs84.lon ||
        northWestWgs84.lat < southEastWgs84.lat)
    {
        throw std::invalid_argument("高德矢量下载边界必须由西北角和东南角组成。");
    }
    if (zoom > kMaximumAmapVectorZoom)
    {
        throw std::invalid_argument("高德矢量瓦片缩放级别超出支持范围。");
    }

    const std::array<LonLat, 4> cornersWgs84 = {{
        northWestWgs84,
        { southEastWgs84.lon, northWestWgs84.lat },
        { northWestWgs84.lon, southEastWgs84.lat },
        southEastWgs84
    }};
    std::array<LonLat, 4> cornersGcj02;
    std::transform(
        cornersWgs84.begin(),
        cornersWgs84.end(),
        cornersGcj02.begin(),
        [](const LonLat& corner)
        {
            return GCJ02GoogleDownBase::WGS84ToGCJ02(corner);
        });

    double minimumLongitude = cornersGcj02.front().lon;
    double maximumLongitude = cornersGcj02.front().lon;
    double minimumLatitude = cornersGcj02.front().lat;
    double maximumLatitude = cornersGcj02.front().lat;
    for (const LonLat& corner : cornersGcj02)
    {
        minimumLongitude = std::min(minimumLongitude, corner.lon);
        maximumLongitude = std::max(maximumLongitude, corner.lon);
        minimumLatitude = std::min(minimumLatitude, corner.lat);
        maximumLatitude = std::max(maximumLatitude, corner.lat);
    }

    const AmapVectorTile northWest = LonLatToTile(
        { minimumLongitude, maximumLatitude }, zoom);
    const AmapVectorTile southEast = LonLatToTile(
        { maximumLongitude, minimumLatitude }, zoom);
    std::vector<AmapVectorTile> tiles;
    tiles.reserve(
        static_cast<size_t>(southEast.x - northWest.x + 1) *
        static_cast<size_t>(southEast.y - northWest.y + 1));
    AppendTileRange(
        tiles,
        northWest.x,
        southEast.x,
        northWest.y,
        southEast.y,
        zoom);
    return tiles;
}

std::vector<DownTool::AmapVectorTile>
DownTool::AmapVectorTileGrid::SelectTilesForWgs84Polygons(
    const Wgs84Polygons& polygonsWgs84,
    uint32_t zoom)
{
    std::string validationError;
    if (!ValidateWgs84Polygons(polygonsWgs84, validationError))
    {
        throw std::invalid_argument(validationError);
    }
    const std::optional<GeographicBounds> polygonBounds =
        CalculateGeographicBounds(polygonsWgs84);
    if (!polygonBounds.has_value())
    {
        throw std::invalid_argument("无法计算高德矢量多边形范围。");
    }

    const std::vector<AmapVectorTile> candidates = SelectTilesForWgs84Bounds(
        {polygonBounds->west, polygonBounds->north},
        {polygonBounds->east, polygonBounds->south},
        zoom);
    std::vector<AmapVectorTile> selected;
    selected.reserve(candidates.size());
    for (const AmapVectorTile& tile : candidates)
    {
        const LonLat northWestGcj02 = TileNorthWestToLonLat(tile);
        const LonLat southEastGcj02 = TileSouthEastToLonLat(tile);
        const std::array<LonLat, 4> cornersWgs84 = {{
            GCJ02GoogleDownBase::GCJ02ToWGS84(northWestGcj02),
            GCJ02GoogleDownBase::GCJ02ToWGS84(
                {southEastGcj02.lon, northWestGcj02.lat}),
            GCJ02GoogleDownBase::GCJ02ToWGS84(southEastGcj02),
            GCJ02GoogleDownBase::GCJ02ToWGS84(
                {northWestGcj02.lon, southEastGcj02.lat})
        }};
        GeographicBounds tileBounds{
            cornersWgs84.front().lon,
            cornersWgs84.front().lon,
            cornersWgs84.front().lat,
            cornersWgs84.front().lat
        };
        for (const LonLat& corner : cornersWgs84)
        {
            tileBounds.west = std::min(tileBounds.west, corner.lon);
            tileBounds.east = std::max(tileBounds.east, corner.lon);
            tileBounds.south = std::min(tileBounds.south, corner.lat);
            tileBounds.north = std::max(tileBounds.north, corner.lat);
        }
        if (GeographicPolygonsIntersectBounds(polygonsWgs84, tileBounds))
        {
            selected.push_back(tile);
        }
    }
    return selected;
}

DownTool::AmapVectorTile DownTool::AmapVectorTileGrid::LonLatToTile(
    const LonLat& gcj02LonLat,
    uint32_t zoom)
{
    ValidateLonLat(gcj02LonLat);
    if (zoom > kMaximumAmapVectorZoom)
    {
        throw std::invalid_argument("高德矢量瓦片缩放级别超出支持范围。");
    }

    const uint64_t tileCount = uint64_t{ 1 } << zoom;
    const double longitude = NormalizeLongitude(gcj02LonLat.lon);
    const double latitude = std::clamp(
        gcj02LonLat.lat,
        -90.,
        std::nextafter(90., -std::numeric_limits<double>::infinity()));
    return {
        TileIndex((longitude + 180.) / 360., tileCount),
        TileIndex((90. - latitude) / 180., tileCount),
        zoom
    };
}

DownTool::LonLat DownTool::AmapVectorTileGrid::TileNorthWestToLonLat(
    const AmapVectorTile& tile)
{
    if (tile.zoom > kMaximumAmapVectorZoom)
    {
        throw std::invalid_argument("高德矢量瓦片缩放级别超出支持范围。");
    }

    const uint64_t tileCount = uint64_t{ 1 } << tile.zoom;
    if (tile.x >= tileCount || tile.y >= tileCount)
    {
        throw std::invalid_argument("高德矢量瓦片坐标超出缩放级别范围。");
    }

    return {
        static_cast<double>(tile.x) * 360. / static_cast<double>(tileCount) - 180.,
        90. - static_cast<double>(tile.y) * 180. / static_cast<double>(tileCount)
    };
}

DownTool::LonLat DownTool::AmapVectorTileGrid::TileSouthEastToLonLat(
    const AmapVectorTile& tile)
{
    if (tile.zoom > kMaximumAmapVectorZoom)
    {
        throw std::invalid_argument("高德矢量瓦片缩放级别超出支持范围。");
    }

    const uint64_t tileCount = uint64_t{ 1 } << tile.zoom;
    if (tile.x >= tileCount || tile.y >= tileCount)
    {
        throw std::invalid_argument("高德矢量瓦片坐标超出缩放级别范围。");
    }

    return {
        static_cast<double>(tile.x + 1) * 360. / static_cast<double>(tileCount) - 180.,
        90. - static_cast<double>(tile.y + 1) * 180. / static_cast<double>(tileCount)
    };
}
