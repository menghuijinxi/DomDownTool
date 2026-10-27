#include "PostProcessTestSupport.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "BaiduDown.h"
#include "GaoDeDown.h"
#include "GoogleDownBase.h"
#include "TianDiTuMultidateDown.h"
#include "preview/MapHoverRefreshThrottle.h"
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

    const std::optional<DomDownToolPreview::ViewportTransform> transform =
        viewport.makeTransform(projection, yDirection);
    const std::optional<DomDownToolPreview::ViewportSourcePoint> sourcePoint =
        transform.has_value() ? transform->sourcePointForLonLat(beijing) : std::nullopt;
    const DomDownToolPreview::ViewportPoint transformedPoint =
        sourcePoint.has_value() ? transform->screenPointForSourcePoint(*sourcePoint)
                                : DomDownToolPreview::ViewportPoint{};
    const std::optional<DomDownToolPreview::ViewportGeoBounds> bounds =
        viewport.visibleGeoBounds(projection, yDirection);
    if (!postprocess_test::Require(
            transform.has_value() && sourcePoint.has_value() &&
                std::abs(transformedPoint.x - centerPoint->x) < 0.001 &&
                std::abs(transformedPoint.y - centerPoint->y) < 0.001,
            projectionName + " reusable viewport transform does not match direct projection") ||
        !postprocess_test::Require(
            bounds.has_value() && bounds->minLon <= beijing.lon &&
                bounds->maxLon >= beijing.lon && bounds->minLat <= beijing.lat &&
                bounds->maxLat >= beijing.lat,
            projectionName + " visible geographic bounds do not contain the center"))
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
            viewport.zoomAt(kAnchorX, kAnchorY, 0.7, projection, yDirection),
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

bool testTianDiTuMultidateTileSelection()
{
    DownTool::TianDiTuMultidateDown projection;
    projection.SetTileSize(256);

    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1024, 768, {52., 28.});
    viewport.setCenter({0., -84.});
    viewport.setZoom(3.);
    const std::vector<DomDownToolPreview::VisibleTile> tiles =
        viewport.visibleTiles(projection, DomDownToolPreview::TileYAxisDirection::Down, 256, 1);
    const bool rowsInMatrix = std::ranges::all_of(
        tiles,
        [](const DomDownToolPreview::VisibleTile& tile)
        {
            return tile.cacheTile.Y >= 0 && tile.cacheTile.Y < 4 &&
                tile.cacheTile.X >= 0 && tile.cacheTile.X < 8;
        });

    return postprocess_test::Require(!tiles.empty() && rowsInMatrix,
                                     "TianDiTu multidate visible tiles exceeded its geographic matrix");
}

bool testViewSpanShowsMoreTilesWithoutChangingSourceZoom()
{
    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);

    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1024, 768, {52., 28.});
    viewport.setCenter({116.397389, 39.908722});
    viewport.setZoom(15.);
    const uint32_t sourceZoom = viewport.sourceZoom();
    const std::vector<DomDownToolPreview::VisibleTile> normalTiles = viewport.visibleTiles(
        projection, DomDownToolPreview::TileYAxisDirection::Down, 256, 0);

    viewport.SetViewSpan(2.);
    const std::vector<DomDownToolPreview::VisibleTile> expandedTiles = viewport.visibleTiles(
        projection, DomDownToolPreview::TileYAxisDirection::Down, 256, 0);
    const bool sameSourceZoom = std::all_of(
        expandedTiles.begin(), expandedTiles.end(),
        [sourceZoom](const DomDownToolPreview::VisibleTile& tile)
        { return tile.cacheTile.Zoom == sourceZoom; });

    viewport.SetViewSpan(100.);
    const bool maximumClamped = viewport.GetViewSpan() == 4.;
    viewport.SetViewSpan(0.);
    const bool minimumClamped = viewport.GetViewSpan() == 0.1;

    return postprocess_test::Require(!normalTiles.empty() &&
                                         expandedTiles.size() > normalTiles.size(),
                                     "expanded view span did not show more tiles") &&
        postprocess_test::Require(sourceZoom == 15 && viewport.sourceZoom() == sourceZoom &&
                                      sameSourceZoom,
                                  "view span changed the source tile zoom") &&
        postprocess_test::Require(expandedTiles.front().size < normalTiles.front().size,
                                  "expanded view span did not reduce tile display size") &&
        postprocess_test::Require(maximumClamped && minimumClamped,
                                  "view span limits were not enforced");
}

bool testMapHoverRefreshThrottle()
{
    using namespace std::chrono_literals;
    using Clock = DomDownToolPreview::MapHoverRefreshThrottle::Clock;

    DomDownToolPreview::MapHoverRefreshThrottle throttle(50ms);
    const Clock::time_point start{};
    const bool firstRefresh = throttle.shouldRefresh(start);
    const bool earlyRefresh = throttle.shouldRefresh(start + 49ms);
    const bool intervalRefresh = throttle.shouldRefresh(start + 50ms);
    const bool secondEarlyRefresh = throttle.shouldRefresh(start + 99ms);
    const bool secondIntervalRefresh = throttle.shouldRefresh(start + 100ms);
    throttle.reset();
    const bool refreshAfterReset = throttle.shouldRefresh(start + 101ms);

    return postprocess_test::Require(firstRefresh && !earlyRefresh && intervalRefresh &&
                                         !secondEarlyRefresh && secondIntervalRefresh &&
                                         refreshAfterReset,
                                     "map hover UI refresh throttle did not enforce its interval");
}

bool testHighZoomAndFractionalViewSpan()
{
    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1024, 768);
    viewport.setZoom(30.);
    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);
    const auto tiles =
        viewport.visibleTiles(projection, DomDownToolPreview::TileYAxisDirection::Down);
    const bool highZoomAccepted =
        viewport.zoom() == 30. && !tiles.empty() && tiles.front().cacheTile.Zoom == 30;
    viewport.setZoom(100.);
    const bool maximumClamped = viewport.zoom() == 30.;
    viewport.setZoom(std::numeric_limits<double>::quiet_NaN());
    const bool invalidIgnored = viewport.zoom() == 30.;
    viewport.setZoom(-1.);
    const bool minimumClamped = viewport.zoom() == 0.;
    viewport.SetViewSpan(0.7);
    viewport.SetViewSpan(std::numeric_limits<double>::infinity());
    return postprocess_test::Require(highZoomAccepted, "viewport still limits zoom to 23") &&
           postprocess_test::Require(maximumClamped && minimumClamped && invalidIgnored,
                                     "zoom did not preserve numeric bounds or reject NaN") &&
           postprocess_test::Require(std::abs(viewport.GetViewSpan() - 0.7) < 1.e-9,
                                     "fractional view span below 1 is not accepted");
}

bool testZoomThroughUndefinedSourceLevel()
{
    DownTool::TianDiTuMultidateDown projection;
    projection.SetTileSize(256);
    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(1024, 768);
    viewport.setZoom(1.);
    const auto center = viewport.center();
    if (!postprocess_test::Require(viewport.zoomAt(512., 384., -1., projection,
                                                   DomDownToolPreview::TileYAxisDirection::Down) &&
                                       viewport.zoom() == 0.,
                                   "undefined source level is still clamped"))
    {
        return false;
    }
    return postprocess_test::Require(
        viewport.zoomAt(512., 384., 1., projection, DomDownToolPreview::TileYAxisDirection::Down) &&
            viewport.zoom() == 1. && isNear(viewport.center(), center, 1.e-9),
        "cannot zoom back from an undefined source level");
}

bool runTest()
{
    if (!testZoomThroughUndefinedSourceLevel() || !testHighZoomAndFractionalViewSpan() ||
        !testStandardTileSelection() || !testBaiduTileSelection() ||
        !testTianDiTuMultidateTileSelection() ||
        !testViewSpanShowsMoreTilesWithoutChangingSourceZoom() ||
        !testMapHoverRefreshThrottle())
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

    DownTool::TianDiTuMultidateDown geographicProjection;
    geographicProjection.SetTileSize(256);
    if (!testProjectionInteraction(geographicProjection,
                                   DomDownToolPreview::TileYAxisDirection::Down,
                                   0.000001, "TianDiTu multidate"))
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
