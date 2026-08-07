#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "DownToolBase.h"
#include "preview/MapViewport.h"
#include "preview/TileDownloadQueue.h"
#include "preview/TileImageCache.h"

class SkCanvas;

namespace DomDownToolPreview
{
struct MapSourceSummary
{
    std::string id;
    std::string name;
    std::string coordinateSystem;
    std::filesystem::path cacheRoot;
    double minimumZoom = 1.;
    double maximumZoom = 20.;
};

struct MapRenderStats
{
    size_t visibleTiles = 0;
    size_t readyTiles = 0;
    TileCacheStats cache;
    TileDownloadStats downloads;
};

class MapRenderer
{
  public:
    explicit MapRenderer(std::function<void()> requestRedraw);

    ~MapRenderer();

    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;

    void draw(SkCanvas& canvas, int width, int height, float dpiScale);

    bool panBy(double deltaX, double deltaY);

    bool zoomAt(double screenX, double screenY, double zoomDelta);

    void setView(const DownTool::LonLat& centerWgs84, double zoom);

    void setLocationMarker(const std::optional<DownTool::LonLat>& coordinateWgs84);

    [[nodiscard]] std::optional<DownTool::LonLat> lonLatAt(double screenX, double screenY) const;

    bool selectSource(std::string_view sourceId);

    [[nodiscard]] std::vector<MapSourceSummary> sources() const;

    [[nodiscard]] MapSourceSummary activeSource() const;

    [[nodiscard]] const DownTool::LonLat& center() const
    {
        return viewport_.center();
    }

    [[nodiscard]] double zoom() const
    {
        return viewport_.zoom();
    }

    [[nodiscard]] MapRenderStats stats() const;

    void clearCache();

    void setAutoDownloadEnabled(bool enabled);

    [[nodiscard]] bool autoDownloadEnabled() const;

    [[nodiscard]] double toolbarHeight() const
    {
        return toolbarHeight_;
    }

    [[nodiscard]] double statusBarHeight() const
    {
        return statusBarHeight_;
    }

  private:
    struct MapSource
    {
        MapSourceSummary summary;
        TileYAxisDirection yDirection = TileYAxisDirection::Down;
        std::unique_ptr<DownTool::DownToolBase> projection;
    };

    [[nodiscard]] MapSource& currentSource();

    [[nodiscard]] const MapSource& currentSource() const;

    void drawMissingTile(SkCanvas& canvas, const VisibleTile& tile) const;

    void drawCenterMarker(SkCanvas& canvas, int width, int height) const;

    void drawLocationMarker(SkCanvas& canvas, int width, int height) const;

    std::vector<MapSource> sources_;
    size_t activeSourceIndex_ = 0;
    MapViewport viewport_;
    TileImageCache imageCache_;
    TileDownloadQueue downloadQueue_;
    MapRenderStats renderStats_;
    std::optional<DownTool::LonLat> locationMarkerWgs84_;
    double toolbarHeight_ = 52.;
    double statusBarHeight_ = 28.;
};
} // namespace DomDownToolPreview
