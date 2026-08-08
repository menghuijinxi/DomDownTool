#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "AmapVectorDecoder.h"
#include "AmapVectorPngDownloader.h"
#include "DownToolBase.h"
#include "preview/MapViewport.h"

class SkCanvas;

namespace DomDownToolPreview
{
class GisOverlayRendererTestAccess;

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

class GisOverlayRenderer
{
  public:
    explicit GisOverlayRenderer(std::function<void()> requestRedraw);

    ~GisOverlayRenderer();

    GisOverlayRenderer(const GisOverlayRenderer&) = delete;
    GisOverlayRenderer& operator=(const GisOverlayRenderer&) = delete;

    void SetCategory(GisOverlayCategory category);

    [[nodiscard]] GisOverlayCategory GetCategory() const;

    [[nodiscard]] static GisOverlayCategory ClassifyFeature(
        const DownTool::AmapVectorFeature& feature);

    void SetColor(GisOverlayCategory category, const DownTool::AmapVectorColor& color);

    [[nodiscard]] DownTool::AmapVectorColor GetColor(GisOverlayCategory category) const;

    void UpdateContext(const DownTool::LonLat& centerWgs84, double radiusMeters, uint32_t zoom);

    void Draw(SkCanvas& canvas, const MapViewport& viewport,
              const DownTool::DownToolBase& projection, TileYAxisDirection yDirection,
              uint32_t tileSize);

    void Clear();

    [[nodiscard]] bool IsLoading() const;

  private:
    friend class GisOverlayRendererTestAccess;

    struct PreparedData;
    struct PreparedRenderCache;

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
        const DownTool::AmapVectorDecodedData& decodedData);

    void workerLoop();

    std::function<void()> requestRedraw_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<LoadRequest> pendingRequest_;
    std::optional<LoadRequest> activeRequest_;
    std::optional<LoadRequest> loadedContext_;
    std::shared_ptr<const PreparedData> preparedData_;
    std::map<GisOverlayCategory, DownTool::AmapVectorColor> colors_;
    std::unique_ptr<PreparedRenderCache> renderCache_;
    GisOverlayCategory category_ = GisOverlayCategory::None;
    uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point retryAfter_{};
    bool loading_ = false;
    bool stopping_ = false;
    std::thread worker_;
};
} // namespace DomDownToolPreview
