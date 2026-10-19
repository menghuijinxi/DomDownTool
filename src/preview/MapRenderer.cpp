#include "preview/MapRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>
#include <system_error>
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
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTypeface.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/ports/SkTypeface_win.h"

namespace
{
constexpr uint32_t kTileSize = 256;
constexpr double kEarthRadiusMeters = 6378137.;
constexpr auto kMultidateEmptyTileRetryDelay = std::chrono::seconds(10);

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

std::filesystem::path multidateCacheRoot(std::string_view year, std::string_view key,
                                         std::string_view gbCode,
                                         std::string_view regionName)
{
    return multidateCacheRoot(DownTool::TianDiTuMultidateDown::BuildCacheLayerName(
        year, key, gbCode, regionName));
}

std::filesystem::path emptyTileMarkerPath(const std::filesystem::path& tilePath)
{
    std::filesystem::path markerPath = tilePath;
    markerPath += L".empty";
    return markerPath;
}

bool shouldRetryEmptyTilePlaceholder(
    const std::filesystem::path& tilePath,
    const DomDownToolPreview::TilePersistentFailureState& failureState,
    bool forceRetry)
{
    if (failureState.exhausted)
    {
        return false;
    }
    if (forceRetry)
    {
        return true;
    }

    const std::filesystem::path markerPath = emptyTileMarkerPath(tilePath);
    std::error_code existsError;
    if (!std::filesystem::exists(markerPath, existsError) || existsError)
    {
        return true;
    }

    std::error_code timeError;
    const std::filesystem::file_time_type markerTime =
        std::filesystem::last_write_time(markerPath, timeError);
    if (timeError)
    {
        return true;
    }

    return std::filesystem::file_time_type::clock::now() - markerTime >=
        kMultidateEmptyTileRetryDelay;
}

void removeEmptyTileImageForRetry(const std::filesystem::path& tilePath)
{
    std::error_code removeError;
    std::filesystem::remove(tilePath, removeError);
}

std::string persistentFailureStateText(
    const DomDownToolPreview::TilePersistentFailureState& failureState)
{
    if (failureState.attempts == 0)
    {
        return "请求失败";
    }

    const char* reason = failureState.kind ==
        DomDownToolPreview::TilePersistentFailureKind::EmptyResponse ? "连续空响应" :
        "连续失败";
    return "请求失败（" + std::string(reason) + " " +
        std::to_string(failureState.attempts) + " 次）";
}

size_t regularFileSize(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
    {
        return 0;
    }

    const uintmax_t fileSize = std::filesystem::file_size(path, error);
    if (error)
    {
        return 0;
    }
    const uintmax_t maximumSize = static_cast<uintmax_t>(
        std::numeric_limits<size_t>::max());
    return static_cast<size_t>(std::min(fileSize, maximumSize));
}

bool isHistoryDebugLayer(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    return layer.layerName == "历史叠加";
}

bool isBaseDebugLayer(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    return layer.layerName == "最新底图";
}

bool isEmptyDebugTile(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    return layer.stateText == "服务端空白占位";
}

bool isReadyDebugTile(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    return layer.stateText == "已显示";
}

bool isFailedDebugTile(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    return layer.stateText.find("失败") != std::string::npos;
}

std::string debugTileLabel(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    std::string label = std::to_string(layer.tile.X) + "," + std::to_string(layer.tile.Y);
    if (isEmptyDebugTile(layer))
    {
        label += " empty";
    }
    else if (isFailedDebugTile(layer))
    {
        label += " failed";
    }
    else if (!isReadyDebugTile(layer))
    {
        label += " loading";
    }
    return label;
}

SkColor debugTileFillColor(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    if (isEmptyDebugTile(layer))
    {
        return SkColorSetARGB(132, 87, 24, 22);
    }
    if (isFailedDebugTile(layer))
    {
        return SkColorSetARGB(104, 128, 28, 28);
    }
    if (!isReadyDebugTile(layer))
    {
        return SkColorSetARGB(72, 238, 178, 59);
    }
    return SK_ColorTRANSPARENT;
}

SkColor debugTileStrokeColor(const DomDownToolPreview::MapTileDebugLayer& layer)
{
    if (isEmptyDebugTile(layer))
    {
        return SkColorSetARGB(235, 255, 108, 92);
    }
    if (isFailedDebugTile(layer))
    {
        return SkColorSetARGB(235, 255, 80, 80);
    }
    if (!isReadyDebugTile(layer))
    {
        return SkColorSetARGB(235, 255, 194, 82);
    }
    if (isHistoryDebugLayer(layer))
    {
        return SkColorSetARGB(210, 235, 248, 243);
    }
    return SkColorSetARGB(190, 129, 226, 211);
}

sk_sp<SkTypeface> tileDebugTypeface()
{
    static const sk_sp<SkTypeface> typeface = []() -> sk_sp<SkTypeface>
    {
        sk_sp<SkFontMgr> fontManager = SkFontMgr_New_GDI();
        if (!fontManager)
        {
            return nullptr;
        }

        const char* fontPaths[] = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/simhei.ttf",
            "C:/Windows/Fonts/arial.ttf"};
        for (const char* fontPath : fontPaths)
        {
            sk_sp<SkTypeface> typefaceFromFile = fontManager->makeFromFile(fontPath);
            if (typefaceFromFile)
            {
                return typefaceFromFile;
            }
        }

        return fontManager->matchFamilyStyle("Arial", SkFontStyle());
    }();
    return typeface;
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
              std::move(multidateProjection), TileYAxisDirection::Down, 1., 18.);
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
    renderStats_ = MapRenderStats{};
    if (tileDebugEnabled_)
    {
        tileDebugEntries_.clear();
    }
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
        size_t emptyTiles = 0;
    };
    auto drawTileLayer = [&](std::string_view layerName,
                             DownTool::DownToolBase& projection,
                             TileYAxisDirection yDirection,
                             const std::filesystem::path& cacheRoot,
                             bool drawMissingTiles,
                             const TileDownloader* customDownloader = nullptr) -> TileLayerResult
    {
        const std::vector<VisibleTile> visibleTiles =
            viewport_.visibleTiles(projection, yDirection, kTileSize, 1);
        downloadJobs.reserve(downloadJobs.size() + visibleTiles.size());
        TileLayerResult result;
        result.visibleTiles = visibleTiles.size();
        const auto rememberTileDebug =
            [&](const VisibleTile& visibleTile, const std::filesystem::path& tilePath,
                std::string stateText, int imageWidth, int imageHeight)
        {
            if (!tileDebugEnabled_)
            {
                return;
            }

            MapTileDebugLayer layer;
            layer.layerName = std::string(layerName);
            layer.tile = visibleTile.cacheTile;
            layer.cachePath = tilePath;
            layer.requestUrl = projection.BuildTileHttpRequest(visibleTile.cacheTile).url;
            layer.stateText = std::move(stateText);
            layer.fileBytes = regularFileSize(tilePath);
            layer.imageWidth = imageWidth;
            layer.imageHeight = imageHeight;
            tileDebugEntries_.push_back(
                TileDebugEntry{visibleTile.left, visibleTile.top, visibleTile.size,
                               std::move(layer)});
        };
        const auto queueDownload =
            [&](const VisibleTile& visibleTile, const std::filesystem::path& tilePath)
        {
            if (TileDownloadQueue::HasExhaustedPersistentFailures(tilePath))
            {
                return;
            }

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
        };
        for (const VisibleTile& visibleTile : visibleTiles)
        {
            const std::filesystem::path tilePath =
                resolveTilePath(cacheRoot, visibleTile.cacheTile);
            sk_sp<SkImage> image = imageCache_.findOrRequest(tilePath, canvas);
            if (!image)
            {
                const TilePersistentFailureState failureState =
                    TileDownloadQueue::GetPersistentFailureState(tilePath);
                if (failureState.exhausted)
                {
                    rememberTileDebug(visibleTile, tilePath,
                                      persistentFailureStateText(failureState), 0, 0);
                    if (drawMissingTiles)
                    {
                        drawMissingTile(canvas, visibleTile);
                    }
                    continue;
                }

                const bool knownMissing = imageCache_.isKnownMissing(tilePath);
                if (knownMissing || failureState.attempts != 0)
                {
                    queueDownload(visibleTile, tilePath);
                }
                const size_t fileBytes = regularFileSize(tilePath);
                rememberTileDebug(visibleTile, tilePath,
                                  knownMissing ? "已加入下载队列" :
                                      fileBytes > 0 ? "正在解码缓存" : "正在检查缓存",
                                  0, 0);
                if (drawMissingTiles)
                {
                    drawMissingTile(canvas, visibleTile);
                }
                continue;
            }

            const SkRect sourceRect = SkRect::MakeIWH(image->width(), image->height());
            const bool transparentPlaceholder = !drawMissingTiles && image->width() == 1 &&
                image->height() == 1;
            if (transparentPlaceholder)
            {
                const TilePersistentFailureState failureState =
                    TileDownloadQueue::GetPersistentFailureState(tilePath);
                if (failureState.exhausted)
                {
                    rememberTileDebug(visibleTile, tilePath,
                                      persistentFailureStateText(failureState), image->width(),
                                      image->height());
                    continue;
                }

                if (shouldRetryEmptyTilePlaceholder(tilePath, failureState,
                                                    forceRetryKnownTileFailures_))
                {
                    imageCache_.invalidate(tilePath);
                    removeEmptyTileImageForRetry(tilePath);
                    queueDownload(visibleTile, tilePath);
                    rememberTileDebug(visibleTile, tilePath, "透明占位重试", image->width(),
                                      image->height());
                    continue;
                }

                result.readyTiles++;
                result.emptyTiles++;
                rememberTileDebug(visibleTile, tilePath, "服务端空白占位", image->width(),
                                  image->height());
                continue;
            }
            result.readyTiles++;
            rememberTileDebug(visibleTile, tilePath, "已显示", image->width(), image->height());
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
                "最新底图", *tianDiTuMultidateLatestProjection_, source.yDirection,
                multidateCacheRoot("latest"), true);
            const TileLayerResult historyLayer = drawTileLayer(
                "历史叠加", *source.projection, source.yDirection, source.summary.cacheRoot, false,
                &tianDiTuMultidateTileDownloader_);
            renderStats_.visibleTiles = baseLayer.visibleTiles + historyLayer.visibleTiles;
            renderStats_.readyTiles = baseLayer.readyTiles + historyLayer.readyTiles;
            renderStats_.multidateBaseVisibleTiles = baseLayer.visibleTiles;
            renderStats_.multidateBaseReadyTiles = baseLayer.readyTiles;
            renderStats_.multidateHistoryVisibleTiles = historyLayer.visibleTiles;
            renderStats_.multidateHistoryReadyTiles = historyLayer.readyTiles;
            renderStats_.multidateHistoryEmptyTiles = historyLayer.emptyTiles;
        }
        else
        {
            const TileLayerResult layer = drawTileLayer(
                "瓦片", *source.projection, source.yDirection, source.summary.cacheRoot, true);
            renderStats_.visibleTiles = layer.visibleTiles;
            renderStats_.readyTiles = layer.readyTiles;
        }
    }

    downloadQueue_.updateVisibleRequests(downloadJobs);
    forceRetryKnownTileFailures_ = false;

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

    drawTileDebugOverlay(canvas);
    drawExportBounds(canvas);
    drawRangeEditor(canvas);
    drawCenterMarker(canvas, width, height);
    drawLocationMarker(canvas, width, height);
    renderStats_.cache = imageCache_.stats();
    renderStats_.downloads = downloadQueue_.stats();
}

bool DomDownToolPreview::MapRenderer::panBy(double deltaX, double deltaY)
{
    MapSource& source = currentSource();
    const bool moved = viewport_.panBy(deltaX, deltaY, *source.projection, source.yDirection,
                                       kTileSize);
    if (moved)
    {
        tileDebugEntries_.clear();
    }
    return moved;
}

bool DomDownToolPreview::MapRenderer::zoomAt(double screenX, double screenY, double zoomDelta)
{
    MapSource& source = currentSource();
    const bool zoomed = viewport_.zoomAt(screenX, screenY, zoomDelta, source.summary.minimumZoom,
                                        source.summary.maximumZoom, *source.projection,
                                        source.yDirection, kTileSize);
    if (zoomed)
    {
        tileDebugEntries_.clear();
    }
    return zoomed;
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
    tileDebugEntries_.clear();
    return true;
}

void DomDownToolPreview::MapRenderer::setView(const DownTool::LonLat& centerWgs84, double zoom)
{
    const MapSource& source = currentSource();
    viewport_.setCenter(centerWgs84);
    viewport_.setZoom(std::clamp(zoom, source.summary.minimumZoom, source.summary.maximumZoom));
    downloadQueue_.cancelPending();
    tileDebugEntries_.clear();
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
    tileDebugEntries_.clear();
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
    tileDebugEntries_.clear();
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
    sourceIterator->summary.cacheRoot = multidateCacheRoot(year, key, gbCode, name);
    projection->SetCachePath(pathToUtf8(sourceIterator->summary.cacheRoot));
    downloadQueue_.cancelPending();
    tileDebugEntries_.clear();
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
    tileDebugEntries_.clear();
    return true;
}

void DomDownToolPreview::MapRenderer::SetTianDiTuMultidateTileDownloader(
    TileDownloader downloader)
{
    tianDiTuMultidateTileDownloader_ = std::move(downloader);
    downloadQueue_.cancelPending();
    tileDebugEntries_.clear();
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

void DomDownToolPreview::MapRenderer::SetExportPolygonsWgs84(
    const DownTool::Wgs84Polygons& polygons)
{
    exportPolygonsWgs84_ = polygons;
}

void DomDownToolPreview::MapRenderer::SetDownloadRegions(
    const DownTool::DownloadRegions& regions, const std::string& selectedId)
{
    downloadRegions_ = regions;
    selectedRegionId_ = selectedId;
}

void DomDownToolPreview::MapRenderer::SetDraftRegion(
    const DownTool::Wgs84Ring& vertices, bool excluded, bool closed)
{
    draftRegion_ = vertices;
    draftRegionExcluded_ = excluded;
    draftRegionClosed_ = closed;
}

std::optional<DomDownToolPreview::ViewportPoint>
DomDownToolPreview::MapRenderer::ScreenPointForLonLat(
    const DownTool::LonLat& coordinate) const
{
    const auto& source = currentSource();
    return viewport_.screenPointForLonLat(
        coordinate, *source.projection, source.yDirection, kTileSize);
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

void DomDownToolPreview::MapRenderer::SetTileDebugEnabled(bool enabled)
{
    if (tileDebugEnabled_ == enabled)
    {
        return;
    }

    tileDebugEnabled_ = enabled;
    tileDebugEntries_.clear();
}

std::optional<DomDownToolPreview::MapTileDebugInfo>
DomDownToolPreview::MapRenderer::TileDebugAt(double screenX, double screenY) const
{
    if (!tileDebugEnabled_)
    {
        return std::nullopt;
    }

    const std::optional<DownTool::LonLat> coordinate = lonLatAt(screenX, screenY);
    if (!coordinate.has_value())
    {
        return std::nullopt;
    }

    MapTileDebugInfo result;
    result.coordinate = *coordinate;
    for (const TileDebugEntry& entry : tileDebugEntries_)
    {
        if (screenX >= entry.left && screenX <= entry.left + entry.size &&
            screenY >= entry.top && screenY <= entry.top + entry.size)
        {
            result.layers.push_back(entry.layer);
        }
    }
    return result;
}

void DomDownToolPreview::MapRenderer::clearCache()
{
    downloadQueue_.cancelPending();
    const MapSource& source = currentSource();
    TileDownloadQueue::ClearPersistentFailureStatesUnder(source.summary.cacheRoot);
    if (source.summary.id == "tianditu.multidate")
    {
        TileDownloadQueue::ClearPersistentFailureStatesUnder(multidateCacheRoot("latest"));
    }
    imageCache_.clear();
    gisOverlay_.Clear();
    tileDebugEntries_.clear();
    forceRetryKnownTileFailures_ = true;
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

void DomDownToolPreview::MapRenderer::drawTileDebugOverlay(SkCanvas& canvas) const
{
    if (!tileDebugEnabled_ || tileDebugEntries_.empty())
    {
        return;
    }

    const bool hasHistoryLayer = std::any_of(
        tileDebugEntries_.begin(), tileDebugEntries_.end(),
        [](const TileDebugEntry& entry) { return isHistoryDebugLayer(entry.layer); });

    SkFont labelFont(tileDebugTypeface(), 16.F);
    labelFont.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    labelFont.setHinting(SkFontHinting::kSlight);
    labelFont.setLinearMetrics(true);
    labelFont.setBaselineSnap(true);
    labelFont.setSubpixel(true);
    SkPaint tileFillPaint;
    tileFillPaint.setAntiAlias(false);
    tileFillPaint.setStyle(SkPaint::kFill_Style);

    SkPaint tileStrokePaint;
    tileStrokePaint.setAntiAlias(false);
    tileStrokePaint.setStyle(SkPaint::kStroke_Style);
    tileStrokePaint.setStrokeWidth(1.F);

    SkPaint labelBackgroundPaint;
    labelBackgroundPaint.setColor(SkColorSetARGB(170, 12, 16, 15));
    labelBackgroundPaint.setStyle(SkPaint::kFill_Style);

    SkPaint labelTextPaint;
    labelTextPaint.setAntiAlias(true);
    labelTextPaint.setColor(SK_ColorWHITE);

    for (const TileDebugEntry& entry : tileDebugEntries_)
    {
        if (hasHistoryLayer && isBaseDebugLayer(entry.layer))
        {
            continue;
        }

        const float tileLeft = std::floor(static_cast<float>(entry.left));
        const float tileTop = std::floor(static_cast<float>(entry.top));
        const float tileSize = std::ceil(static_cast<float>(entry.size));
        const SkRect tileRect = SkRect::MakeXYWH(tileLeft, tileTop, tileSize, tileSize);
        const SkColor fillColor = debugTileFillColor(entry.layer);
        if (SkColorGetA(fillColor) > 0)
        {
            tileFillPaint.setColor(fillColor);
            canvas.drawRect(tileRect, tileFillPaint);
        }

        tileStrokePaint.setColor(debugTileStrokeColor(entry.layer));
        canvas.drawRect(tileRect, tileStrokePaint);

        const float labelHeight = std::min(24.F, tileRect.height());
        if (labelHeight < 10.F || tileRect.width() < 24.F)
        {
            continue;
        }

        const SkRect labelRect = SkRect::MakeXYWH(tileRect.left(), tileRect.top(),
                                                 tileRect.width(), labelHeight);
        canvas.drawRect(labelRect, labelBackgroundPaint);

        SkAutoCanvasRestore labelRestore(&canvas, true);
        canvas.clipRect(tileRect, SkClipOp::kIntersect, false);
        const std::string label = debugTileLabel(entry.layer);
        const float baseline = tileRect.top() + std::min(18.F, labelHeight - 4.F);
        canvas.drawString(label.c_str(), tileRect.left() + 4.F, baseline, labelFont,
                          labelTextPaint);
    }
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
    SkPaint fillPaint;
    fillPaint.setAntiAlias(true);
    fillPaint.setColor(SkColorSetARGB(28, 255, 204, 74));
    fillPaint.setStyle(SkPaint::kFill_Style);

    SkPaint strokePaint;
    strokePaint.setAntiAlias(true);
    strokePaint.setColor(SkColorSetARGB(235, 255, 204, 74));
    strokePaint.setStyle(SkPaint::kStroke_Style);
    strokePaint.setStrokeWidth(2.F);

    const auto drawRangePath = [&](const SkPath& path)
    {
        canvas.save();
        for (const auto& region : downloadRegions_)
        {
            if (!region.GetEnabled() ||
                region.GetRole() != DownTool::DownloadRegionRole::ExcludeGis)
            {
                continue;
            }
            SkPathBuilder excluded;
            bool started = false;
            for (const auto& vertex : region.GetVertices())
            {
                if (const auto point = ScreenPointForLonLat(vertex))
                {
                    if (!started)
                    {
                        excluded.moveTo(static_cast<float>(point->x),
                            static_cast<float>(point->y));
                        started = true;
                    }
                    else
                    {
                        excluded.lineTo(static_cast<float>(point->x),
                            static_cast<float>(point->y));
                    }
                }
            }
            excluded.close();
            canvas.clipPath(excluded.detach(), SkClipOp::kDifference, true);
        }
        canvas.drawPath(path, fillPaint);
        canvas.restore();
        canvas.drawPath(path, strokePaint);
    };

    const MapSource& source = currentSource();
    if (!exportPolygonsWgs84_.empty())
    {
        for (const DownTool::Wgs84Ring& ring : exportPolygonsWgs84_)
        {
            if (ring.size() < 3)
            {
                continue;
            }
            SkPathBuilder builder;
            bool hasPoint = false;
            bool valid = true;
            for (const DownTool::LonLat& coordinate : ring)
            {
                const std::optional<ViewportPoint> point =
                    viewport_.screenPointForLonLat(
                        coordinate,
                        *source.projection,
                        source.yDirection,
                        kTileSize);
                if (!point.has_value())
                {
                    valid = false;
                    break;
                }
                if (!hasPoint)
                {
                    builder.moveTo(
                        static_cast<float>(point->x),
                        static_cast<float>(point->y));
                    hasPoint = true;
                }
                else
                {
                    builder.lineTo(
                        static_cast<float>(point->x),
                        static_cast<float>(point->y));
                }
            }
            if (!valid || !hasPoint)
            {
                continue;
            }
            builder.close();
            const SkPath path = builder.detach();
            drawRangePath(path);
        }
        return;
    }

    const std::optional<ExportGeoBounds> bounds =
        calculateExportGeoBounds(viewport_.center(), exportRadiusMeters_);
    if (!bounds.has_value())
    {
        return;
    }

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

    drawRangePath(path);
}

void DomDownToolPreview::MapRenderer::drawRangeEditor(SkCanvas& canvas) const
{
    const auto drawRegion = [&](const DownTool::Wgs84Ring& vertices,
                               bool excluded, bool selected, bool closed)
    {
        SkPathBuilder builder;
        std::vector<ViewportPoint> points;
        for (const auto& vertex : vertices)
        {
            if (const auto point = ScreenPointForLonLat(vertex))
            {
                if (points.empty())
                {
                    builder.moveTo(static_cast<float>(point->x), static_cast<float>(point->y));
                }
                else
                {
                    builder.lineTo(static_cast<float>(point->x), static_cast<float>(point->y));
                }
                points.push_back(*point);
            }
        }
        if (closed)
        {
            builder.close();
        }
        const SkPath path = builder.detach();
        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setColor(excluded ? SkColorSetRGB(243, 112, 112) :
            SkColorSetRGB(255, 204, 74));
        stroke.setStrokeWidth(selected ? 3.F : 2.F);
        if (excluded)
        {
            const float intervals[] = {7.F, 5.F};
            stroke.setPathEffect(SkDashPathEffect::Make(intervals, 0.F));
            if (closed)
            {
                canvas.save();
                canvas.clipPath(path, true);
                SkPaint hatch;
                hatch.setColor(SkColorSetARGB(72, 243, 112, 112));
                hatch.setStrokeWidth(1.F);
                const SkRect bounds = canvas.getLocalClipBounds();
                for (float x = bounds.left() - bounds.height(); x < bounds.right(); x += 12.F)
                {
                    canvas.drawLine(x, bounds.bottom(), x + bounds.height(),
                        bounds.top(), hatch);
                }
                canvas.restore();
            }
        }
        canvas.drawPath(path, stroke);
        if (selected)
        {
            SkPaint handle;
            handle.setAntiAlias(true);
            handle.setColor(SkColorSetRGB(245, 247, 246));
            for (const auto& point : points)
            {
                canvas.drawCircle(static_cast<float>(point.x),
                    static_cast<float>(point.y), 4.F, handle);
            }
        }
    };
    for (const auto& region : downloadRegions_)
    {
        if (region.GetEnabled())
        {
            const bool selected = region.GetId() == selectedRegionId_;
            const bool excluded = region.GetRole() == DownTool::DownloadRegionRole::ExcludeGis;
            if (excluded || selected)
            {
                drawRegion(region.GetVertices(), excluded, selected, true);
            }
        }
    }
    if (!draftRegion_.empty())
    {
        drawRegion(draftRegion_, draftRegionExcluded_, true, draftRegionClosed_);
    }
}
