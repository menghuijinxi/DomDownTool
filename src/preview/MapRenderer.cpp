#include "preview/MapRenderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <utility>

#include "ArcGISDown.h"
#include "BaiduDown.h"
#include "BingDown.h"
#include "GaoDeDown.h"
#include "GoogleDown.h"
#include "TencentDown.h"
#include "TianDiTuDown.h"
#include "TianDiTuMultidateDown.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"

namespace
{
constexpr uint32_t kTileSize = 256;
constexpr double kEarthRadiusMeters = 6378137.;

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8Path = path.u8string();
    std::string result;
    result.reserve(utf8Path.size());
    for (const char8_t character : utf8Path)
    {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::string sanitizeCacheLayerName(std::string layerName)
{
    if (layerName.empty())
    {
        return "latest";
    }

    for (char& character : layerName)
    {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 32 || character == '<' || character == '>' || character == ':' ||
            character == '"' || character == '/' || character == '\\' || character == '|' ||
            character == '?' || character == '*')
        {
            character = '_';
        }
    }
    return layerName;
}

std::filesystem::path multidateCacheRoot(std::string layerName)
{
    return std::filesystem::u8path("cache/天地图多时相") /
        std::filesystem::u8path(sanitizeCacheLayerName(std::move(layerName)));
}
} // namespace

std::optional<DomDownToolPreview::ExportGeoBounds>
DomDownToolPreview::calculateExportGeoBounds(
    const DownTool::LonLat& centerWgs84, double radiusMeters)
{
    if (!std::isfinite(centerWgs84.lon) || !std::isfinite(centerWgs84.lat) ||
        !std::isfinite(radiusMeters) || radiusMeters <= 0.)
    {
        return std::nullopt;
    }

    const double latitudeRadians = centerWgs84.lat * std::numbers::pi / 180.;
    const double latitudeOffset = radiusMeters / kEarthRadiusMeters * 180. /
        std::numbers::pi;
    const double longitudeScale = std::max(0.000001, std::abs(std::cos(latitudeRadians)));
    const double longitudeOffset = latitudeOffset / longitudeScale;
    return ExportGeoBounds{
        std::max(-180., centerWgs84.lon - longitudeOffset),
        std::min(180., centerWgs84.lon + longitudeOffset),
        std::max(-85.05112878, centerWgs84.lat - latitudeOffset),
        std::min(85.05112878, centerWgs84.lat + latitudeOffset)};
}

DomDownToolPreview::MapRenderer::MapRenderer(std::function<void()> requestRedraw)
    : imageCache_(requestRedraw),
      downloadQueue_(requestRedraw),
      gisOverlay_(std::move(requestRedraw))
{
    auto addSource =
        [this](std::string id, std::string name, std::string coordinateSystem,
               std::unique_ptr<DownTool::DownToolBase> projection,
               TileYAxisDirection yDirection, double minimumZoom, double maximumZoom)
    {
        projection->SetTileSize(kTileSize);
        MapSource source;
        source.summary.id = std::move(id);
        source.summary.name = std::move(name);
        source.summary.coordinateSystem = std::move(coordinateSystem);
        source.summary.cacheRoot = std::filesystem::u8path(projection->GetCachePath());
        if (source.summary.id == "none")
        {
            source.summary.cacheRoot.clear();
        }
        source.summary.minimumZoom = minimumZoom;
        source.summary.maximumZoom = maximumZoom;
        source.yDirection = yDirection;
        source.projection = std::move(projection);
        sources_.push_back(std::move(source));
    };

    addSource("gaode.satellite", "高德卫星", "GCJ02 / XYZ",
              std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::Satellite),
              TileYAxisDirection::Down, 3., 20.);
    addSource("gaode.electronic", "高德电子地图", "GCJ02 / XYZ",
              std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::Electronic),
              TileYAxisDirection::Down, 3., 20.);
    addSource("gaode.roads", "高德透明路网", "GCJ02 / XYZ",
              std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::RoadNetworkOverlay),
              TileYAxisDirection::Down, 3., 20.);
    addSource("tencent.satellite", "腾讯卫星", "GCJ02 / XYZ",
              std::make_unique<DownTool::TencentDown>(), TileYAxisDirection::Down, 3., 19.);
    addSource("tianditu.satellite", "天地图影像", "CGCS2000 / XYZ",
              std::make_unique<DownTool::TianDiTuDown>(), TileYAxisDirection::Down, 1., 18.);
    auto multidateProjection = std::make_unique<DownTool::TianDiTuMultidateDown>();
    multidateProjection->SetCachePath(pathToUtf8(multidateCacheRoot("latest")));
    tianDiTuMultidateLatestProjection_ = std::make_unique<DownTool::TianDiTuMultidateDown>();
    tianDiTuMultidateLatestProjection_->SetTileSize(kTileSize);
    tianDiTuMultidateLatestProjection_->SetCachePath(pathToUtf8(multidateCacheRoot("latest")));
    addSource("tianditu.multidate", "天地图多时相", "CGCS2000 / EPSG:4326",
              std::move(multidateProjection), TileYAxisDirection::Down, 17., 18.);
    addSource("baidu.satellite", "百度卫星", "BD09 / Baidu",
              std::make_unique<DownTool::BaiduDown>(), TileYAxisDirection::Up, 3., 19.);
    addSource("google.satellite", "Google 卫星", "WGS84 / XYZ",
              std::make_unique<DownTool::GoogleDown>(), TileYAxisDirection::Down, 1., 21.);
    addSource("bing.satellite", "Bing 卫星", "WGS84 / XYZ", std::make_unique<DownTool::BingDown>(),
              TileYAxisDirection::Down, 1., 20.);
    addSource("arcgis.satellite", "ArcGIS 影像", "WGS84 / XYZ",
              std::make_unique<DownTool::ArcGISDown>(), TileYAxisDirection::Down, 1., 20.);
    addSource("none", "无", "WGS84 / XYZ", std::make_unique<DownTool::GoogleDownBase>(),
              TileYAxisDirection::Down, 1., 21.);
}

DomDownToolPreview::MapRenderer::~MapRenderer() = default;

void DomDownToolPreview::MapRenderer::draw(SkCanvas& canvas, int width, int height, float dpiScale)
{
    toolbarHeight_ = 52. * std::max(0.1F, dpiScale);
    statusBarHeight_ = 28. * std::max(0.1F, dpiScale);
    viewport_.setSize(width, height, {toolbarHeight_, statusBarHeight_});
    for (const std::filesystem::path& path : downloadQueue_.takeCompletedPaths())
    {
        imageCache_.invalidate(path);
    }
    imageCache_.beginFrame(canvas);

    const SkRect mapBounds = SkRect::MakeLTRB(
        0.F, static_cast<float>(toolbarHeight_), static_cast<float>(width),
        std::max(static_cast<float>(toolbarHeight_),
                 static_cast<float>(height) - static_cast<float>(statusBarHeight_)));
    SkAutoCanvasRestore mapRestore(&canvas, true);
    canvas.clipRect(mapBounds, SkClipOp::kIntersect, false);
    SkPaint backgroundPaint;
    backgroundPaint.setColor(SkColorSetRGB(31, 34, 33));
    canvas.drawRect(mapBounds, backgroundPaint);

    MapSource& source = currentSource();
    renderStats_.visibleTiles = 0;
    renderStats_.readyTiles = 0;
    std::vector<TileDownloadJob> downloadJobs;
    const double mapCenterX = static_cast<double>(width) * 0.5;
    const double mapContentHeight =
        std::max(0., static_cast<double>(height) - toolbarHeight_ - statusBarHeight_);
    const double mapCenterY = static_cast<double>(toolbarHeight_) + mapContentHeight * 0.5;

    const SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    struct TileLayerResult
    {
        size_t visibleTiles = 0;
        size_t readyTiles = 0;
    };
    auto drawTileLayer = [&](DownTool::DownToolBase& projection, TileYAxisDirection yDirection,
                             const std::filesystem::path& cacheRoot,
                             bool drawMissingTiles,
                             const TileDownloader* customDownloader = nullptr) -> TileLayerResult
    {
        const std::vector<VisibleTile> visibleTiles =
            viewport_.visibleTiles(projection, yDirection, kTileSize, 1);
        downloadJobs.reserve(downloadJobs.size() + visibleTiles.size());
        TileLayerResult result;
        result.visibleTiles = visibleTiles.size();
        for (const VisibleTile& visibleTile : visibleTiles)
        {
            const std::filesystem::path tilePath =
                resolveTilePath(cacheRoot, visibleTile.cacheTile);
            sk_sp<SkImage> image = imageCache_.findOrRequest(tilePath, canvas);
            if (!image)
            {
                if (imageCache_.isKnownMissing(tilePath))
                {
                    TileDownloadJob job{
                        tilePath,
                        visibleTile.cacheTile,
                        &projection,
                        projection.BuildTileHttpRequest(visibleTile.cacheTile)};
                    job.useTransparentPlaceholderOnEmptyResponse = !drawMissingTiles;
                    if (customDownloader != nullptr && *customDownloader)
                    {
                        job.customDownloader = *customDownloader;
                    }
                    const double tileCenterX =
                        visibleTile.left + static_cast<double>(visibleTile.size) * 0.5;
                    const double tileCenterY =
                        visibleTile.top + static_cast<double>(visibleTile.size) * 0.5;
                    const double distanceX = tileCenterX - mapCenterX;
                    const double distanceY = tileCenterY - mapCenterY;
                    job.priority = distanceX * distanceX + distanceY * distanceY;
                    downloadJobs.push_back(std::move(job));
                }
                if (drawMissingTiles)
                {
                    drawMissingTile(canvas, visibleTile);
                }
                continue;
            }

            result.readyTiles++;
            const SkRect sourceRect = SkRect::MakeIWH(image->width(), image->height());
            const SkRect destinationRect = SkRect::MakeXYWH(
                static_cast<float>(visibleTile.left), static_cast<float>(visibleTile.top),
                static_cast<float>(visibleTile.size + 0.5),
                static_cast<float>(visibleTile.size + 0.5));
            canvas.drawImageRect(image, sourceRect, destinationRect, sampling, nullptr,
                                 SkCanvas::kFast_SrcRectConstraint);
        }

        return result;
    };
    if (source.summary.id != "none")
    {
        auto* multidateProjection = dynamic_cast<DownTool::TianDiTuMultidateDown*>(
            source.projection.get());
        const bool shouldDrawMultidateBase = source.summary.id == "tianditu.multidate" &&
            multidateProjection != nullptr && !multidateProjection->GetYear().empty() &&
            tianDiTuMultidateLatestProjection_ != nullptr;
        if (shouldDrawMultidateBase)
        {
            // 官网多时相也是“最新影像底图 + 历史时相覆盖层”。历史瓦片缺失时保留底图。
            const TileLayerResult baseLayer = drawTileLayer(
                *tianDiTuMultidateLatestProjection_, source.yDirection,
                multidateCacheRoot("latest"), true);
            const TileLayerResult historyLayer = drawTileLayer(
                *source.projection, source.yDirection, source.summary.cacheRoot, false,
                &tianDiTuMultidateTileDownloader_);
            renderStats_.visibleTiles = baseLayer.visibleTiles + historyLayer.visibleTiles;
            renderStats_.readyTiles = baseLayer.readyTiles + historyLayer.readyTiles;
        }
        else
        {
            const TileLayerResult layer = drawTileLayer(
                *source.projection, source.yDirection, source.summary.cacheRoot, true);
            renderStats_.visibleTiles = layer.visibleTiles;
            renderStats_.readyTiles = layer.readyTiles;
        }
    }

    downloadQueue_.updateVisibleRequests(downloadJobs);

    const double worldPixels =
        static_cast<double>(kTileSize) * std::pow(2., viewport_.sourceZoom());
    const double displayScale = 1. / viewport_.GetViewSpan();
    const double mapWidthDegrees =
        360. * static_cast<double>(width) / std::max(1., worldPixels * displayScale);
    const double mapHeightDegrees =
        360. * mapContentHeight / std::max(1., worldPixels * displayScale);
    const double latitudeScale =
        std::max(0.2, std::cos(viewport_.center().lat * std::numbers::pi / 180.));
    const double radiusMeters = std::max(mapWidthDegrees * latitudeScale, mapHeightDegrees) *
        111320. * 0.65;
    gisOverlay_.UpdateContext(viewport_.center(), radiusMeters, viewport_.sourceZoom());
    gisOverlay_.Draw(canvas, viewport_, *source.projection, source.yDirection, kTileSize);

    drawExportBounds(canvas);
    drawCenterMarker(canvas, width, height);
    drawLocationMarker(canvas, width, height);
    renderStats_.cache = imageCache_.stats();
    renderStats_.downloads = downloadQueue_.stats();
}

bool DomDownToolPreview::MapRenderer::panBy(double deltaX, double deltaY)
{
    MapSource& source = currentSource();
    return viewport_.panBy(deltaX, deltaY, *source.projection, source.yDirection, kTileSize);
}

bool DomDownToolPreview::MapRenderer::zoomAt(double screenX, double screenY, double zoomDelta)
{
    MapSource& source = currentSource();
    return viewport_.zoomAt(screenX, screenY, zoomDelta, source.summary.minimumZoom,
                            source.summary.maximumZoom, *source.projection, source.yDirection,
                            kTileSize);
}

bool DomDownToolPreview::MapRenderer::adjustViewSpan(double delta)
{
    const double previousViewSpan = viewport_.GetViewSpan();
    viewport_.SetViewSpan(previousViewSpan + delta);
    if (viewport_.GetViewSpan() == previousViewSpan)
    {
        return false;
    }

    downloadQueue_.cancelPending();
    return true;
}

void DomDownToolPreview::MapRenderer::setView(const DownTool::LonLat& centerWgs84, double zoom)
{
    const MapSource& source = currentSource();
    viewport_.setCenter(centerWgs84);
    viewport_.setZoom(std::clamp(zoom, source.summary.minimumZoom, source.summary.maximumZoom));
    downloadQueue_.cancelPending();
}

void DomDownToolPreview::MapRenderer::setLocationMarker(
    const std::optional<DownTool::LonLat>& coordinateWgs84)
{
    locationMarkerWgs84_ = coordinateWgs84;
}

std::optional<DownTool::LonLat> DomDownToolPreview::MapRenderer::lonLatAt(double screenX,
                                                                          double screenY) const
{
    const MapSource& source = currentSource();
    return viewport_.lonLatAt(screenX, screenY, *source.projection, source.yDirection, kTileSize);
}

bool DomDownToolPreview::MapRenderer::selectSource(std::string_view sourceId)
{
    const auto sourceIterator =
        std::find_if(sources_.begin(), sources_.end(),
                     [sourceId](const MapSource& source) { return source.summary.id == sourceId; });
    if (sourceIterator == sources_.end())
    {
        return false;
    }

    activeSourceIndex_ = static_cast<size_t>(std::distance(sources_.begin(), sourceIterator));
    downloadQueue_.cancelPending();
    viewport_.setZoom(std::clamp(viewport_.zoom(), sourceIterator->summary.minimumZoom,
                                 sourceIterator->summary.maximumZoom));
    return true;
}

bool DomDownToolPreview::MapRenderer::UseLatestTianDiTuMultidate()
{
    const auto sourceIterator = std::find_if(
        sources_.begin(), sources_.end(),
        [](const MapSource& source) { return source.summary.id == "tianditu.multidate"; });
    if (sourceIterator == sources_.end())
    {
        return false;
    }

    auto* projection = dynamic_cast<DownTool::TianDiTuMultidateDown*>(
        sourceIterator->projection.get());
    if (!projection)
    {
        return false;
    }

    projection->UseLatestYear();
    sourceIterator->summary.cacheRoot = multidateCacheRoot("latest");
    projection->SetCachePath(pathToUtf8(sourceIterator->summary.cacheRoot));
    downloadQueue_.cancelPending();
    return true;
}

bool DomDownToolPreview::MapRenderer::ConfigureTianDiTuMultidateLayer(
    const std::string& year,
    const std::string& key,
    const std::string& gbCode,
    const std::string& name,
    const std::string& cookie)
{
    const auto sourceIterator = std::find_if(
        sources_.begin(), sources_.end(),
        [](const MapSource& source) { return source.summary.id == "tianditu.multidate"; });
    if (sourceIterator == sources_.end())
    {
        return false;
    }

    auto* projection = dynamic_cast<DownTool::TianDiTuMultidateDown*>(
        sourceIterator->projection.get());
    if (!projection || key.empty())
    {
        return false;
    }

    projection->SetMultidateRegion(gbCode, name);
    projection->SetSessionCookie(cookie);
    projection->SetYearKey(year, key);
    sourceIterator->summary.cacheRoot = multidateCacheRoot(year.empty() ? key : year);
    projection->SetCachePath(pathToUtf8(sourceIterator->summary.cacheRoot));
    downloadQueue_.cancelPending();
    return true;
}

bool DomDownToolPreview::MapRenderer::SetTianDiTuMultidateCookie(const std::string& cookie)
{
    const auto sourceIterator = std::find_if(
        sources_.begin(), sources_.end(),
        [](const MapSource& source) { return source.summary.id == "tianditu.multidate"; });
    if (sourceIterator == sources_.end())
    {
        return false;
    }

    auto* projection = dynamic_cast<DownTool::TianDiTuMultidateDown*>(
        sourceIterator->projection.get());
    if (!projection)
    {
        return false;
    }

    projection->SetSessionCookie(cookie);
    downloadQueue_.cancelPending();
    return true;
}

void DomDownToolPreview::MapRenderer::SetTianDiTuMultidateTileDownloader(
    TileDownloader downloader)
{
    tianDiTuMultidateTileDownloader_ = std::move(downloader);
    downloadQueue_.cancelPending();
}

std::optional<DomDownToolPreview::ViewportGeoBounds>
DomDownToolPreview::MapRenderer::visibleGeoBounds() const
{
    const MapSource& source = currentSource();
    return viewport_.visibleGeoBounds(*source.projection, source.yDirection, kTileSize);
}

void DomDownToolPreview::MapRenderer::SetGisOverlayCategory(GisOverlayCategory category)
{
    gisOverlay_.SetCategory(category);
}

DomDownToolPreview::GisOverlayCategory
DomDownToolPreview::MapRenderer::GetGisOverlayCategory() const
{
    return gisOverlay_.GetCategory();
}

void DomDownToolPreview::MapRenderer::SetGisOverlayCategories(
    const GisOverlayCategories& categories)
{
    gisOverlay_.SetCategories(categories);
}

DomDownToolPreview::GisOverlayCategories
DomDownToolPreview::MapRenderer::GetGisOverlayCategories() const
{
    return gisOverlay_.GetCategories();
}

void DomDownToolPreview::MapRenderer::SetGisOverlayColor(
    GisOverlayCategory category, const DownTool::AmapVectorColor& color)
{
    gisOverlay_.SetColor(category, color);
}

DownTool::AmapVectorColor DomDownToolPreview::MapRenderer::GetGisOverlayColor(
    GisOverlayCategory category) const
{
    return gisOverlay_.GetColor(category);
}

void DomDownToolPreview::MapRenderer::ResetGisOverlayColors()
{
    gisOverlay_.ResetColors();
}

void DomDownToolPreview::MapRenderer::SetExportRadiusMeters(double radiusMeters)
{
    exportRadiusMeters_ = std::isfinite(radiusMeters) ? std::max(0., radiusMeters) : 0.;
}

std::vector<DomDownToolPreview::MapSourceSummary> DomDownToolPreview::MapRenderer::sources() const
{
    std::vector<MapSourceSummary> result;
    result.reserve(sources_.size());
    for (const MapSource& source : sources_)
    {
        result.push_back(source.summary);
    }
    return result;
}

DomDownToolPreview::MapSourceSummary DomDownToolPreview::MapRenderer::activeSource() const
{
    return currentSource().summary;
}

DomDownToolPreview::MapRenderStats DomDownToolPreview::MapRenderer::stats() const
{
    return renderStats_;
}

void DomDownToolPreview::MapRenderer::clearCache()
{
    downloadQueue_.cancelPending();
    imageCache_.clear();
    gisOverlay_.Clear();
}

DomDownToolPreview::MapRenderer::MapSource& DomDownToolPreview::MapRenderer::currentSource()
{
    return sources_.at(activeSourceIndex_);
}

const DomDownToolPreview::MapRenderer::MapSource& DomDownToolPreview::MapRenderer::currentSource()
    const
{
    return sources_.at(activeSourceIndex_);
}

void DomDownToolPreview::MapRenderer::drawMissingTile(SkCanvas& canvas,
                                                      const VisibleTile& tile) const
{
    const bool alternate = ((tile.cacheTile.X + tile.cacheTile.Y) & 1) != 0;
    SkPaint fillPaint;
    fillPaint.setColor(alternate ? SkColorSetRGB(47, 52, 50) : SkColorSetRGB(41, 45, 44));
    canvas.drawRect(SkRect::MakeXYWH(static_cast<float>(tile.left), static_cast<float>(tile.top),
                                     static_cast<float>(tile.size + 0.5),
                                     static_cast<float>(tile.size + 0.5)),
                    fillPaint);

    SkPaint borderPaint;
    borderPaint.setStyle(SkPaint::kStroke_Style);
    borderPaint.setStrokeWidth(1.F);
    borderPaint.setColor(SkColorSetARGB(72, 190, 198, 193));
    canvas.drawRect(SkRect::MakeXYWH(static_cast<float>(tile.left), static_cast<float>(tile.top),
                                     static_cast<float>(tile.size), static_cast<float>(tile.size)),
                    borderPaint);
}

void DomDownToolPreview::MapRenderer::drawCenterMarker(SkCanvas& canvas, int width,
                                                       int height) const
{
    const float centerX = static_cast<float>(width) * 0.5F;
    const float contentHeight =
        std::max(0.F, static_cast<float>(height - toolbarHeight_ - statusBarHeight_));
    const float centerY = static_cast<float>(toolbarHeight_) + contentHeight * 0.5F;

    SkPaint shadowPaint;
    shadowPaint.setStyle(SkPaint::kStroke_Style);
    shadowPaint.setStrokeWidth(4.F);
    shadowPaint.setColor(SkColorSetARGB(180, 24, 27, 26));
    canvas.drawLine(centerX - 10.F, centerY, centerX + 10.F, centerY, shadowPaint);
    canvas.drawLine(centerX, centerY - 10.F, centerX, centerY + 10.F, shadowPaint);

    SkPaint markerPaint;
    markerPaint.setStyle(SkPaint::kStroke_Style);
    markerPaint.setStrokeWidth(2.F);
    markerPaint.setColor(SkColorSetRGB(246, 190, 67));
    canvas.drawLine(centerX - 10.F, centerY, centerX + 10.F, centerY, markerPaint);
    canvas.drawLine(centerX, centerY - 10.F, centerX, centerY + 10.F, markerPaint);
}

void DomDownToolPreview::MapRenderer::drawLocationMarker(SkCanvas& canvas, int width,
                                                          int height) const
{
    if (!locationMarkerWgs84_.has_value())
    {
        return;
    }

    const MapSource& source = currentSource();
    const std::optional<ViewportPoint> point = viewport_.screenPointForLonLat(
        *locationMarkerWgs84_, *source.projection, source.yDirection, kTileSize);
    if (!point.has_value() || point->x < -20. || point->x > static_cast<double>(width) + 20. ||
        point->y < toolbarHeight_ - 30. ||
        point->y > static_cast<double>(height) - statusBarHeight_ + 20.)
    {
        return;
    }

    const float x = static_cast<float>(point->x);
    const float tipY = static_cast<float>(point->y);
    const float centerY = tipY - 14.F;
    SkPaint shadowPaint;
    shadowPaint.setAntiAlias(true);
    shadowPaint.setColor(SkColorSetARGB(90, 0, 0, 0));
    canvas.drawCircle(x + 1.5F, centerY + 2.F, 10.5F, shadowPaint);

    SkPathBuilder pinPathBuilder;
    pinPathBuilder.moveTo(x - 7.F, centerY + 5.F)
        .lineTo(x, tipY)
        .lineTo(x + 7.F, centerY + 5.F)
        .close();
    const SkPath pinPath = pinPathBuilder.detach();
    SkPaint pinPaint;
    pinPaint.setAntiAlias(true);
    pinPaint.setColor(SkColorSetRGB(220, 75, 67));
    pinPaint.setStyle(SkPaint::kFill_Style);
    canvas.drawPath(pinPath, pinPaint);
    canvas.drawCircle(x, centerY, 10.F, pinPaint);

    SkPaint outlinePaint;
    outlinePaint.setAntiAlias(true);
    outlinePaint.setColor(SK_ColorWHITE);
    outlinePaint.setStyle(SkPaint::kStroke_Style);
    outlinePaint.setStrokeWidth(2.F);
    canvas.drawCircle(x, centerY, 9.F, outlinePaint);
    canvas.drawCircle(x, centerY, 3.F, outlinePaint);
}

void DomDownToolPreview::MapRenderer::drawExportBounds(SkCanvas& canvas) const
{
    const std::optional<ExportGeoBounds> bounds =
        calculateExportGeoBounds(viewport_.center(), exportRadiusMeters_);
    if (!bounds.has_value())
    {
        return;
    }

    const MapSource& source = currentSource();
    const DownTool::LonLat corners[] = {
        {bounds->minimumLongitude, bounds->maximumLatitude},
        {bounds->maximumLongitude, bounds->maximumLatitude},
        {bounds->maximumLongitude, bounds->minimumLatitude},
        {bounds->minimumLongitude, bounds->minimumLatitude}};
    std::array<ViewportPoint, 4> points{};
    for (size_t index = 0; index < points.size(); ++index)
    {
        const std::optional<ViewportPoint> point = viewport_.screenPointForLonLat(
            corners[index], *source.projection, source.yDirection, kTileSize);
        if (!point.has_value())
        {
            return;
        }
        points[index] = *point;
    }

    SkPathBuilder builder;
    builder.moveTo(static_cast<float>(points[0].x), static_cast<float>(points[0].y));
    for (size_t index = 1; index < points.size(); ++index)
    {
        builder.lineTo(static_cast<float>(points[index].x), static_cast<float>(points[index].y));
    }
    builder.close();
    const SkPath path = builder.detach();

    SkPaint fillPaint;
    fillPaint.setAntiAlias(true);
    fillPaint.setColor(SkColorSetARGB(28, 255, 204, 74));
    fillPaint.setStyle(SkPaint::kFill_Style);
    canvas.drawPath(path, fillPaint);

    SkPaint strokePaint;
    strokePaint.setAntiAlias(true);
    strokePaint.setColor(SkColorSetARGB(235, 255, 204, 74));
    strokePaint.setStyle(SkPaint::kStroke_Style);
    strokePaint.setStrokeWidth(2.F);
    canvas.drawPath(path, strokePaint);
}
