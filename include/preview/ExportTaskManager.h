#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "AmapVectorPngDownloader.h"
#include "DataType.h"

namespace DownTool
{
class CopernicusDemData;
class DownToolBase;
}

namespace DomDownToolPreview
{
enum class ExportTaskState
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Canceled
};

enum class GisExportMode
{
    OverlayOnSatellite,
    MergeSelectedLayers,
    SeparateLayers
};

struct SatelliteExportSource
{
    std::string id;
    std::string name;
    std::filesystem::path cachePath;
};

using ExportDownloaderFactory =
    std::function<std::unique_ptr<DownTool::DownToolBase>(std::string_view)>;

struct ExportConfiguration
{
    DownTool::LonLat centerWgs84;
    double radiusMeters = 0.;
    uint32_t zoom = 15;
    std::vector<SatelliteExportSource> satelliteSources;
    std::set<DownTool::AmapVectorRenderCategory> gisCategories;
    std::set<GisExportMode> gisModes;
    std::map<DownTool::AmapVectorRenderCategory, DownTool::AmapVectorColor> gisColors;
    std::filesystem::path outputRoot;
    bool exportTiles = false;
    bool exportMergedImage = false;
    bool exportFbx = false;
    bool exportTerrainHeight = false;
    bool exportGeoJson = false;
    bool colorBuildingsByHeight = false;
    bool exportBuildingFbx = false;
    std::filesystem::path buildingWallTexturePath = std::filesystem::u8path("贴图/墙体");
    std::filesystem::path buildingRoofTexturePath = std::filesystem::u8path("贴图/房顶");
    double buildingLayerHeightCentimeters = 45.;
    uint32_t buildingMaxWallMaterialsPerBucket = 1;
    uint32_t buildingTextureRandomSeed = 0;
    uint32_t mergedImageResolution = 16384;
    std::string mergedImageFormat = "png";
    uint32_t fbxTextureSize = 4096;
    double fbxTerrainSampleRateMeters = 30.;
};

struct ExportTaskRecord
{
    std::string id;
    std::string title;
    std::string detail;
    std::string outputPath;
    std::string createdAt;
    std::string finishedAt;
    ExportTaskState state = ExportTaskState::Queued;
    std::string stage = "等待执行";
    double progress = 0.;
    uint64_t completed = 0;
    uint64_t total = 0;
    std::string error;
    std::optional<ExportConfiguration> configuration;
};

[[nodiscard]] std::string ExportTaskStateName(ExportTaskState state);

[[nodiscard]] std::filesystem::path ExportTaskOutputRoot(
    const ExportConfiguration& configuration, std::string_view taskId);

[[nodiscard]] std::filesystem::path ExportGisOverlayInputRoot(
    bool exportTiles, const std::filesystem::path& tileOutputPath,
    const std::filesystem::path& temporaryRoot);

class ExportTaskManager
{
  public:
    explicit ExportTaskManager(
        std::function<void()> changeCallback,
        std::filesystem::path recordsPath =
            std::filesystem::u8path("cache/导出任务.json"),
        ExportDownloaderFactory downloaderFactory = {});

    ~ExportTaskManager();

    ExportTaskManager(const ExportTaskManager&) = delete;
    ExportTaskManager& operator=(const ExportTaskManager&) = delete;

    [[nodiscard]] std::string Enqueue(const ExportConfiguration& configuration);

    bool Cancel(std::string_view taskId);

    bool RemoveFinished(std::string_view taskId);

    bool DeleteFinishedOutput(std::string_view taskId, std::string& error);

    void ClearFinished();

    [[nodiscard]] std::vector<ExportTaskRecord> GetRecords() const;

  private:
    struct QueuedExport
    {
        std::string taskId;
        ExportConfiguration configuration;
    };

    void WorkerLoop();

    bool RunExport(const QueuedExport& queuedExport, std::string& error);

    bool RunSatelliteExport(const QueuedExport& queuedExport,
                            const SatelliteExportSource& source,
                            const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
                            size_t stepIndex, size_t stepCount,
                            std::string& error);

    bool RunGisExport(const QueuedExport& queuedExport,
                      const std::set<DownTool::AmapVectorRenderCategory>& categories,
                      GisExportMode mode, std::string_view name,
                      const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
                      size_t stepIndex, size_t stepCount,
                      std::string& error);

    bool RunBuildingFbxExport(const QueuedExport& queuedExport,
                              size_t stepIndex, size_t stepCount,
                              std::string& error);

    bool RunGeoJsonExport(const QueuedExport& queuedExport,
                          size_t stepIndex, size_t stepCount,
                          std::string& error);

    void UpdateProgress(std::string_view taskId, std::string stage,
                        double progress, uint64_t completed, uint64_t total);

    void FinishTask(std::string_view taskId, ExportTaskState state,
                    std::string error = {});

    void SetActiveDownloader(DownTool::DownToolBase* downloader);

    void ClearActiveDownloader();

    void LoadRecords();

    void SaveRecords(bool force);

    void NotifyChanged() const;

    [[nodiscard]] bool IsCancellationRequested() const;

    std::function<void()> changeCallback_;
    ExportDownloaderFactory downloaderFactory_;
    std::filesystem::path recordsPath_;
    mutable std::mutex mutex_;
    std::mutex recordsFileMutex_;
    std::condition_variable condition_;
    std::vector<ExportTaskRecord> records_;
    std::deque<QueuedExport> queue_;
    std::thread worker_;
    // 仅在工作线程执行导出期间有效；读取和清空由 mutex_ 保护。
    DownTool::DownToolBase* activeDownloader_ = nullptr;
    std::atomic_bool cancelActive_{false};
    bool stopping_ = false;
    std::chrono::steady_clock::time_point lastSaveTime_{};
    std::chrono::steady_clock::time_point lastProgressNotificationTime_{};
};
} // namespace DomDownToolPreview
