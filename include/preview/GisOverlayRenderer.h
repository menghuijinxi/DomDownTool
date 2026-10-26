#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "AmapVectorDecoder.h"
#include "AmapVectorPngDownloader.h"
#include "DownToolBase.h"
#include "Wgs84ShapefileReader.h"
#include "preview/GisMeshBuilder.h"
#include "preview/MapViewport.h"

class SkCanvas;

namespace DomDownToolPreview
{
class GisOverlayRendererTestAccess;
class Dx12GisRenderer;

enum class GisOverlayCategory
{
    None,
    All,
    Road,
    Water,
    Green,
    Highway,
    Subway,
    Building,
    Point,
    Boundary,
    Other
};

using GisOverlayCategories = std::set<GisOverlayCategory>;

class GisOverlayRenderer
{
  public:
    explicit GisOverlayRenderer(std::function<void()> requestRedraw);

    ~GisOverlayRenderer();

    GisOverlayRenderer(const GisOverlayRenderer&) = delete;
    GisOverlayRenderer& operator=(const GisOverlayRenderer&) = delete;

    void SetCategory(GisOverlayCategory category);

    [[nodiscard]] GisOverlayCategory GetCategory() const;

    void SetCategories(const GisOverlayCategories& categories);

    [[nodiscard]] GisOverlayCategories GetCategories() const;
    void SetUseTencent(bool value);
    [[nodiscard]] bool GetUseTencent() const;
    void SetDataSource(DownTool::VectorDataSource source);
    [[nodiscard]] DownTool::VectorDataSource GetDataSource() const;

    [[nodiscard]] static GisOverlayCategory ClassifyFeature(
        const DownTool::AmapVectorFeature& feature);

    void SetColor(GisOverlayCategory category, const DownTool::AmapVectorColor& color);

    [[nodiscard]] DownTool::AmapVectorColor GetColor(GisOverlayCategory category) const;

    void ResetColors();

    void UpdateContext(const DownTool::LonLat& centerWgs84, double radiusMeters, uint32_t zoom);

    void Draw(SkCanvas& canvas, const MapViewport& viewport,
              const DownTool::DownToolBase& projection, TileYAxisDirection yDirection,
              uint32_t tileSize);

    void Clear();

    [[nodiscard]] bool IsLoading() const;

    void SetLocalShapefiles(const std::vector<DownTool::ShapefileImportOptions>& sources);

    [[nodiscard]] std::string GetLoadError() const;

  private:
    friend class GisOverlayRendererTestAccess;

    struct PreparedData;
    struct PreparedRenderCache;
    struct MeshBuildRequest;
    struct MeshBuildResult;

    struct LoadRequest
    {
        DownTool::LonLat centerWgs84;
        double radiusMeters = 0.;
        uint32_t zoom = 0;
        uint64_t generation = 0;
    };

    [[nodiscard]] static DownTool::AmapVectorColor defaultColor(
        GisOverlayCategory category);

    [[nodiscard]] static std::shared_ptr<const PreparedData> prepareData(
        const DownTool::AmapVectorDecodedData& decodedData,
        const std::function<bool()>& cancellationRequested = {});

    void workerLoop();

    void meshWorkerLoop();

    std::function<void()> requestRedraw_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable meshCondition_;
    std::optional<LoadRequest> pendingRequest_;
    std::optional<LoadRequest> activeRequest_;
    std::optional<LoadRequest> loadedContext_;
    std::unique_ptr<MeshBuildRequest> pendingMeshRequest_;
    std::unique_ptr<MeshBuildRequest> activeMeshRequest_;
    std::unique_ptr<MeshBuildResult> readyMeshResult_;
    std::shared_ptr<const PreparedData> preparedData_;
    std::map<GisOverlayCategory, DownTool::AmapVectorColor> colors_;
    std::unique_ptr<PreparedRenderCache> renderCache_;
    GisOverlayCategories categories_;
    uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point retryAfter_{};
    bool loading_ = false;
    bool stopping_ = false;
    bool localMode_ = false;

  protected:
    DownTool::VectorDataSource dataSource_ = DownTool::VectorDataSource::Amap;

  private:
    std::vector<DownTool::ShapefileImportOptions> localShapefiles_;
    std::string loadError_;
    std::thread worker_;
    std::thread meshWorker_;
    std::unique_ptr<Dx12GisRenderer> dx12Renderer_;
};
} // namespace DomDownToolPreview
