#include "preview/GisOverlayRenderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iterator>
#include <numbers>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "AmapVectorTileGrid.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"

#include "preview/Dx12GisRenderer.h"
#include "preview/SearchTrace.h"

namespace
{
using DomDownToolPreview::GisOverlayCategory;
using DownTool::AmapVectorColor;
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

DomDownToolPreview::GisOverlayCategories allGisCategories()
{
    return {kAllDrawOrder.begin(), kAllDrawOrder.end()};
}

DomDownToolPreview::GisOverlayCategories normalizeCategories(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    if (categories.contains(GisOverlayCategory::All))
    {
        return allGisCategories();
    }

    DomDownToolPreview::GisOverlayCategories normalized;
    for (const GisOverlayCategory category : categories)
    {
        if (isKnownCategory(category) && category != GisOverlayCategory::None &&
            category != GisOverlayCategory::All)
        {
            normalized.insert(category);
        }
    }
    return normalized;
}

std::string categoriesName(const DomDownToolPreview::GisOverlayCategories& categories)
{
    if (categories.empty())
    {
        return categoryName(GisOverlayCategory::None);
    }
    if (categories == allGisCategories())
    {
        return categoryName(GisOverlayCategory::All);
    }

    std::string names;
    for (const GisOverlayCategory category : kAllDrawOrder)
    {
        if (!categories.contains(category))
        {
            continue;
        }
        if (!names.empty())
        {
            names += ',';
        }
        names += categoryName(category);
    }
    return names;
}
} // namespace

struct DomDownToolPreview::GisOverlayRenderer::PreparedData
{
    using Feature = GisMeshFeature;

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
    std::array<std::shared_ptr<const GisGeometryMesh>, kCategoryCount> meshes;
    std::array<uint8_t, kCategoryCount> meshInitialized{};
};

struct DomDownToolPreview::GisOverlayRenderer::MeshBuildRequest
{
    std::shared_ptr<const PreparedData> data;
    ViewportTransform transform;
};

struct DomDownToolPreview::GisOverlayRenderer::MeshBuildResult
{
    std::shared_ptr<const PreparedData> data;
    const DownTool::DownToolBase* projection = nullptr;
    TileYAxisDirection yDirection = TileYAxisDirection::Down;
    uint32_t tileSize = 0;
    uint32_t sourceZoom = 0;
    std::array<std::shared_ptr<const GisGeometryMesh>, kCategoryCount> meshes;
};

DomDownToolPreview::GisOverlayRenderer::GisOverlayRenderer(
    std::function<void()> requestRedraw)
    : requestRedraw_(std::move(requestRedraw)),
      dx12Renderer_(std::make_unique<Dx12GisRenderer>(requestRedraw_))
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
                     fmt::format("categories={}", categoriesName(categories_)));
    worker_ = std::thread([this] { workerLoop(); });
    meshWorker_ = std::thread([this] { meshWorkerLoop(); });
}

DomDownToolPreview::GisOverlayRenderer::~GisOverlayRenderer()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        pendingRequest_.reset();
    }
    condition_.notify_all();
    meshCondition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    if (meshWorker_.joinable())
    {
        meshWorker_.join();
    }
}

void DomDownToolPreview::GisOverlayRenderer::SetCategory(GisOverlayCategory category)
{
    if (category == GisOverlayCategory::None)
    {
        SetCategories({});
        return;
    }
    if (category == GisOverlayCategory::All)
    {
        SetCategories(allGisCategories());
        return;
    }
    SetCategories({category});
}

DomDownToolPreview::GisOverlayCategory
DomDownToolPreview::GisOverlayRenderer::GetCategory() const
{
    const GisOverlayCategories categories = GetCategories();
    if (categories.empty())
    {
        return GisOverlayCategory::None;
    }
    if (categories.size() == 1)
    {
        return *categories.begin();
    }
    return GisOverlayCategory::All;
}

void DomDownToolPreview::GisOverlayRenderer::SetCategories(
    const GisOverlayCategories& categories)
{
    const GisOverlayCategories normalizedCategories = normalizeCategories(categories);
    GisOverlayCategories previousCategories;
    uint64_t generation = 0;
    {
        const std::lock_guard lock(mutex_);
        previousCategories = categories_;
        if (categories_ == normalizedCategories)
        {
            return;
        }
        categories_ = normalizedCategories;
        if (categories_.empty())
        {
            pendingRequest_.reset();
            pendingMeshRequest_.reset();
            readyMeshResult_.reset();
            ++generation_;
            retryAfter_ = {};
            loading_ = false;
        }
        generation = generation_;
    }
    writeSearchTrace(
        "GIS", "categories_changed",
        fmt::format("from={} to={} generation={}", categoriesName(previousCategories),
                    categoriesName(normalizedCategories), generation));
    condition_.notify_all();
    if (requestRedraw_)
    {
        requestRedraw_();
    }
}

DomDownToolPreview::GisOverlayCategories
DomDownToolPreview::GisOverlayRenderer::GetCategories() const
{
    const std::lock_guard lock(mutex_);
    return categories_;
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

void DomDownToolPreview::GisOverlayRenderer::ResetColors()
{
    {
        const std::lock_guard lock(mutex_);
        for (auto& [category, color] : colors_)
        {
            color = defaultColor(category);
        }
    }
    if (requestRedraw_)
    {
        requestRedraw_();
    }
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
        if (categories_.empty() || stopping_ || localMode_)
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
            fmt::format("categories={} lon={:.6f} lat={:.6f} radius={:.1f} zoom={} generation={}",
                        categoriesName(categories_), centerWgs84.lon, centerWgs84.lat,
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
    GisOverlayCategories selectedCategories;
    std::map<GisOverlayCategory, DownTool::AmapVectorColor> colors;
    {
        const std::lock_guard lock(mutex_);
        selectedCategories = categories_;
        if (selectedCategories.empty() || !preparedData_)
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
    std::vector<GisOverlayCategory> categories;
    for (const GisOverlayCategory category : kAllDrawOrder)
    {
        if (selectedCategories.contains(category))
        {
            categories.push_back(category);
        }
    }

    const auto meshRequestMatches = [&](const MeshBuildRequest* request)
    {
        return request != nullptr && request->data.get() == preparedData.get() &&
            request->transform.projection == transform->projection &&
            request->transform.yDirection == transform->yDirection &&
            request->transform.tileSize == transform->tileSize &&
            request->transform.sourceZoom == transform->sourceZoom;
    };
    const auto meshResultMatches = [&](const MeshBuildResult* result)
    {
        return result != nullptr && result->data.get() == preparedData.get() &&
            result->projection == transform->projection &&
            result->yDirection == transform->yDirection &&
            result->tileSize == transform->tileSize &&
            result->sourceZoom == transform->sourceZoom;
    };

    Dx12GisRenderer& dx12Renderer = *dx12Renderer_;
    const bool dx12Supported = dx12Renderer.SupportsCanvas(canvas);
    bool notifyMeshWorker = false;
    if (dx12Supported)
    {
        const std::lock_guard lock(mutex_);
        if (readyMeshResult_)
        {
            if (meshResultMatches(readyMeshResult_.get()))
            {
                renderCache_->meshes = std::move(readyMeshResult_->meshes);
                renderCache_->meshInitialized.fill(uint8_t{1});
            }
            readyMeshResult_.reset();
        }
        const bool meshesReady = std::all_of(
            renderCache_->meshInitialized.begin(), renderCache_->meshInitialized.end(),
            [](uint8_t initialized) { return initialized != 0; });
        if (!meshesReady && !meshRequestMatches(pendingMeshRequest_.get()) &&
            !meshRequestMatches(activeMeshRequest_.get()))
        {
            pendingMeshRequest_ = std::make_unique<MeshBuildRequest>(
                MeshBuildRequest{preparedData, *transform});
            notifyMeshWorker = true;
        }
    }
    if (notifyMeshWorker)
    {
        meshCondition_.notify_one();
    }

    const bool meshesReady = dx12Supported && std::all_of(
        renderCache_->meshInitialized.begin(), renderCache_->meshInitialized.end(),
        [](uint8_t initialized) { return initialized != 0; });
    if (dx12Supported)
    {
        Dx12GisRenderFrame dx12Frame;
        dx12Frame.transform = *transform;
        dx12Frame.meshSetReady = meshesReady;
        const SkISize surfaceSize = canvas.getBaseLayerSize();
        dx12Frame.width = surfaceSize.width();
        dx12Frame.height = surfaceSize.height();
        SkIRect clipBounds;
        if (canvas.getDeviceClipBounds(&clipBounds))
        {
            dx12Frame.clipTop = static_cast<float>(clipBounds.top());
            dx12Frame.clipBottom = static_cast<float>(clipBounds.bottom());
        }
        else
        {
            dx12Frame.clipTop = 0.F;
            dx12Frame.clipBottom = static_cast<float>(dx12Frame.height);
        }
        if (meshesReady)
        {
            for (const std::shared_ptr<const GisGeometryMesh>& mesh : renderCache_->meshes)
            {
                if (mesh)
                {
                    dx12Frame.residentMeshes.push_back(mesh);
                }
            }
            for (const GisOverlayCategory category : categories)
            {
                const size_t index = categoryIndex(category);
                const std::shared_ptr<const GisGeometryMesh>& mesh =
                    renderCache_->meshes[index];
                const bool hasGeometry = mesh && (!mesh->fillIndices.empty() ||
                    !mesh->lineIndices.empty() || !mesh->pointIndices.empty());
                if (!hasGeometry)
                {
                    continue;
                }
                const auto colorIterator = colors.find(category);
                const AmapVectorColor color = colorIterator == colors.end()
                    ? defaultColor(category)
                    : colorIterator->second;
                dx12Frame.batches.push_back(
                    {mesh,
                     static_cast<float>(color.red) / 255.F,
                     static_cast<float>(color.green) / 255.F,
                     static_cast<float>(color.blue) / 255.F,
                     static_cast<float>(color.alpha) / 255.F,
                     lineWidthForCategory(category),
                     5.F});
            }
        }
        if (dx12Renderer.Draw(canvas, dx12Frame))
        {
            return;
        }
    }

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
    for (const GisOverlayCategory category : categories)
    {
        drawCategory(category);
    }
    canvas.restore();
}

void DomDownToolPreview::GisOverlayRenderer::Clear()
{
    {
        const std::lock_guard lock(mutex_);
        preparedData_.reset();
        renderCache_.reset();
        dx12Renderer_->Clear();
        pendingRequest_.reset();
        pendingMeshRequest_.reset();
        readyMeshResult_.reset();
        loadedContext_.reset();
        ++generation_;
        retryAfter_ = {};
        loading_ = false;
    }
    condition_.notify_all();
    meshCondition_.notify_all();
}

bool DomDownToolPreview::GisOverlayRenderer::IsLoading() const
{
    const std::lock_guard lock(mutex_);
    return loading_;
}

void DomDownToolPreview::GisOverlayRenderer::SetLocalShapefiles(
    const std::vector<DownTool::ShapefileImportOptions>& sources)
{
    // 与远程 GIS 复用工作线程；代次变化会取消旧读取，防止删除后重新出现。
    {
        const std::lock_guard lock(mutex_);
        localMode_ = true;
        localShapefiles_ = sources;
        categories_ = sources.empty() ? GisOverlayCategories{} : allGisCategories();
        preparedData_.reset();
        renderCache_.reset();
        dx12Renderer_->Clear();
        pendingMeshRequest_.reset();
        readyMeshResult_.reset();
        loadedContext_.reset();
        pendingRequest_.reset();
        loadError_.clear();
        ++generation_;
        loading_ = !sources.empty();
        if (loading_)
        {
            LoadRequest request;
            request.generation = generation_;
            pendingRequest_ = request;
        }
    }
    condition_.notify_one();
    if (requestRedraw_)
    {
        requestRedraw_();
    }
}

std::string DomDownToolPreview::GisOverlayRenderer::GetLoadError() const
{
    const std::lock_guard lock(mutex_);
    return loadError_;
}

DomDownToolPreview::GisOverlayCategory DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(
    const DownTool::AmapVectorFeature& feature)
{
    switch (DownTool::ClassifyAmapVectorFeature(feature))
    {
    case DownTool::AmapVectorRenderCategory::All:
        return GisOverlayCategory::All;
    case DownTool::AmapVectorRenderCategory::Point:
        return GisOverlayCategory::Point;
    case DownTool::AmapVectorRenderCategory::Road:
        return GisOverlayCategory::Road;
    case DownTool::AmapVectorRenderCategory::Green:
        return GisOverlayCategory::Green;
    case DownTool::AmapVectorRenderCategory::Highway:
        return GisOverlayCategory::Highway;
    case DownTool::AmapVectorRenderCategory::Subway:
        return GisOverlayCategory::Subway;
    case DownTool::AmapVectorRenderCategory::Building:
        return GisOverlayCategory::Building;
    case DownTool::AmapVectorRenderCategory::Water:
        return GisOverlayCategory::Water;
    case DownTool::AmapVectorRenderCategory::Boundary:
        return GisOverlayCategory::Boundary;
    case DownTool::AmapVectorRenderCategory::Other:
        return GisOverlayCategory::Other;
    }
    return GisOverlayCategory::Other;
}

DownTool::AmapVectorColor DomDownToolPreview::GisOverlayRenderer::defaultColor(
    GisOverlayCategory category)
{
    switch (category)
    {
    case GisOverlayCategory::Road:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Road);
    case GisOverlayCategory::Water:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Water);
    case GisOverlayCategory::Green:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Green);
    case GisOverlayCategory::Highway:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Highway);
    case GisOverlayCategory::Subway:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Subway);
    case GisOverlayCategory::Building:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Building);
    case GisOverlayCategory::Point:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Point);
    case GisOverlayCategory::Boundary:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Boundary);
    case GisOverlayCategory::Other:
        return DownTool::GetDefaultAmapVectorRenderColor(
            DownTool::AmapVectorRenderCategory::Other);
    case GisOverlayCategory::None:
    case GisOverlayCategory::All:
        return {0, 0, 0, 0};
    }
    return {0, 0, 0, 0};
}

std::shared_ptr<const DomDownToolPreview::GisOverlayRenderer::PreparedData> DomDownToolPreview::
    GisOverlayRenderer::prepareData(const DownTool::AmapVectorDecodedData& decodedData,
                                    const std::function<bool()>& cancellationRequested)
{
    auto preparedData = std::make_shared<PreparedData>();
    size_t coordinateCount = 0;
    for (const DownTool::AmapVectorFeature& sourceFeature : decodedData.features)
    {
        if (cancellationRequested && cancellationRequested())
        {
            return {};
        }
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
                if (++coordinateCount % 256 == 0 && cancellationRequested &&
                    cancellationRequested())
                {
                    return {};
                }
                const DownTool::LonLat coordinate =
                    DownTool::VectorCoordinateWgs84(sourceFeature, sourceCoordinate);
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
        bool localMode = false;
        std::vector<DownTool::ShapefileImportOptions> localSources;
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
            localMode = localMode_;
            localSources = localShapefiles_;
        }

        const auto cancelled = [this, generation = request.generation] {
            const std::lock_guard lock(mutex_);
            return stopping_ || generation != generation_;
        };
        DownTool::AmapVectorDownloadOptions options;
        options.centerWgs84 = request.centerWgs84;
        options.radiusMeters = request.radiusMeters;
        options.zoom = request.zoom;
        options.includeBaseLayers = true;
        options.includeLabels = true;
        options.maxConcurrency = 0;
        options.cancellationRequested = cancelled;
        DownTool::AmapVectorDownloadResult result;
        std::shared_ptr<const PreparedData> preparedData;
        try
        {
            if (localMode)
            {
                result.success = true;
                for (const auto& source : localSources)
                {
                    DownTool::AmapVectorDecodedData decoded;
                    if (!DownTool::Wgs84ShapefileReader::Read(source, decoded, result.error,
                                                              cancelled))
                    {
                        result.success = false;
                        break;
                    }
                    result.decodedData.features.insert(
                        result.decodedData.features.end(),
                        std::make_move_iterator(decoded.features.begin()),
                        std::make_move_iterator(decoded.features.end()));
                }
            }
            else
            {
                result = DownTool::AmapVectorDownloader().Download(options);
            }
            if (result.success)
            {
                preparedData = prepareData(result.decodedData, cancelled);
            }
        }
        catch (const std::exception& exception)
        {
            result.success = false;
            result.error = std::string("GIS 加载失败：") + exception.what();
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

        if (preparedData)
        {
            writeSearchTrace(
                "GIS", "prepare_complete",
                fmt::format(
                    "road={} water={} green={} highway={} subway={} "
                    "building={} point={} boundary={} other={}",
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
            if (activeRequest_.has_value() && activeRequest_->generation == request.generation)
            {
                activeRequest_.reset();
            }
            if (!stopping_ && request.generation == generation_ && !categories_.empty())
            {
                loadError_ = result.error;
                if (result.success && preparedData)
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

void DomDownToolPreview::GisOverlayRenderer::meshWorkerLoop()
{
    while (true)
    {
        MeshBuildRequest request;
        {
            std::unique_lock lock(mutex_);
            meshCondition_.wait(lock,
                                [this] { return stopping_ || pendingMeshRequest_ != nullptr; });
            if (stopping_)
            {
                return;
            }
            activeMeshRequest_ = std::move(pendingMeshRequest_);
            request = *activeMeshRequest_;
        }

        const auto buildStart = std::chrono::steady_clock::now();
        auto result = std::make_unique<MeshBuildResult>();
        result->data = request.data;
        result->projection = request.transform.projection;
        result->yDirection = request.transform.yDirection;
        result->tileSize = request.transform.tileSize;
        result->sourceZoom = request.transform.sourceZoom;
        size_t vertexCount = 0;
        size_t indexCount = 0;
        for (size_t index = 0; index < kCategoryCount; ++index)
        {
            result->meshes[index] = std::make_shared<GisGeometryMesh>(GisMeshBuilder::Build(
                request.data->featuresByCategory[index], request.transform,
                request.transform.sourceZoom));
            const GisGeometryMesh& mesh = *result->meshes[index];
            vertexCount += mesh.fillVertices.size() + mesh.lineVertices.size() +
                mesh.pointVertices.size();
            indexCount += mesh.fillIndices.size() + mesh.lineIndices.size() +
                mesh.pointIndices.size();
        }
        const auto buildMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - buildStart);

        bool accepted = false;
        {
            const std::lock_guard lock(mutex_);
            activeMeshRequest_.reset();
            if (!stopping_ && preparedData_.get() == request.data.get())
            {
                readyMeshResult_ = std::move(result);
                accepted = true;
            }
        }
        writeSearchTrace(
            "GIS", "mesh_build_complete",
            fmt::format("accepted={} elapsed_ms={} vertices={} indices={} source_zoom={}",
                        accepted, buildMilliseconds.count(), vertexCount, indexCount,
                        request.transform.sourceZoom));
        if (accepted && requestRedraw_)
        {
            requestRedraw_();
        }
    }
}
