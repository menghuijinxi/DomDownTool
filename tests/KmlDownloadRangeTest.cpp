#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "GeographicPolygon.h"
#include "AmapVectorTileGrid.h"
#include "GoogleDownBase.h"
#include "KmlDownloadRange.h"

namespace
{
bool Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << message << "\n";
    }
    return condition;
}

bool NearlyEqual(double left, double right, double tolerance = 1.e-9)
{
    return std::abs(left - right) <= tolerance;
}

class InspectableGoogleDown final : public DownTool::GoogleDownBase
{
  public:
    std::vector<DownTool::Tile> BuildSelectedTiles()
    {
        CalculateDownRange();
        std::vector<DownTool::Tile> tiles;
        BuildRequestedTiles(tiles);
        return tiles;
    }
};

bool TestKmlParsing(const std::filesystem::path& testRoot)
{
    const std::filesystem::path kmlPath =
        testRoot / std::filesystem::u8path(u8"范围.kml");
    std::ofstream output(kmlPath, std::ios::binary | std::ios::trunc);
    output << R"xml(<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
  <Document>
    <Placemark>
      <Polygon>
        <outerBoundaryIs>
          <LinearRing>
            <coordinates>
              116.1,39.9,0 116.2,39.9,10 116.2,40.0,0
              116.1,40.0,0 116.1,39.9,0
            </coordinates>
          </LinearRing>
        </outerBoundaryIs>
      </Polygon>
    </Placemark>
    <Placemark>
      <MultiGeometry>
        <Polygon>
          <outerBoundaryIs>
            <LinearRing>
              <coordinates>
                116.3,39.8 116.4,39.8 116.35,39.85 116.3,39.8
              </coordinates>
            </LinearRing>
          </outerBoundaryIs>
        </Polygon>
      </MultiGeometry>
    </Placemark>
  </Document>
</kml>)xml";
    output.close();

    std::string error;
    const std::optional<DownTool::KmlDownloadRange> range =
        DownTool::LoadKmlDownloadRange(kmlPath, error);
    if (!Require(range.has_value(), "无法解析合法 KML：" + error))
    {
        return false;
    }
    if (!Require(
            range->polygonsWgs84.size() == 2 &&
                range->polygonsWgs84[0].size() == 4 &&
                range->polygonsWgs84[1].size() == 3,
            "KML 多边形或闭合重复点解析错误") ||
        !Require(
            NearlyEqual(range->bounds.west, 116.1) &&
                NearlyEqual(range->bounds.east, 116.4) &&
                NearlyEqual(range->bounds.south, 39.8) &&
                NearlyEqual(range->bounds.north, 40.0),
            "KML 地理边界计算错误") ||
        !Require(
            range->fileName == std::string(
                reinterpret_cast<const char*>(u8"范围.kml"),
                sizeof(u8"范围.kml") - 1),
            "KML 中文文件名没有按 UTF-8 保存") ||
        !Require(range->boundingRadiusMeters > 0.,
                 "KML 包围半径没有计算"))
    {
        return false;
    }

    const std::filesystem::path invalidPath = testRoot / "no-polygon.kml";
    std::ofstream invalidOutput(
        invalidPath, std::ios::binary | std::ios::trunc);
    invalidOutput << R"xml(<kml><Placemark><LineString><coordinates>
        116.1,39.9 116.2,40.0
    </coordinates></LineString></Placemark></kml>)xml";
    invalidOutput.close();
    error.clear();
    return Require(
        !DownTool::LoadKmlDownloadRange(invalidPath, error).has_value() &&
            !error.empty(),
        "不含 Polygon 的 KML 没有被拒绝");
}

bool TestPolygonIntersection()
{
    const DownTool::Wgs84Polygons polygons = {{
        {0., 0.},
        {4., 0.},
        {2., 4.}
    }};
    return Require(
               DownTool::GeographicPolygonsIntersectBounds(
                   polygons, {1.8, 2.2, 1.5, 2.0}),
               "多边形包含矩形时未检测到相交") &&
        Require(
            DownTool::GeographicPolygonsIntersectBounds(
                polygons, {-1., 1., -1., 1.}),
            "矩形包含多边形顶点时未检测到相交") &&
        Require(
            DownTool::GeographicPolygonsIntersectBounds(
                polygons, {3.8, 4.2, -0.2, 0.2}),
            "多边形边穿过矩形时未检测到相交") &&
        Require(
            !DownTool::GeographicPolygonsIntersectBounds(
                polygons, {5., 6., 5., 6.}),
            "分离的多边形和矩形被误判为相交") &&
        []()
        {
            std::string error;
            return Require(
                !DownTool::ValidateWgs84Polygons(
                    {{{0., 0.}, {1., 1.}, {0., 0.}}}, error),
                "少于三个不同坐标点的 KML 多边形没有被拒绝");
        }();
}

bool TestTileSelectionFollowsPolygon()
{
    constexpr uint32_t kZoom = 4;
    const DownTool::Wgs84Polygons polygons = {{
        DownTool::GoogleDownBase::TileToLonLat({8, 6, kZoom}),
        DownTool::GoogleDownBase::TileToLonLat({12, 6, kZoom}),
        DownTool::GoogleDownBase::TileToLonLat({8, 10, kZoom})
    }};

    InspectableGoogleDown downloader;
    downloader.SetTileSize(256);
    downloader.SetZoom(kZoom);
    downloader.SetDownloadPolygonsWgs84(polygons);
    const std::vector<DownTool::Tile> tiles = downloader.BuildSelectedTiles();
    const DownTool::Tile minTile = downloader.GetMinTile();
    const DownTool::Tile maxTile = downloader.GetMaxTile();
    const size_t boundingTileCount =
        static_cast<size_t>(maxTile.X - minTile.X + 1) *
        static_cast<size_t>(maxTile.Y - minTile.Y + 1);

    const bool containsNorthWest = std::ranges::any_of(
        tiles,
        [](const DownTool::Tile& tile)
        {
            return tile.X == 8 && tile.Y == 6;
        });
    const bool containsSouthEast = std::ranges::any_of(
        tiles,
        [](const DownTool::Tile& tile)
        {
            return tile.X == 12 && tile.Y == 10;
        });
    if (!Require(
            !tiles.empty() && tiles.size() < boundingTileCount &&
                containsNorthWest && !containsSouthEast,
            "XYZ 瓦片仍按 KML 外接矩形整块选择"))
    {
        return false;
    }

    return Require(
        std::ranges::all_of(
            tiles,
            [&polygons](const DownTool::Tile& tile)
            {
                const DownTool::LonLat northWest =
                    DownTool::GoogleDownBase::TileToLonLat(tile);
                const DownTool::LonLat southEast =
                    DownTool::GoogleDownBase::TileToLonLat(
                        {tile.X + 1, tile.Y + 1, tile.Zoom});
                return DownTool::GeographicPolygonsIntersectBounds(
                    polygons,
                    {
                        northWest.lon,
                        southEast.lon,
                        southEast.lat,
                        northWest.lat
                    });
            }),
        "选择结果中包含与 KML 多边形不相交的 XYZ 瓦片");
}

bool TestSeparatedAndOverlappingRegions()
{
    constexpr uint32_t kZoom = 20;
    const DownTool::Wgs84Ring west = {
        {116.1, 39.9}, {116.1001, 39.9}, {116.1001, 39.9001}, {116.1, 39.9001}};
    const DownTool::Wgs84Ring east = {
        {120.1, 30.9}, {120.1001, 30.9}, {120.1001, 30.9001}, {120.1, 30.9001}};
    const DownTool::Wgs84Polygons polygons = {west, east, west};
    InspectableGoogleDown downloader;
    downloader.SetTileSize(256);
    downloader.SetZoom(kZoom);
    downloader.SetDownloadPolygonsWgs84(polygons);
    const auto tiles = downloader.BuildSelectedTiles();
    std::set<std::pair<int32_t, int32_t>> uniqueTiles;
    for (const auto& tile : tiles)
    {
        uniqueTiles.emplace(tile.X, tile.Y);
    }
    const auto gisTiles = DownTool::AmapVectorTileGrid::SelectTilesForWgs84Polygons(
        polygons, kZoom);
    std::set<std::pair<uint32_t, uint32_t>> uniqueGisTiles;
    for (const auto& tile : gisTiles)
    {
        uniqueGisTiles.emplace(tile.x, tile.y);
    }
    return Require(tiles.size() >= 2 && tiles.size() < 20 &&
            uniqueTiles.size() == tiles.size(), "独立下载区间仍整块枚举，或重叠区域重复下载") &&
        Require(gisTiles.size() >= 2 && gisTiles.size() < 20 &&
            uniqueGisTiles.size() == gisTiles.size(), "GIS 多区域选片未保持稀疏或出现重复");
}

bool RunTest()
{
    const std::filesystem::path testRoot =
        std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) /
        "tmp" / "kml-download-range-test";
    std::error_code error;
    std::filesystem::remove_all(testRoot, error);
    error.clear();
    std::filesystem::create_directories(testRoot, error);
    if (!Require(!error, "无法创建 KML 测试目录"))
    {
        return false;
    }

    const bool result = TestKmlParsing(testRoot) &&
        TestPolygonIntersection() &&
        TestTileSelectionFollowsPolygon() &&
        TestSeparatedAndOverlappingRegions();
    std::filesystem::remove_all(testRoot, error);
    if (result)
    {
        std::cout << "KmlDownloadRangeTest passed\n";
    }
    return result;
}
} // namespace

int main()
{
    try
    {
        return RunTest() ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << "\n";
        return 1;
    }
}
