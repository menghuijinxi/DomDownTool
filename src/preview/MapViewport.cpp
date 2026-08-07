#include "preview/MapViewport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

#include "DownToolBase.h"

namespace
{
constexpr double kMinimumLatitude = -85.0511287798066;
constexpr double kMaximumLatitude = 85.0511287798066;
constexpr double kMinimumZoom = 0.;
constexpr double kMaximumZoom = 23.;
constexpr double kMinimumViewSpan = 1.;
constexpr double kMaximumViewSpan = 4.;

int64_t normalizeTileX(int64_t tileX, int64_t tileCount)
{
    const int64_t remainder = tileX % tileCount;
    return remainder >= 0 ? remainder : remainder + tileCount;
}
} // namespace

DomDownToolPreview::MapViewport::MapViewport() = default;

std::optional<DomDownToolPreview::ViewportSourcePoint>
DomDownToolPreview::ViewportTransform::sourcePointForLonLat(
    const DownTool::LonLat& lonLat) const
{
    if (projection == nullptr || tileSize == 0)
    {
        return std::nullopt;
    }

    DownTool::Tile tile;
    double pixelX = 0.;
    double pixelY = 0.;
    if (!projection->TryProjectToSourceTile(lonLat, sourceZoom, tile, pixelX, pixelY))
    {
        return std::nullopt;
    }

    const double tileExtent = static_cast<double>(tileSize);
    const double sourceX = static_cast<double>(tile.X) * tileExtent + pixelX;
    const double sourceY = yDirection == TileYAxisDirection::Down
                               ? static_cast<double>(tile.Y) * tileExtent + pixelY
                               : -static_cast<double>(tile.Y + 1) * tileExtent + pixelY;
    if (!std::isfinite(sourceX) || !std::isfinite(sourceY))
    {
        return std::nullopt;
    }
    return ViewportSourcePoint{sourceX, sourceY};
}

DomDownToolPreview::ViewportPoint
DomDownToolPreview::ViewportTransform::screenPointForSourcePoint(
    const ViewportSourcePoint& point) const
{
    return ViewportPoint{centerScreenX + (point.x - centerSourceX) * displayScale,
                         centerScreenY + (point.y - centerSourceY) * displayScale};
}

void DomDownToolPreview::MapViewport::setSize(int width, int height, const ViewportInsets& insets)
{
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    insets_.top = std::clamp(insets.top, 0., static_cast<double>(height_));
    insets_.bottom = std::clamp(insets.bottom, 0., static_cast<double>(height_) - insets_.top);
}

void DomDownToolPreview::MapViewport::setCenter(const DownTool::LonLat& center)
{
    center_.lon = std::isfinite(center.lon) ? center.lon : center_.lon;
    center_.lat = std::isfinite(center.lat)
                      ? std::clamp(center.lat, kMinimumLatitude, kMaximumLatitude)
                      : center_.lat;
}

void DomDownToolPreview::MapViewport::setZoom(double zoom)
{
    if (std::isfinite(zoom))
    {
        zoom_ = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
    }
}

uint32_t DomDownToolPreview::MapViewport::sourceZoom() const
{
    return static_cast<uint32_t>(std::floor(zoom_));
}

void DomDownToolPreview::MapViewport::SetViewSpan(double viewSpan)
{
    if (std::isfinite(viewSpan))
    {
        viewSpan_ = std::clamp(viewSpan, kMinimumViewSpan, kMaximumViewSpan);
    }
}

std::vector<DomDownToolPreview::VisibleTile> DomDownToolPreview::MapViewport::visibleTiles(
    const DownTool::DownToolBase& projection, TileYAxisDirection yDirection, uint32_t tileSize,
    int overscanTiles) const
{
    std::vector<VisibleTile> result;
    if (tileSize == 0)
    {
        return result;
    }

    const uint32_t integerZoom = sourceZoom();
    SourcePoint centerPoint;
    if (!project(center_, integerZoom, projection, yDirection, tileSize, centerPoint))
    {
        return result;
    }

    const double scale = displayScale();
    const double tileExtent = static_cast<double>(tileSize);
    const int margin = std::max(0, overscanTiles);
    const double worldLeft = centerPoint.x - centerScreenX() / scale;
    const double worldTop = centerPoint.y - centerScreenY() / scale;
    const double worldRight =
        centerPoint.x + (static_cast<double>(width_) - centerScreenX()) / scale;
    const double worldBottom =
        centerPoint.y + (static_cast<double>(height_) - centerScreenY()) / scale;

    const int64_t firstColumn = static_cast<int64_t>(std::floor(worldLeft / tileExtent)) - margin;
    const int64_t lastColumn = static_cast<int64_t>(std::floor(worldRight / tileExtent)) + margin;
    const int64_t firstRow = static_cast<int64_t>(std::floor(worldTop / tileExtent)) - margin;
    const int64_t lastRow = static_cast<int64_t>(std::floor(worldBottom / tileExtent)) + margin;
    const int64_t tileCount = int64_t{1} << integerZoom;

    result.reserve(static_cast<size_t>(std::max<int64_t>(0, lastColumn - firstColumn + 1) *
                                       std::max<int64_t>(0, lastRow - firstRow + 1)));
    for (int64_t row = firstRow; row <= lastRow; row++)
    {
        if (yDirection == TileYAxisDirection::Down && (row < 0 || row >= tileCount))
        {
            continue;
        }

        for (int64_t column = firstColumn; column <= lastColumn; column++)
        {
            const int64_t cacheX =
                yDirection == TileYAxisDirection::Down ? normalizeTileX(column, tileCount) : column;
            const int64_t cacheY = yDirection == TileYAxisDirection::Down ? row : -row - 1;
            if (cacheX < std::numeric_limits<int32_t>::min() ||
                cacheX > std::numeric_limits<int32_t>::max() ||
                cacheY < std::numeric_limits<int32_t>::min() ||
                cacheY > std::numeric_limits<int32_t>::max())
            {
                continue;
            }

            VisibleTile visibleTile;
            visibleTile.cacheTile = DownTool::Tile(static_cast<int32_t>(cacheX),
                                                   static_cast<int32_t>(cacheY), integerZoom);
            visibleTile.left = centerScreenX() +
                               (static_cast<double>(column) * tileExtent - centerPoint.x) * scale;
            visibleTile.top =
                centerScreenY() + (static_cast<double>(row) * tileExtent - centerPoint.y) * scale;
            visibleTile.size = tileExtent * scale;
            result.push_back(visibleTile);
        }
    }

    return result;
}

bool DomDownToolPreview::MapViewport::panBy(double deltaX, double deltaY,
                                            const DownTool::DownToolBase& projection,
                                            TileYAxisDirection yDirection, uint32_t tileSize)
{
    const uint32_t integerZoom = sourceZoom();
    SourcePoint centerPoint;
    if (!project(center_, integerZoom, projection, yDirection, tileSize, centerPoint))
    {
        return false;
    }

    const double scale = displayScale();
    centerPoint.x -= deltaX / scale;
    centerPoint.y -= deltaY / scale;
    DownTool::LonLat nextCenter;
    if (!unproject(centerPoint, integerZoom, projection, yDirection, tileSize, nextCenter))
    {
        return false;
    }

    setCenter(nextCenter);
    return true;
}

bool DomDownToolPreview::MapViewport::zoomAt(double screenX, double screenY, double zoomDelta,
                                             double minimumZoom, double maximumZoom,
                                             const DownTool::DownToolBase& projection,
                                             TileYAxisDirection yDirection, uint32_t tileSize)
{
    const std::optional<DownTool::LonLat> anchor =
        lonLatAt(screenX, screenY, projection, yDirection, tileSize);
    if (!anchor.has_value())
    {
        return false;
    }

    const double previousZoom = zoom_;
    const DownTool::LonLat previousCenter = center_;
    setZoom(std::clamp(zoom_ + zoomDelta, minimumZoom, maximumZoom));
    const uint32_t integerZoom = sourceZoom();
    SourcePoint anchorPoint;
    if (!project(anchor.value(), integerZoom, projection, yDirection, tileSize, anchorPoint))
    {
        zoom_ = previousZoom;
        return false;
    }

    const double scale = displayScale();
    const SourcePoint centerPoint{anchorPoint.x - (screenX - centerScreenX()) / scale,
                                  anchorPoint.y - (screenY - centerScreenY()) / scale};
    DownTool::LonLat nextCenter;
    if (!unproject(centerPoint, integerZoom, projection, yDirection, tileSize, nextCenter))
    {
        zoom_ = previousZoom;
        center_ = previousCenter;
        return false;
    }

    setCenter(nextCenter);
    return true;
}

std::optional<DownTool::LonLat> DomDownToolPreview::MapViewport::lonLatAt(
    double screenX, double screenY, const DownTool::DownToolBase& projection,
    TileYAxisDirection yDirection, uint32_t tileSize) const
{
    const uint32_t integerZoom = sourceZoom();
    SourcePoint centerPoint;
    if (!project(center_, integerZoom, projection, yDirection, tileSize, centerPoint))
    {
        return std::nullopt;
    }

    const double scale = displayScale();
    const SourcePoint targetPoint{centerPoint.x + (screenX - centerScreenX()) / scale,
                                  centerPoint.y + (screenY - centerScreenY()) / scale};
    DownTool::LonLat lonLat;
    if (!unproject(targetPoint, integerZoom, projection, yDirection, tileSize, lonLat))
    {
        return std::nullopt;
    }

    return lonLat;
}

std::optional<DomDownToolPreview::ViewportPoint>
DomDownToolPreview::MapViewport::screenPointForLonLat(
    const DownTool::LonLat& lonLat, const DownTool::DownToolBase& projection,
    TileYAxisDirection yDirection, uint32_t tileSize) const
{
    const uint32_t integerZoom = sourceZoom();
    SourcePoint centerPoint;
    SourcePoint targetPoint;
    if (!project(center_, integerZoom, projection, yDirection, tileSize, centerPoint) ||
        !project(lonLat, integerZoom, projection, yDirection, tileSize, targetPoint))
    {
        return std::nullopt;
    }

    const double scale = displayScale();
    return ViewportPoint{centerScreenX() + (targetPoint.x - centerPoint.x) * scale,
                         centerScreenY() + (targetPoint.y - centerPoint.y) * scale};
}

std::optional<DomDownToolPreview::ViewportTransform>
DomDownToolPreview::MapViewport::makeTransform(
    const DownTool::DownToolBase& projection, TileYAxisDirection yDirection,
    uint32_t tileSize) const
{
    if (tileSize == 0)
    {
        return std::nullopt;
    }

    SourcePoint centerPoint;
    if (!project(center_, sourceZoom(), projection, yDirection, tileSize, centerPoint))
    {
        return std::nullopt;
    }

    return ViewportTransform{sourceZoom(), centerPoint.x, centerPoint.y, centerScreenX(),
                             centerScreenY(), displayScale(), &projection, yDirection, tileSize};
}

std::optional<DomDownToolPreview::ViewportGeoBounds>
DomDownToolPreview::MapViewport::visibleGeoBounds(
    const DownTool::DownToolBase& projection, TileYAxisDirection yDirection,
    uint32_t tileSize) const
{
    const double top = insets_.top;
    const double bottom = static_cast<double>(height_) - insets_.bottom;
    const std::array<std::pair<double, double>, 4> corners = {
        std::pair{0., top}, std::pair{static_cast<double>(width_), top},
        std::pair{0., bottom}, std::pair{static_cast<double>(width_), bottom}};
    ViewportGeoBounds bounds;
    bool initialized = false;
    for (const auto& [x, y] : corners)
    {
        const std::optional<DownTool::LonLat> coordinate =
            lonLatAt(x, y, projection, yDirection, tileSize);
        if (!coordinate.has_value())
        {
            return std::nullopt;
        }
        if (!initialized)
        {
            bounds.minLon = coordinate->lon;
            bounds.maxLon = coordinate->lon;
            bounds.minLat = coordinate->lat;
            bounds.maxLat = coordinate->lat;
            initialized = true;
        }
        else
        {
            bounds.minLon = std::min(bounds.minLon, coordinate->lon);
            bounds.maxLon = std::max(bounds.maxLon, coordinate->lon);
            bounds.minLat = std::min(bounds.minLat, coordinate->lat);
            bounds.maxLat = std::max(bounds.maxLat, coordinate->lat);
        }
    }
    return bounds;
}

bool DomDownToolPreview::MapViewport::project(const DownTool::LonLat& lonLat, uint32_t zoom,
                                              const DownTool::DownToolBase& projection,
                                              TileYAxisDirection yDirection, uint32_t tileSize,
                                              SourcePoint& point) const
{
    DownTool::Tile tile;
    double pixelX = 0.;
    double pixelY = 0.;
    if (!projection.TryProjectToSourceTile(lonLat, zoom, tile, pixelX, pixelY))
    {
        return false;
    }

    const double tileExtent = static_cast<double>(tileSize);
    point.x = static_cast<double>(tile.X) * tileExtent + pixelX;
    point.y = yDirection == TileYAxisDirection::Down
                  ? static_cast<double>(tile.Y) * tileExtent + pixelY
                  : -static_cast<double>(tile.Y + 1) * tileExtent + pixelY;
    return std::isfinite(point.x) && std::isfinite(point.y);
}

bool DomDownToolPreview::MapViewport::unproject(const SourcePoint& point, uint32_t zoom,
                                                const DownTool::DownToolBase& projection,
                                                TileYAxisDirection yDirection, uint32_t tileSize,
                                                DownTool::LonLat& lonLat) const
{
    if (tileSize == 0 || !std::isfinite(point.x) || !std::isfinite(point.y))
    {
        return false;
    }

    const double tileExtent = static_cast<double>(tileSize);
    const int64_t column = static_cast<int64_t>(std::floor(point.x / tileExtent));
    const int64_t row = static_cast<int64_t>(std::floor(point.y / tileExtent));
    const int64_t cacheY = yDirection == TileYAxisDirection::Down ? row : -row - 1;
    if (column < std::numeric_limits<int32_t>::min() ||
        column > std::numeric_limits<int32_t>::max() ||
        cacheY < std::numeric_limits<int32_t>::min() ||
        cacheY > std::numeric_limits<int32_t>::max())
    {
        return false;
    }

    const DownTool::Tile tile(static_cast<int32_t>(column), static_cast<int32_t>(cacheY), zoom);
    const double pixelX = point.x - static_cast<double>(column) * tileExtent;
    const double pixelY = point.y - static_cast<double>(row) * tileExtent;
    return projection.TryUnprojectFromSourceTile(tile, pixelX, pixelY, lonLat);
}

double DomDownToolPreview::MapViewport::centerScreenX() const
{
    return static_cast<double>(width_) * 0.5;
}

double DomDownToolPreview::MapViewport::centerScreenY() const
{
    const double contentHeight =
        std::max(0., static_cast<double>(height_) - insets_.top - insets_.bottom);
    return insets_.top + contentHeight * 0.5;
}

double DomDownToolPreview::MapViewport::displayScale() const
{
    return std::exp2(zoom_ - static_cast<double>(sourceZoom())) / viewSpan_;
}

std::filesystem::path DomDownToolPreview::resolveTilePath(const std::filesystem::path& cacheRoot,
                                                          const DownTool::Tile& tile)
{
    return cacheRoot / std::to_string(tile.Zoom) / std::to_string(tile.X) /
           (std::to_string(tile.Y) + ".png");
}
