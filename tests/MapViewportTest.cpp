#include "PostProcessTestSupport.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "BaiduDown.h"
#include "GaoDeDown.h"
#include "GoogleDownBase.h"
#include "preview/MapViewport.h"

namespace
{
constexpr double kCenterScreenX = 512.;
constexpr double kCenterScreenY = 396.;

bool isNear(const DownTool::LonLat& left, const DownTool::LonLat& right, double tolerance)
{
    return std::abs(left.lon - right.lon) <= tolerance &&
           std::abs(left.lat - right.lat) <= tolerance;
}

bool testProjectionInteraction(DownTool::DownToolBase& projection,
                               DomDownToolPreview::TileYAxisDirection yDirection, double tolerance,
                               const std::string& projectionName)
{
    const DownTool::LonLat beijing(116.397389, 39.908722);
    DownTool::Tile directTile;
    double directPixelX = 0.;
    double directPixelY = 0.;
    DownTool::LonLat directRoundTrip;
    const bool directProjected =
        projection.TryProjectToSourceTile(beijing, 15, directTile, directPixelX, directPixelY);
    const bool directUnprojected =
        directProjected && projection.TryUnprojectFromSourceTile(directTile, directPixelX,
                                                                 directPixelY, directRoundTrip);
    if (!postprocess_test::Require(directUnprojected,
                                   projectionName + " direct projection round trip failed"))
    {
        return false;
    }

    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1024, 768, {52., 28.});
    viewport.setCenter(beijing);
    viewport.setZoom(15.35);

    const std::optional<DownTool::LonLat> centerCoordinate =
        viewport.lonLatAt(kCenterScreenX, kCenterScreenY, projection, yDirection);
    if (!postprocess_test::Require(
            centerCoordinate.has_value() && isNear(centerCoordinate.value(), beijing, tolerance),
            projectionName + " center coordinate did not round trip: " +
                (centerCoordinate.has_value() ? std::to_string(centerCoordinate->lon) + "," +
                                                    std::to_string(centerCoordinate->lat)
                                              : "unavailable")))
    {
        return false;
    }

    const std::optional<DomDownToolPreview::ViewportPoint> centerPoint =
        viewport.screenPointForLonLat(beijing, projection, yDirection);
    if (!postprocess_test::Require(
            centerPoint.has_value() && std::abs(centerPoint->x - kCenterScreenX) < 0.001 &&
                std::abs(centerPoint->y - kCenterScreenY) < 0.001,
            projectionName + " center screen projection failed"))
    {
        return false;
    }

    constexpr double kAnchorX = 723.;
    constexpr double kAnchorY = 331.;
    const std::optional<DownTool::LonLat> anchorBefore =
        viewport.lonLatAt(kAnchorX, kAnchorY, projection, yDirection);
    if (!postprocess_test::Require(anchorBefore.has_value(),
                                   projectionName + " zoom anchor was unavailable"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            viewport.zoomAt(kAnchorX, kAnchorY, 0.7, 2., 20., projection, yDirection),
            projectionName + " zoom operation failed"))
    {
        return false;
    }

    const std::optional<DownTool::LonLat> anchorAfter =
        viewport.lonLatAt(kAnchorX, kAnchorY, projection, yDirection);
    if (!postprocess_test::Require(anchorAfter.has_value() &&
                                       isNear(anchorAfter.value(), anchorBefore.value(), tolerance),
                                   projectionName + " zoom anchor moved"))
    {
        return false;
    }

    const DownTool::LonLat centerBeforePan = viewport.center();
    if (!postprocess_test::Require(viewport.panBy(80., -45., projection, yDirection),
                                   projectionName + " pan operation failed") ||
        !postprocess_test::Require(!isNear(viewport.center(), centerBeforePan, 0.000001),
                                   projectionName + " pan did not change the center"))
    {
        return false;
    }

    const std::vector<DomDownToolPreview::VisibleTile> tiles =
        viewport.visibleTiles(projection, yDirection);
    return postprocess_test::Require(!tiles.empty() && tiles.size() < 100,
                                     projectionName + " visible tile range is invalid");
}

bool testStandardTileSelection()
{
    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);

    DownTool::LonLat tileCenter;
    if (!postprocess_test::Require(
            projection.TryUnprojectFromSourceTile(DownTool::Tile(2, 2, 2), 128., 128., tileCenter),
            "standard XYZ tile center projection failed"))
    {
        return false;
    }

    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1, 1);
    viewport.setCenter(tileCenter);
    viewport.setZoom(2.);
    const std::vector<DomDownToolPreview::VisibleTile> tiles =
        viewport.visibleTiles(projection, DomDownToolPreview::TileYAxisDirection::Down, 256, 0);
    if (!postprocess_test::Require(tiles.size() == 1 && tiles.front().cacheTile.X == 2 &&
                                       tiles.front().cacheTile.Y == 2 &&
                                       tiles.front().cacheTile.Zoom == 2,
                                   "standard XYZ tile selection is incorrect"))
    {
        return false;
    }

    const std::filesystem::path tilePath =
        DomDownToolPreview::resolveTilePath("cache/provider", tiles.front().cacheTile);
    return postprocess_test::Require(tilePath.generic_string() == "cache/provider/2/2/2.png",
                                     "preview cache path does not match downloader layout");
}

bool testBaiduTileSelection()
{
    DownTool::BaiduDown projection;
    projection.SetTileSize(256);
    const DownTool::LonLat center(116.397389, 39.908722);
    DownTool::Tile projectedTile;
    double pixelX = 0.;
    double pixelY = 0.;
    if (!postprocess_test::Require(
            projection.TryProjectToSourceTile(center, 16, projectedTile, pixelX, pixelY),
            "Baidu center projection failed"))
    {
        return false;
    }

    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1, 1);
    viewport.setCenter(center);
    viewport.setZoom(16.);
    const std::vector<DomDownToolPreview::VisibleTile> tiles =
        viewport.visibleTiles(projection, DomDownToolPreview::TileYAxisDirection::Up, 256, 0);
    const auto matchingTile =
        std::find_if(tiles.begin(), tiles.end(),
                     [&projectedTile](const DomDownToolPreview::VisibleTile& visibleTile)
                     {
                         return visibleTile.cacheTile.X == projectedTile.X &&
                                visibleTile.cacheTile.Y == projectedTile.Y;
                     });
    return postprocess_test::Require(matchingTile != tiles.end(),
                                     "Baidu upward Y tile selection is incorrect");
}

bool runTest()
{
    if (!testStandardTileSelection() || !testBaiduTileSelection())
    {
        return false;
    }

    DownTool::GoogleDownBase standardProjection;
    standardProjection.SetTileSize(256);
    if (!testProjectionInteraction(standardProjection, DomDownToolPreview::TileYAxisDirection::Down,
                                   0.000001, "standard"))
    {
        return false;
    }

    DownTool::GaoDeDown gcjProjection;
    gcjProjection.SetTileSize(256);
    if (!testProjectionInteraction(gcjProjection, DomDownToolPreview::TileYAxisDirection::Down,
                                   0.0003, "GCJ02"))
    {
        return false;
    }

    DownTool::BaiduDown baiduProjection;
    baiduProjection.SetTileSize(256);
    if (!testProjectionInteraction(baiduProjection, DomDownToolPreview::TileYAxisDirection::Up,
                                   0.0003, "Baidu"))
    {
        return false;
    }

    std::cout << "MapViewportTest passed\n";
    return true;
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(runTest);
}
