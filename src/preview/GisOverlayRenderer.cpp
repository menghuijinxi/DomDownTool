#include "preview/GisOverlayRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "AmapVectorTileGrid.h"
#include "GCJ02GoogleDownBase.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"

#include "preview/SearchTrace.h"

namespace
{
using DomDownToolPreview::GisOverlayCategory;
using DownTool::AmapVectorColor;
using DownTool::AmapVectorFeature;
using DownTool::AmapVectorGeometryType;

constexpr double kEarthRadiusMeters = 6378137.;
constexpr double kPreloadRadiusFactor = 1.5;
constexpr auto kRetryDelay = std::chrono::seconds(5);
constexpr std::array kAllDrawOrder = {
    GisOverlayCategory::Other,
    GisOverlayCategory::Green,
    GisOverlayCategory::Water,
    GisOverlayCategory::Road,
    GisOverlayCategory::Building,
    GisOverlayCategory::Subway,
    GisOverlayCategory::Highway,
    GisOverlayCategory::Point,
    GisOverlayCategory::Boundary,
};

double distanceMeters(const DownTool::LonLat& left, const DownTool::LonLat& right)
{
    const double degreesToRadians = std::numbers::pi / 180.;
    const double leftLatitude = left.lat * degreesToRadians;
    const double rightLatitude = right.lat * degreesToRadians;
    const double latitudeDelta = rightLatitude - leftLatitude;
    const double longitudeDelta = (right.lon - left.lon) * degreesToRadians;
    const double latitudeSin = std::sin(latitudeDelta * 0.5);
    const double longitudeSin = std::sin(longitudeDelta * 0.5);
    const double haversine = latitudeSin * latitudeSin + std::cos(leftLatitude) *
        std::cos(rightLatitude) * longitudeSin * longitudeSin;
    return 2. * kEarthRadiusMeters *
        std::asin(std::sqrt(std::clamp(haversine, 0., 1.)));
}

constexpr size_t kCategoryCount =
    static_cast<size_t>(GisOverlayCategory::Other) + static_cast<size_t>(1);

size_t categoryIndex(GisOverlayCategory category)
{
    return static_cast<size_t>(category);
}

bool isWaterFeatureCode(uint32_t mainKey, uint32_t subKey)
{
    if (mainKey == 30001)
    {
        return subKey == 2 || subKey == 6 || subKey == 11 || subKey == 13;
    }
    if (mainKey == 20014)
    {
        return subKey == 1 || subKey == 2;
    }
    if (mainKey == 10002)
    {
        return subKey == 13 || subKey == 38;
    }
    return false;
}

bool isGreenFeatureCode(uint32_t mainKey, uint32_t subKey)
{
    return mainKey == 30001 && subKey == 3;
}

bool hasNameContaining(const AmapVectorFeature& feature, std::string_view text)
{
    for (const DownTool::AmapVectorName& name : feature.names)
    {
        if (name.text.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool isHighwayFeature(const AmapVectorFeature& feature)
{
    if (feature.layerName != "road")
    {
        return false;
    }
    if (feature.mainKey == 20001 || feature.mainKey == 20002)
    {
        return true;
    }
    return feature.mainKey == 20018 &&
        (hasNameContaining(feature, "高速") || hasNameContaining(feature, "快速路"));
}

bool isBoundaryFeature(const AmapVectorFeature& feature)
{
    if (feature.layerName != "region-line")
    {
        return false;
    }
    // region-line 是容器图层；20010 为铁路、20014 为河流/水系线，其余当前样本为边界类线。
    return feature.mainKey != 20010 && feature.mainKey != 20014;
}

SkColor toSkColor(const AmapVectorColor& color)
{
    return SkColorSetARGB(color.alpha, color.red, color.green, color.blue);
}

float lineWidthForCategory(GisOverlayCategory category)
{
    switch (category)
    {
    case GisOverlayCategory::Highway:
        return 5.F;
    case GisOverlayCategory::Subway:
        return 4.F;
    case GisOverlayCategory::Road:
        return 3.F;
    default:
        return 2.F;
    }
}

bool isFeatureVisibleAtZoom(uint32_t minZoom, uint32_t maxZoom, uint32_t zoom)
{
    return (minZoom == 0 || zoom >= minZoom) && (maxZoom == 0 || zoom <= maxZoom);
}

bool boundsIntersect(
    bool initialized, double minLon, double maxLon, double minLat, double maxLat,
    const DomDownToolPreview::ViewportGeoBounds& viewportBounds)
{
    if (!initialized)
    {
        return true;
    }
    if (maxLat < viewportBounds.minLat || minLat > viewportBounds.maxLat)
    {
        return false;
    }
    // A viewport crossing the antimeridian is represented as a world-spanning interval here.
    if (viewportBounds.maxLon - viewportBounds.minLon >= 180.)
    {
        return true;
    }
    return maxLon >= viewportBounds.minLon && minLon <= viewportBounds.maxLon;
}

const char* categoryName(GisOverlayCategory category)
{
    switch (category)
    {
    case GisOverlayCategory::None:
        return "none";
    case GisOverlayCategory::All:
        return "all";
    case GisOverlayCategory::Road:
        return "road";
    case GisOverlayCategory::Water:
        return "water";
    case GisOverlayCategory::Green:
        return "green";
    case GisOverlayCategory::Highway:
        return "highway";
    case GisOverlayCategory::Subway:
        return "subway";
    case GisOverlayCategory::Building:
        return "building";
    case GisOverlayCategory::Point:
        return "point";
    case GisOverlayCategory::Boundary:
        return "boundary";
    case GisOverlayCategory::Other:
        return "other";
    }
    return "unknown";
}

bool isKnownCategory(GisOverlayCategory category)
{
    return static_cast<unsigned>(category) <= static_cast<unsigned>(GisOverlayCategory::Other);
}
} // namespace

struct DomDownToolPreview::GisOverlayRenderer::PreparedData
{
    struct Bounds
    {
        double minLon = 0.;
        double maxLon = 0.;
        double minLat = 0.;
        double maxLat = 0.;
        bool initialized = false;
    };

    struct Feature
    {
        DownTool::AmapVectorGeometryType geometryType = DownTool::AmapVectorGeometryType::Point;
        std::vector<std::vector<DownTool::LonLat>> geometryParts;
        Bounds bounds;
        uint32_t minZoom = 0;
        uint32_t maxZoom = 0;
    };

    std::array<std::vector<Feature>, kCategoryCount> featuresByCategory;
};

struct DomDownToolPreview::GisOverlayRenderer::PreparedRenderCache
{
    std::shared_ptr<const PreparedData> data;
    const DownTool::DownToolBase* projection = nullptr;
    TileYAxisDirection yDirection = TileYAxisDirection::Down;
    uint32_t tileSize = 0;
    uint32_t sourceZoom = 0;
    std::array<std::vector<SkPath>, kCategoryCount> paths;
    std::array<std::vector<uint8_t>, kCategoryCount> initialized;
};

DomDownToolPreview::GisOverlayRenderer::GisOverlayRenderer(
    std::function<void()> requestRedraw)
    : requestRedraw_(std::move(requestRedraw))
{
    const GisOverlayCategory categories[] = {
        GisOverlayCategory::Road,
        GisOverlayCategory::Water,
        GisOverlayCategory::Green,
        GisOverlayCategory::Highway,
        GisOverlayCategory::Subway,
        GisOverlayCategory::Building,
        GisOverlayCategory::Point,
        GisOverlayCategory::Boundary,
        GisOverlayCategory::Other};
    for (const GisOverlayCategory category : categories)
    {
        colors_.emplace(category, defaultColor(category));
    }
    writeSearchTrace("GIS", "constructed",
                     fmt::format("category={} raw={}", categoryName(category_),
                                 static_cast<unsigned>(category_)));
    worker_ = std::thread([this] { workerLoop(); });
}

DomDownToolPreview::GisOverlayRenderer::~GisOverlayRenderer()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        pendingRequest_.reset();
    }
    condition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void DomDownToolPreview::GisOverlayRenderer::SetCategory(GisOverlayCategory category)
{
    GisOverlayCategory previousCategory = GisOverlayCategory::None;
    uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        previousCategory = category_;
        if (category_ == category)
        {
            return;
        }
        category_ = category;
        if (category == GisOverlayCategory::None)
        {
            pendingRequest_.reset();
            ++generation_;
            retryAfter_ = {};
            loading_ = false;
        }
        generation = generation_;
    }
    writeSearchTrace("GIS", "category_changed",
                     fmt::format("from={} to={} generation={}", categoryName(previousCategory),
                                 categoryName(category), generation));
    condition_.notify_all();
    if (requestRedraw_)
    {
        requestRedraw_();
    }
}

DomDownToolPreview::GisOverlayCategory
DomDownToolPreview::GisOverlayRenderer::GetCategory() const
{
    const std::lock_guard lock(mutex_);
    return category_;
}

void DomDownToolPreview::GisOverlayRenderer::SetColor(
    GisOverlayCategory category, const DownTool::AmapVectorColor& color)
{
    if (category == GisOverlayCategory::None || category == GisOverlayCategory::All)
    {
        return;
    }
    {
        const std::lock_guard lock(mutex_);
        colors_[category] = color;
    }
    if (requestRedraw_)
    {
        requestRedraw_();
    }
}

DownTool::AmapVectorColor DomDownToolPreview::GisOverlayRenderer::GetColor(
    GisOverlayCategory category) const
{
    const std::lock_guard lock(mutex_);
    const auto iterator = colors_.find(category);
    return iterator == colors_.end() ? defaultColor(category) : iterator->second;
}

void DomDownToolPreview::GisOverlayRenderer::UpdateContext(
    const DownTool::LonLat& centerWgs84, double radiusMeters, uint32_t zoom)
{
    if (!std::isfinite(centerWgs84.lon) || !std::isfinite(centerWgs84.lat) ||
        !std::isfinite(radiusMeters) || radiusMeters <= 0.)
    {
        return;
    }

    std::string requestDetail;
    {
        const std::lock_guard lock(mutex_);
        if (!isKnownCategory(category_))
        {
            writeSearchTrace("GIS", "invalid_category",
                             fmt::format("raw={} reset=none", static_cast<unsigned>(category_)));
            category_ = GisOverlayCategory::None;
            pendingRequest_.reset();
            ++generation_;
            loading_ = false;
        }
        if (category_ == GisOverlayCategory::None || stopping_)
        {
            return;
        }

        const auto coversVisibleArea = [&](const std::optional<LoadRequest>& request)
        {
            return request.has_value() &&
                DownTool::AmapVectorTileGrid::ResolveDataZoom(request->zoom) ==
                    DownTool::AmapVectorTileGrid::ResolveDataZoom(zoom) &&
                distanceMeters(request->centerWgs84, centerWgs84) + radiusMeters <=
                    request->radiusMeters;
        };
        if (coversVisibleArea(loadedContext_) ||
            (pendingRequest_.has_value() && pendingRequest_->generation == generation_ &&
             coversVisibleArea(pendingRequest_)) ||
            (activeRequest_.has_value() && activeRequest_->generation == generation_ &&
             coversVisibleArea(activeRequest_)) ||
            std::chrono::steady_clock::now() < retryAfter_)
        {
            return;
        }
        pendingRequest_ = LoadRequest{
            centerWgs84, radiusMeters * kPreloadRadiusFactor, zoom, ++generation_};
        loading_ = true;
        requestDetail =
            fmt::format("category={} lon={:.6f} lat={:.6f} radius={:.1f} zoom={} generation={}",
                        categoryName(category_), centerWgs84.lon, centerWgs84.lat,
                        radiusMeters * kPreloadRadiusFactor, zoom, generation_);
    }
    writeSearchTrace("GIS", "load_requested", requestDetail);
    condition_.notify_one();
}

void DomDownToolPreview::GisOverlayRenderer::Draw(
    SkCanvas& canvas, const MapViewport& viewport, const DownTool::DownToolBase& projection,
    TileYAxisDirection yDirection, uint32_t tileSize)
{
    std::shared_ptr<const PreparedData> preparedData;
    GisOverlayCategory selectedCategory;
    std::map<GisOverlayCategory, DownTool::AmapVectorColor> colors;
    {
        const std::lock_guard lock(mutex_);
        selectedCategory = category_;
        if (selectedCategory == GisOverlayCategory::None || !preparedData_)
        {
            return;
        }
        preparedData = preparedData_;
        colors = colors_;
    }

    const std::optional<ViewportTransform> transform =
        viewport.makeTransform(projection, yDirection, tileSize);
    const std::optional<ViewportGeoBounds> viewportBounds =
        viewport.visibleGeoBounds(projection, yDirection, tileSize);
    if (!transform.has_value())
    {
        return;
    }

    if (!renderCache_ || renderCache_->data.get() != preparedData.get() ||
        renderCache_->projection != &projection || renderCache_->yDirection != yDirection ||
        renderCache_->tileSize != tileSize || renderCache_->sourceZoom != transform->sourceZoom)
    {
        renderCache_ = std::make_unique<PreparedRenderCache>();
        renderCache_->data = preparedData;
        renderCache_->projection = &projection;
        renderCache_->yDirection = yDirection;
        renderCache_->tileSize = tileSize;
        renderCache_->sourceZoom = transform->sourceZoom;
        for (size_t index = 0; index < kCategoryCount; ++index)
        {
            renderCache_->paths[index].resize(preparedData->featuresByCategory[index].size());
            renderCache_->initialized[index].assign(
                preparedData->featuresByCategory[index].size(), uint8_t{0});
        }
    }

    const uint32_t zoom = viewport.sourceZoom();
    const auto drawCategory = [&](GisOverlayCategory category)
    {
        const size_t index = categoryIndex(category);
        const std::vector<PreparedData::Feature>& features =
            preparedData->featuresByCategory[index];
        std::vector<SkPath>& paths = renderCache_->paths[index];
        std::vector<uint8_t>& initialized = renderCache_->initialized[index];
        const auto colorIterator = colors.find(category);
        const AmapVectorColor color = colorIterator == colors.end() ?
            defaultColor(category) : colorIterator->second;
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(toSkColor(color));
        if (category == GisOverlayCategory::Point)
        {
            paint.setStyle(SkPaint::kFill_Style);
        }

        for (size_t featureIndex = 0; featureIndex < features.size(); ++featureIndex)
        {
            const PreparedData::Feature& feature = features[featureIndex];
            if (!isFeatureVisibleAtZoom(feature.minZoom, feature.maxZoom, zoom) ||
                (viewportBounds.has_value() &&
                 !boundsIntersect(feature.bounds.initialized, feature.bounds.minLon,
                                  feature.bounds.maxLon, feature.bounds.minLat,
                                  feature.bounds.maxLat, *viewportBounds)))
            {
                continue;
            }

            if (feature.geometryType == AmapVectorGeometryType::Point)
            {
                const float radius = static_cast<float>(5. / transform->displayScale);
                for (const std::vector<DownTool::LonLat>& part : feature.geometryParts)
                {
                    for (const DownTool::LonLat& coordinate : part)
                    {
                        const std::optional<ViewportSourcePoint> point =
                            transform->sourcePointForLonLat(coordinate);
                        if (point.has_value())
                        {
                            canvas.drawCircle(static_cast<float>(point->x),
                                              static_cast<float>(point->y), radius, paint);
                        }
                    }
                }
                continue;
            }

            if (!initialized[featureIndex])
            {
                SkPathBuilder pathBuilder;
                pathBuilder.setFillType(SkPathFillType::kEvenOdd);
                for (const std::vector<DownTool::LonLat>& part : feature.geometryParts)
                {
                    bool hasPartPoint = false;
                    for (const DownTool::LonLat& coordinate : part)
                    {
                        const std::optional<ViewportSourcePoint> point =
                            transform->sourcePointForLonLat(coordinate);
                        if (!point.has_value())
                        {
                            continue;
                        }
                        if (!hasPartPoint)
                        {
                            pathBuilder.moveTo(static_cast<float>(point->x),
                                               static_cast<float>(point->y));
                            hasPartPoint = true;
                        }
                        else
                        {
                            pathBuilder.lineTo(static_cast<float>(point->x),
                                               static_cast<float>(point->y));
                        }
                    }
                    if (hasPartPoint && feature.geometryType == AmapVectorGeometryType::Polygon)
                    {
                        pathBuilder.close();
                    }
                }
                paths[featureIndex] = pathBuilder.detach();
                initialized[featureIndex] = 1;
            }
            const SkPath& path = paths[featureIndex];
            if (path.isEmpty())
            {
                continue;
            }

            paint.setStyle(feature.geometryType == AmapVectorGeometryType::Polygon
                               ? SkPaint::kFill_Style
                               : SkPaint::kStroke_Style);
            if (feature.geometryType == AmapVectorGeometryType::LineString)
            {
                paint.setStrokeWidth(lineWidthForCategory(category) /
                                     static_cast<float>(transform->displayScale));
                paint.setStrokeCap(SkPaint::kRound_Cap);
            }
            canvas.drawPath(path, paint);
        }
    };

    canvas.save();
    canvas.translate(
        static_cast<float>(transform->centerScreenX - transform->centerSourceX *
                           transform->displayScale),
        static_cast<float>(transform->centerScreenY - transform->centerSourceY *
                           transform->displayScale));
    canvas.scale(static_cast<float>(transform->displayScale),
                 static_cast<float>(transform->displayScale));
    if (selectedCategory == GisOverlayCategory::All)
    {
        for (const GisOverlayCategory category : kAllDrawOrder)
        {
            drawCategory(category);
        }
    }
    else
    {
        drawCategory(selectedCategory);
    }
    canvas.restore();
}

void DomDownToolPreview::GisOverlayRenderer::Clear()
{
    {
        const std::lock_guard lock(mutex_);
        preparedData_.reset();
        renderCache_.reset();
        pendingRequest_.reset();
        loadedContext_.reset();
        ++generation_;
        retryAfter_ = {};
        loading_ = false;
    }
    condition_.notify_all();
}

bool DomDownToolPreview::GisOverlayRenderer::IsLoading() const
{
    const std::lock_guard lock(mutex_);
    return loading_;
}

DomDownToolPreview::GisOverlayCategory DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(
    const DownTool::AmapVectorFeature& feature)
{
    if (isWaterFeatureCode(feature.mainKey, feature.subKey))
    {
        return GisOverlayCategory::Water;
    }
    if (isGreenFeatureCode(feature.mainKey, feature.subKey))
    {
        return GisOverlayCategory::Green;
    }
    if (isHighwayFeature(feature))
    {
        return GisOverlayCategory::Highway;
    }
    if (feature.layerName == "road")
    {
        return GisOverlayCategory::Road;
    }
    if (feature.layerName == "building")
    {
        return GisOverlayCategory::Building;
    }
    if (isBoundaryFeature(feature))
    {
        return GisOverlayCategory::Boundary;
    }
    if (feature.layerName.starts_with("transit-"))
    {
        return GisOverlayCategory::Subway;
    }
    if (feature.layerName == "poi" ||
        feature.geometryType == DownTool::AmapVectorGeometryType::Point)
    {
        return GisOverlayCategory::Point;
    }
    return GisOverlayCategory::Other;
}

DownTool::AmapVectorColor DomDownToolPreview::GisOverlayRenderer::defaultColor(
    GisOverlayCategory category)
{
    switch (category)
    {
    case GisOverlayCategory::Road:
        return {240, 198, 65, 255};
    case GisOverlayCategory::Water:
        return {66, 165, 245, 190};
    case GisOverlayCategory::Green:
        return {67, 160, 71, 190};
    case GisOverlayCategory::Highway:
        return {255, 111, 56, 255};
    case GisOverlayCategory::Subway:
        return {210, 75, 220, 255};
    case GisOverlayCategory::Building:
        return {180, 180, 180, 170};
    case GisOverlayCategory::Point:
        return {229, 57, 53, 255};
    case GisOverlayCategory::Boundary:
        return {83, 110, 122, 255};
    case GisOverlayCategory::Other:
        return {96, 125, 139, 255};
    case GisOverlayCategory::None:
    case GisOverlayCategory::All:
        return {0, 0, 0, 0};
    }
    return {0, 0, 0, 0};
}

std::shared_ptr<const DomDownToolPreview::GisOverlayRenderer::PreparedData>
DomDownToolPreview::GisOverlayRenderer::prepareData(
    const DownTool::AmapVectorDecodedData& decodedData)
{
    auto preparedData = std::make_shared<PreparedData>();
    for (const DownTool::AmapVectorFeature& sourceFeature : decodedData.features)
    {
        const GisOverlayCategory category = ClassifyFeature(sourceFeature);
        if (category == GisOverlayCategory::None || category == GisOverlayCategory::All)
        {
            continue;
        }

        PreparedData::Feature feature;
        feature.geometryType = sourceFeature.geometryType;
        feature.minZoom = sourceFeature.minZoom;
        feature.maxZoom = sourceFeature.maxZoom;
        feature.geometryParts.reserve(sourceFeature.geometryParts.size());
        for (const std::vector<DownTool::AmapVectorCoordinate>& sourcePart :
             sourceFeature.geometryParts)
        {
            std::vector<DownTool::LonLat> part;
            part.reserve(sourcePart.size());
            for (const DownTool::AmapVectorCoordinate& sourceCoordinate : sourcePart)
            {
                const DownTool::LonLat coordinate =
                    DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(
                        {sourceCoordinate.longitude, sourceCoordinate.latitude});
                part.push_back(coordinate);
                if (!feature.bounds.initialized)
                {
                    feature.bounds.minLon = coordinate.lon;
                    feature.bounds.maxLon = coordinate.lon;
                    feature.bounds.minLat = coordinate.lat;
                    feature.bounds.maxLat = coordinate.lat;
                    feature.bounds.initialized = true;
                }
                else
                {
                    feature.bounds.minLon = std::min(feature.bounds.minLon, coordinate.lon);
                    feature.bounds.maxLon = std::max(feature.bounds.maxLon, coordinate.lon);
                    feature.bounds.minLat = std::min(feature.bounds.minLat, coordinate.lat);
                    feature.bounds.maxLat = std::max(feature.bounds.maxLat, coordinate.lat);
                }
            }
            if (!part.empty())
            {
                feature.geometryParts.push_back(std::move(part));
            }
        }
        if (!feature.geometryParts.empty())
        {
            preparedData->featuresByCategory[categoryIndex(category)].push_back(
                std::move(feature));
        }
    }
    return preparedData;
}

void DomDownToolPreview::GisOverlayRenderer::workerLoop()
{
    while (true)
    {
        LoadRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || pendingRequest_.has_value(); });
            if (stopping_)
            {
                return;
            }
            request = *pendingRequest_;
            pendingRequest_.reset();
            activeRequest_ = request;
        }

        DownTool::AmapVectorDownloadOptions options;
        options.centerWgs84 = request.centerWgs84;
        options.radiusMeters = request.radiusMeters;
        options.zoom = request.zoom;
        options.includeBaseLayers = true;
        options.includeLabels = true;
        options.maxConcurrency = 0;
        DownTool::AmapVectorDownloadResult result;
        try
        {
            result = DownTool::AmapVectorDownloader().Download(options);
        }
        catch (...)
        {
            result.success = false;
            result.error = "GIS worker threw an unknown exception";
        }

        writeSearchTrace("GIS", "load_complete",
                         fmt::format("success={} features={} packets={} cached_packets={} "
                                     "downloaded_tasks={} cached_tasks={} error={}",
                                     result.success, result.featureCount,
                                     result.downloadedPacketCount, result.cachedPacketCount,
                                     result.downloadedTaskCount, result.cachedTaskCount,
                                     result.error));

        std::shared_ptr<const PreparedData> preparedData;
        if (result.success)
        {
            const auto prepareStart = std::chrono::steady_clock::now();
            preparedData = prepareData(result.decodedData);
            const auto prepareMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - prepareStart);
            writeSearchTrace(
                "GIS", "prepare_complete",
                fmt::format(
                    "elapsed_ms={} road={} water={} green={} highway={} subway={} "
                    "building={} point={} boundary={} other={}",
                    prepareMilliseconds.count(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Road)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Water)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Green)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Highway)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Subway)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Building)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Point)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Boundary)]
                        .size(),
                    preparedData->featuresByCategory[categoryIndex(GisOverlayCategory::Other)]
                        .size()));
        }

        bool accepted = false;
        {
            const std::lock_guard lock(mutex_);
            if (activeRequest_.has_value() &&
                activeRequest_->generation == request.generation)
            {
                activeRequest_.reset();
            }
            if (!stopping_ && request.generation == generation_ &&
                category_ != GisOverlayCategory::None)
            {
                if (result.success)
                {
                    preparedData_ = std::move(preparedData);
                    loadedContext_ = request;
                    retryAfter_ = {};
                }
                else
                {
                    retryAfter_ = std::chrono::steady_clock::now() + kRetryDelay;
                }
                loading_ = pendingRequest_.has_value();
                accepted = true;
            }
        }
        if (accepted && requestRedraw_)
        {
            requestRedraw_();
        }
    }
}
