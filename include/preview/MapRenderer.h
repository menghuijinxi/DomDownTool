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
#include "DownloadRegion.h"
#include "TianDiTuMultidateDown.h"
#include "preview/GisOverlayRenderer.h"
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
    size_t multidateBaseVisibleTiles = 0;
    size_t multidateBaseReadyTiles = 0;
    size_t multidateHistoryVisibleTiles = 0;
    size_t multidateHistoryReadyTiles = 0;
    size_t multidateHistoryEmptyTiles = 0;
    TileCacheStats cache;
    TileDownloadStats downloads;
};

struct MapTileDebugLayer
{
    std::string layerName;
    DownTool::Tile tile;
    std::filesystem::path cachePath;
    std::string requestUrl;
    std::string stateText;
    size_t fileBytes = 0;
    int imageWidth = 0;
    int imageHeight = 0;
};

struct MapTileDebugInfo
{
    DownTool::LonLat coordinate;
    std::vector<MapTileDebugLayer> layers;
};

struct ExportGeoBounds
{
    double minimumLongitude = 0.;
    double maximumLongitude = 0.;
    double minimumLatitude = 0.;
    double maximumLatitude = 0.;
};

[[nodiscard]] std::optional<ExportGeoBounds> calculateExportGeoBounds(
    const DownTool::LonLat& centerWgs84, double radiusMeters);

class MapRenderer
{
  public:
    using TileDownloader = std::function<bool(const DownTool::TileHttpRequest&,
                                              const std::filesystem::path&)>;

    explicit MapRenderer(std::function<void()> requestRedraw);

    ~MapRenderer();

    MapRenderer(const MapRenderer&) = delete;
    MapRenderer& operator=(const MapRenderer&) = delete;

    void draw(SkCanvas& canvas, int width, int height, float dpiScale);

    bool panBy(double deltaX, double deltaY);

    bool zoomAt(double screenX, double screenY, double zoomDelta);

    bool adjustViewSpan(double delta);

    void setView(const DownTool::LonLat& centerWgs84, double zoom);

    void setLocationMarker(const std::optional<DownTool::LonLat>& coordinateWgs84);

    [[nodiscard]] std::optional<DownTool::LonLat> lonLatAt(double screenX, double screenY) const;

    bool selectSource(std::string_view sourceId);

    bool UseLatestTianDiTuMultidate();

    bool ConfigureTianDiTuMultidateLayer(const std::string& year,
                                         const std::string& key,
                                         const std::string& gbCode,
                                         const std::string& name,
                                         const std::string& cookie);

    bool SetTianDiTuMultidateCookie(const std::string& cookie);

    void SetTianDiTuMultidateTileDownloader(TileDownloader downloader);

    [[nodiscard]] std::optional<ViewportGeoBounds> visibleGeoBounds() const;

    void SetGisOverlayCategory(GisOverlayCategory category);

    [[nodiscard]] GisOverlayCategory GetGisOverlayCategory() const;

    void SetGisOverlayCategories(const GisOverlayCategories& categories);

    [[nodiscard]] GisOverlayCategories GetGisOverlayCategories() const;

    void SetGisOverlayColor(GisOverlayCategory category,
                             const DownTool::AmapVectorColor& color);

    [[nodiscard]] DownTool::AmapVectorColor GetGisOverlayColor(
        GisOverlayCategory category) const;

    void ResetGisOverlayColors();

    void SetLocalShapefiles(const std::vector<DownTool::ShapefileImportOptions>& sources);
    [[nodiscard]] bool IsLocalShapefileLoading() const;
    [[nodiscard]] std::string GetLocalShapefileError() const;

    void SetExportRadiusMeters(double radiusMeters);

    void SetExportPolygonsWgs84(const DownTool::Wgs84Polygons& polygons);
    void SetDownloadRegions(
        const DownTool::DownloadRegions& regions, const std::string& selectedId = {});
    void SetDraftRegion(const DownTool::Wgs84Ring& vertices, DownTool::DownloadRegionRole role,
                        bool closed);
    [[nodiscard]] std::optional<ViewportPoint> ScreenPointForLonLat(
        const DownTool::LonLat& coordinate) const;

    [[nodiscard]] double GetExportRadiusMeters() const
    {
        return exportRadiusMeters_;
    }

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

    [[nodiscard]] double GetViewSpan() const
    {
        return viewport_.GetViewSpan();
    }

    [[nodiscard]] MapRenderStats stats() const;

    void SetTileDebugEnabled(bool enabled);

    [[nodiscard]] bool GetTileDebugEnabled() const
    {
        return tileDebugEnabled_;
    }

    [[nodiscard]] std::optional<MapTileDebugInfo> TileDebugAt(double screenX,
                                                              double screenY) const;

    void clearCache();

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

    struct TileDebugEntry
    {
        double left = 0.;
        double top = 0.;
        double size = 0.;
        MapTileDebugLayer layer;
    };

    [[nodiscard]] MapSource& currentSource();

    [[nodiscard]] const MapSource& currentSource() const;

    void drawMissingTile(SkCanvas& canvas, const VisibleTile& tile) const;

    void drawTileDebugOverlay(SkCanvas& canvas) const;

    void drawCenterMarker(SkCanvas& canvas, int width, int height) const;

    void drawLocationMarker(SkCanvas& canvas, int width, int height) const;

    void drawExportBounds(SkCanvas& canvas) const;
    void drawRangeEditor(SkCanvas& canvas) const;

    std::vector<MapSource> sources_;
    std::unique_ptr<DownTool::TianDiTuMultidateDown> tianDiTuMultidateLatestProjection_;
    TileDownloader tianDiTuMultidateTileDownloader_;
    size_t activeSourceIndex_ = 0;
    MapViewport viewport_;
    TileImageCache imageCache_;
    TileDownloadQueue downloadQueue_;
    GisOverlayRenderer gisOverlay_;
    GisOverlayRenderer localGisOverlay_;
    MapRenderStats renderStats_;
    std::vector<TileDebugEntry> tileDebugEntries_;
    std::optional<DownTool::LonLat> locationMarkerWgs84_;
    double exportRadiusMeters_ = 0.;
    DownTool::Wgs84Polygons exportPolygonsWgs84_;
    double toolbarHeight_ = 52.;
    double statusBarHeight_ = 28.;
    bool tileDebugEnabled_ = false;
    bool forceRetryKnownTileFailures_ = false;

protected:
    DownTool::DownloadRegions downloadRegions_;
    std::string selectedRegionId_;
    DownTool::Wgs84Ring draftRegion_;
    DownTool::DownloadRegionRole draftRegionRole_ = DownTool::DownloadRegionRole::Include;
    bool draftRegionClosed_ = false;
};
} // namespace DomDownToolPreview
