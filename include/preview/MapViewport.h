#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "DataType.h"

namespace DownTool
{
class DownToolBase;
}

namespace DomDownToolPreview
{
enum class TileYAxisDirection
{
    Down,
    Up
};

struct ViewportInsets
{
    double top = 0.;
    double bottom = 0.;
};

struct VisibleTile
{
    DownTool::Tile cacheTile;
    double left = 0.;
    double top = 0.;
    double size = 0.;
};

struct ViewportPoint
{
    double x = 0.;
    double y = 0.;
};

class MapViewport
{
  public:
    MapViewport();

    void setSize(int width, int height, const ViewportInsets& insets = {});

    void setCenter(const DownTool::LonLat& center);

    [[nodiscard]] const DownTool::LonLat& center() const
    {
        return center_;
    }

    void setZoom(double zoom);

    [[nodiscard]] double zoom() const
    {
        return zoom_;
    }

    [[nodiscard]] uint32_t sourceZoom() const;

    [[nodiscard]] std::vector<VisibleTile> visibleTiles(const DownTool::DownToolBase& projection,
                                                        TileYAxisDirection yDirection,
                                                        uint32_t tileSize = 256,
                                                        int overscanTiles = 1) const;

    bool panBy(double deltaX, double deltaY, const DownTool::DownToolBase& projection,
               TileYAxisDirection yDirection, uint32_t tileSize = 256);

    bool zoomAt(double screenX, double screenY, double zoomDelta, double minimumZoom,
                double maximumZoom, const DownTool::DownToolBase& projection,
                TileYAxisDirection yDirection, uint32_t tileSize = 256);

    [[nodiscard]] std::optional<DownTool::LonLat> lonLatAt(double screenX, double screenY,
                                                           const DownTool::DownToolBase& projection,
                                                           TileYAxisDirection yDirection,
                                                           uint32_t tileSize = 256) const;

    [[nodiscard]] std::optional<ViewportPoint> screenPointForLonLat(
        const DownTool::LonLat& lonLat, const DownTool::DownToolBase& projection,
        TileYAxisDirection yDirection, uint32_t tileSize = 256) const;

  private:
    struct SourcePoint
    {
        double x = 0.;
        double y = 0.;
    };

    [[nodiscard]] bool project(const DownTool::LonLat& lonLat, uint32_t zoom,
                               const DownTool::DownToolBase& projection,
                               TileYAxisDirection yDirection, uint32_t tileSize,
                               SourcePoint& point) const;

    [[nodiscard]] bool unproject(const SourcePoint& point, uint32_t zoom,
                                 const DownTool::DownToolBase& projection,
                                 TileYAxisDirection yDirection, uint32_t tileSize,
                                 DownTool::LonLat& lonLat) const;

    [[nodiscard]] double centerScreenX() const;

    [[nodiscard]] double centerScreenY() const;

    int width_ = 1;
    int height_ = 1;
    ViewportInsets insets_;
    DownTool::LonLat center_{116.397389, 39.908722};
    double zoom_ = 15.;
};

[[nodiscard]] std::filesystem::path resolveTilePath(const std::filesystem::path& cacheRoot,
                                                    const DownTool::Tile& tile);
} // namespace DomDownToolPreview
