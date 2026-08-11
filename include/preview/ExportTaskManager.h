#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "AmapVectorPngDownloader.h"
#include "DataType.h"

namespace DownTool
{
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

enum class ExportFbxSource
{
    Satellite,
    Gis
};

struct GisExportGroup
{
    std::string name;
    std::set<DownTool::AmapVectorRenderCategory> categories;
};

struct ExportConfiguration
{
    DownTool::LonLat centerWgs84;
    double radiusMeters = 0.;
    uint32_t zoom = 15;
    std::string mapSourceId;
    std::string mapSourceName;
    std::filesystem::path satelliteCachePath;
    std::filesystem::path satelliteOutputPath;
    std::filesystem::path gisOutputPath =
        std::filesystem::u8path("cache/高德地图/gis数据/输出");
    std::vector<GisExportGroup> gisGroups;
    std::map<DownTool::AmapVectorRenderCategory, DownTool::AmapVectorColor> gisColors;
    bool exportSatelliteTiles = false;
    bool exportSatelliteMergedImage = false;
    bool exportGisTiles = false;
    bool exportGisMergedImage = false;
    bool exportFbx = false;
    ExportFbxSource fbxSource = ExportFbxSource::Satellite;
    bool colorBuildingsByHeight = false;
    uint32_t mergedImageResolution = 16384;
    std::string mergedImageFormat = "png";
    uint32_t fbxTextureSize = 4096;
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
};

[[nodiscard]] std::string ExportTaskStateName(ExportTaskState state);

class ExportTaskManager
{
  public:
    explicit ExportTaskManager(
        std::function<void()> changeCallback,
        std::filesystem::path recordsPath =
            std::filesystem::u8path("cache/导出任务.json"));

    ~ExportTaskManager();

    ExportTaskManager(const ExportTaskManager&) = delete;
    ExportTaskManager& operator=(const ExportTaskManager&) = delete;

    [[nodiscard]] std::string Enqueue(const ExportConfiguration& configuration);

    bool Cancel(std::string_view taskId);

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

    bool RunSatelliteExport(const QueuedExport& queuedExport, size_t stepIndex,
                            size_t stepCount, std::string& error);

    bool RunGisExportGroup(const QueuedExport& queuedExport,
                           const GisExportGroup& group, size_t stepIndex,
                           size_t stepCount, std::string& error);

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
};
} // namespace DomDownToolPreview
