#include "preview/ExportTaskManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <system_error>
#include <utility>

#include "ArcGISDown.h"
#include "BaiduDown.h"
#include "BingDown.h"
#include "DownToolBase.h"
#include "GaoDeDown.h"
#include "GoogleDown.h"
#include "TencentDown.h"
#include "TianDiTuDown.h"

#include "cpl_json.h"
#include "fmt/format.h"

namespace
{
using DomDownToolPreview::ExportConfiguration;
using DomDownToolPreview::ExportTaskRecord;
using DomDownToolPreview::ExportTaskState;
using DomDownToolPreview::GisExportGroup;

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string CurrentTimeText()
{
    const std::time_t currentTime = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &currentTime);
    std::ostringstream output;
    output << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

std::string MakeTaskId()
{
    static std::atomic_uint64_t sequence = 0;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return fmt::format("{}-{}", milliseconds.count(), sequence.fetch_add(1));
}

std::string CategoryName(DownTool::AmapVectorRenderCategory category)
{
    switch (category)
    {
    case DownTool::AmapVectorRenderCategory::Point:
        return "点";
    case DownTool::AmapVectorRenderCategory::Road:
        return "道路";
    case DownTool::AmapVectorRenderCategory::Green:
        return "绿地";
    case DownTool::AmapVectorRenderCategory::Highway:
        return "高速";
    case DownTool::AmapVectorRenderCategory::Subway:
        return "地铁";
    case DownTool::AmapVectorRenderCategory::Building:
        return "建筑";
    case DownTool::AmapVectorRenderCategory::Water:
        return "水";
    case DownTool::AmapVectorRenderCategory::Boundary:
        return "边界";
    case DownTool::AmapVectorRenderCategory::Other:
        return "其他";
    case DownTool::AmapVectorRenderCategory::All:
        return "全部";
    }
    return "GIS";
}

std::string MakeGroupName(const GisExportGroup& group)
{
    if (!group.name.empty())
    {
        return group.name;
    }

    std::string name;
    for (const DownTool::AmapVectorRenderCategory category : group.categories)
    {
        if (!name.empty())
        {
            name += "+";
        }
        name += CategoryName(category);
    }
    return name.empty() ? "GIS" : name;
}

std::string SanitizeFileName(std::string name)
{
    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    std::replace_if(
        name.begin(), name.end(),
        [invalidCharacters](char character)
        {
            return static_cast<unsigned char>(character) < 32 ||
                invalidCharacters.find(character) != std::string_view::npos;
        },
        '_');
    return name.empty() ? "GIS" : name;
}

std::unique_ptr<DownTool::DownToolBase> CreateDownloader(std::string_view sourceId)
{
    if (sourceId == "gaode.satellite")
    {
        return std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::Satellite);
    }
    if (sourceId == "gaode.electronic")
    {
        return std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::Electronic);
    }
    if (sourceId == "gaode.roads")
    {
        return std::make_unique<DownTool::GaoDeDown>(
            DownTool::GaoDeMapType::RoadNetworkOverlay);
    }
    if (sourceId == "tencent.satellite")
    {
        return std::make_unique<DownTool::TencentDown>();
    }
    if (sourceId == "tianditu.satellite")
    {
        return std::make_unique<DownTool::TianDiTuDown>();
    }
    if (sourceId == "baidu.satellite")
    {
        return std::make_unique<DownTool::BaiduDown>();
    }
    if (sourceId == "google.satellite")
    {
        return std::make_unique<DownTool::GoogleDown>();
    }
    if (sourceId == "bing.satellite")
    {
        return std::make_unique<DownTool::BingDown>();
    }
    if (sourceId == "arcgis.satellite")
    {
        return std::make_unique<DownTool::ArcGISDown>();
    }
    return nullptr;
}

double ProgressRatio(uint64_t completed, uint64_t total)
{
    if (total == 0)
    {
        return 0.;
    }
    return std::clamp(static_cast<double>(completed) / static_cast<double>(total), 0., 1.);
}

double DownToolPhaseProgress(const DownTool::DownToolProgress& progress)
{
    const double phaseRatio = ProgressRatio(progress.completed, progress.total);
    if (progress.stage == "下载瓦片")
    {
        return phaseRatio * 0.45;
    }
    if (progress.stage.find("合并大图") != std::string::npos)
    {
        return 0.72 + phaseRatio * 0.16;
    }
    if (progress.stage.find("FBX") != std::string::npos)
    {
        return 0.88 + phaseRatio * 0.12;
    }
    return 0.45 + phaseRatio * 0.27;
}

std::string BuildTaskTitle(const ExportConfiguration& configuration)
{
    std::string contents;
    if (configuration.exportSatelliteTiles || configuration.exportSatelliteMergedImage ||
        (configuration.exportFbx &&
         configuration.fbxSource == DomDownToolPreview::ExportFbxSource::Satellite))
    {
        contents = configuration.mapSourceName;
    }
    if (configuration.exportGisTiles || configuration.exportGisMergedImage ||
        (configuration.exportFbx &&
         configuration.fbxSource == DomDownToolPreview::ExportFbxSource::Gis))
    {
        if (!contents.empty())
        {
            contents += " + ";
        }
        contents += "高德 GIS";
    }
    return fmt::format("z{} {:.0f} 米 {}", configuration.zoom,
                       configuration.radiusMeters, contents);
}

std::string BuildTaskDetail(const ExportConfiguration& configuration)
{
    std::vector<std::string> outputs;
    if (configuration.exportSatelliteTiles)
    {
        outputs.emplace_back("卫星 XYZ");
    }
    if (configuration.exportGisTiles)
    {
        outputs.emplace_back("GIS XYZ");
    }
    if (configuration.exportSatelliteMergedImage)
    {
        outputs.emplace_back("卫星大图");
    }
    if (configuration.exportGisMergedImage)
    {
        outputs.emplace_back("GIS 大图");
    }
    if (configuration.exportFbx)
    {
        outputs.emplace_back(configuration.fbxSource ==
                DomDownToolPreview::ExportFbxSource::Satellite ?
            "卫星 FBX" : "GIS FBX");
    }

    std::string result;
    for (const std::string& output : outputs)
    {
        if (!result.empty())
        {
            result += "、";
        }
        result += output;
    }
    return result;
}

std::string BuildTaskOutputPath(const ExportConfiguration& configuration)
{
    const bool satelliteRequested = configuration.exportSatelliteTiles ||
        configuration.exportSatelliteMergedImage ||
        (configuration.exportFbx &&
         configuration.fbxSource == DomDownToolPreview::ExportFbxSource::Satellite);
    const bool gisRequested = configuration.exportGisTiles ||
        configuration.exportGisMergedImage ||
        (configuration.exportFbx &&
         configuration.fbxSource == DomDownToolPreview::ExportFbxSource::Gis);
    std::string result;
    if (satelliteRequested)
    {
        result = PathToUtf8(configuration.satelliteOutputPath);
    }
    if (gisRequested)
    {
        if (!result.empty())
        {
            result += "；";
        }
        result += PathToUtf8(configuration.gisOutputPath);
    }
    return result;
}

ExportTaskState ParseTaskState(std::string_view value)
{
    if (value == "running")
    {
        return ExportTaskState::Running;
    }
    if (value == "succeeded")
    {
        return ExportTaskState::Succeeded;
    }
    if (value == "failed")
    {
        return ExportTaskState::Failed;
    }
    if (value == "canceled")
    {
        return ExportTaskState::Canceled;
    }
    return ExportTaskState::Queued;
}

std::string TaskStateValue(ExportTaskState state)
{
    switch (state)
    {
    case ExportTaskState::Queued:
        return "queued";
    case ExportTaskState::Running:
        return "running";
    case ExportTaskState::Succeeded:
        return "succeeded";
    case ExportTaskState::Failed:
        return "failed";
    case ExportTaskState::Canceled:
        return "canceled";
    }
    return "failed";
}
} // namespace

std::string DomDownToolPreview::ExportTaskStateName(ExportTaskState state)
{
    switch (state)
    {
    case ExportTaskState::Queued:
        return "排队中";
    case ExportTaskState::Running:
        return "导出中";
    case ExportTaskState::Succeeded:
        return "已完成";
    case ExportTaskState::Failed:
        return "失败";
    case ExportTaskState::Canceled:
        return "已取消";
    }
    return "未知";
}

DomDownToolPreview::ExportTaskManager::ExportTaskManager(
    std::function<void()> changeCallback, std::filesystem::path recordsPath)
    : changeCallback_(std::move(changeCallback)), recordsPath_(std::move(recordsPath))
{
    LoadRecords();
    worker_ = std::thread([this] { WorkerLoop(); });
}

DomDownToolPreview::ExportTaskManager::~ExportTaskManager()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        cancelActive_.store(true, std::memory_order_relaxed);
        if (activeDownloader_)
        {
            activeDownloader_->StopDown();
        }
    }
    condition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    SaveRecords(true);
}

std::string DomDownToolPreview::ExportTaskManager::Enqueue(
    const ExportConfiguration& configuration)
{
    ExportTaskRecord record;
    record.id = MakeTaskId();
    record.title = BuildTaskTitle(configuration);
    record.detail = BuildTaskDetail(configuration);
    record.outputPath = BuildTaskOutputPath(configuration);
    record.createdAt = CurrentTimeText();
    const std::string taskId = record.id;
    {
        const std::lock_guard lock(mutex_);
        records_.insert(records_.begin(), record);
        queue_.push_back(QueuedExport{taskId, configuration});
    }
    SaveRecords(true);
    condition_.notify_one();
    NotifyChanged();
    return taskId;
}

bool DomDownToolPreview::ExportTaskManager::Cancel(std::string_view taskId)
{
    bool changed = false;
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end())
        {
            return false;
        }
        if (record->state == ExportTaskState::Queued)
        {
            queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                       [taskId](const QueuedExport& item)
                                       { return item.taskId == taskId; }),
                         queue_.end());
            record->state = ExportTaskState::Canceled;
            record->stage = "已取消";
            record->finishedAt = CurrentTimeText();
            changed = true;
        }
        else if (record->state == ExportTaskState::Running)
        {
            cancelActive_.store(true, std::memory_order_relaxed);
            if (activeDownloader_)
            {
                activeDownloader_->StopDown();
            }
            record->stage = "正在取消";
            changed = true;
        }
    }
    if (changed)
    {
        SaveRecords(true);
        NotifyChanged();
    }
    return changed;
}

void DomDownToolPreview::ExportTaskManager::ClearFinished()
{
    {
        const std::lock_guard lock(mutex_);
        records_.erase(std::remove_if(
                           records_.begin(), records_.end(),
                           [](const ExportTaskRecord& record)
                           {
                               return record.state == ExportTaskState::Succeeded ||
                                   record.state == ExportTaskState::Failed ||
                                   record.state == ExportTaskState::Canceled;
                           }),
                       records_.end());
    }
    SaveRecords(true);
    NotifyChanged();
}

std::vector<DomDownToolPreview::ExportTaskRecord>
DomDownToolPreview::ExportTaskManager::GetRecords() const
{
    const std::lock_guard lock(mutex_);
    return records_;
}

void DomDownToolPreview::ExportTaskManager::WorkerLoop()
{
    while (true)
    {
        QueuedExport queuedExport;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
            {
                return;
            }
            queuedExport = std::move(queue_.front());
            queue_.pop_front();
            cancelActive_.store(false, std::memory_order_relaxed);
            const auto record = std::find_if(
                records_.begin(), records_.end(),
                [&queuedExport](const ExportTaskRecord& item)
                { return item.id == queuedExport.taskId; });
            if (record == records_.end() || record->state != ExportTaskState::Queued)
            {
                continue;
            }
            record->state = ExportTaskState::Running;
            record->stage = "准备导出";
        }
        SaveRecords(true);
        NotifyChanged();

        std::string error;
        bool succeeded = false;
        try
        {
            succeeded = RunExport(queuedExport, error);
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
        }
        catch (...)
        {
            error = "导出任务发生未知异常。";
        }

        if (IsCancellationRequested())
        {
            FinishTask(queuedExport.taskId, ExportTaskState::Canceled);
        }
        else if (succeeded)
        {
            FinishTask(queuedExport.taskId, ExportTaskState::Succeeded);
        }
        else
        {
            FinishTask(queuedExport.taskId, ExportTaskState::Failed, std::move(error));
        }
    }
}

bool DomDownToolPreview::ExportTaskManager::RunExport(
    const QueuedExport& queuedExport, std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    const bool satelliteRequested = configuration.exportSatelliteTiles ||
        configuration.exportSatelliteMergedImage ||
        (configuration.exportFbx && configuration.fbxSource == ExportFbxSource::Satellite);
    const bool gisRequested = configuration.exportGisTiles ||
        configuration.exportGisMergedImage ||
        (configuration.exportFbx && configuration.fbxSource == ExportFbxSource::Gis);
    const size_t stepCount = (satelliteRequested ? 1u : 0u) +
        (gisRequested ? configuration.gisGroups.size() : 0u);
    if (stepCount == 0)
    {
        error = "没有选择任何导出内容。";
        return false;
    }

    size_t stepIndex = 0;
    if (satelliteRequested)
    {
        if (!RunSatelliteExport(queuedExport, stepIndex++, stepCount, error))
        {
            return false;
        }
    }
    if (gisRequested)
    {
        if (configuration.gisGroups.empty())
        {
            error = "GIS 导出至少需要一个分类组。";
            return false;
        }
        for (const GisExportGroup& group : configuration.gisGroups)
        {
            if (!RunGisExportGroup(queuedExport, group, stepIndex++, stepCount, error))
            {
                return false;
            }
        }
    }
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunSatelliteExport(
    const QueuedExport& queuedExport, size_t stepIndex, size_t stepCount,
    std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    std::unique_ptr<DownTool::DownToolBase> downloader =
        CreateDownloader(configuration.mapSourceId);
    if (!downloader)
    {
        error = "当前地图源不能导出卫星图。";
        return false;
    }

    downloader->SetCenterLonLat(configuration.centerWgs84);
    downloader->SetRadius(configuration.radiusMeters);
    downloader->SetZoom(configuration.zoom);
    downloader->SetCachePath(PathToUtf8(configuration.satelliteCachePath));
    downloader->SetMaxDownQueue(std::max(1u, std::thread::hardware_concurrency()));
    downloader->SetPostProcessOutputPath(PathToUtf8(configuration.satelliteOutputPath));
    downloader->SetPostProcessEnabled(configuration.exportSatelliteTiles);
    downloader->SetPostProcessMergedImageEnabled(configuration.exportSatelliteMergedImage);
    std::filesystem::path mergedImagePath = configuration.satelliteOutputPath / "merged";
    mergedImagePath.replace_extension(configuration.mergedImageFormat);
    downloader->SetPostProcessMergedImageOutputPath(PathToUtf8(mergedImagePath));
    downloader->SetPostProcessMergedImageResolution(configuration.mergedImageResolution);
    const bool exportFbx = configuration.exportFbx &&
        configuration.fbxSource == ExportFbxSource::Satellite;
    downloader->SetPostProcessFbxEnabled(exportFbx);
    downloader->SetFbxOutputPath(PathToUtf8(configuration.satelliteOutputPath));
    downloader->SetFbxTextureSize(configuration.fbxTextureSize);
    downloader->SetProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex, stepCount](
            const DownTool::DownToolProgress& progress)
        {
            const double overall = (static_cast<double>(stepIndex) +
                                    DownToolPhaseProgress(progress)) /
                static_cast<double>(stepCount);
            UpdateProgress(taskId, progress.stage, overall,
                           progress.completed, progress.total);
        });
    SetActiveDownloader(downloader.get());
    try
    {
        downloader->StartDown();
    }
    catch (...)
    {
        ClearActiveDownloader();
        throw;
    }
    ClearActiveDownloader();
    if (IsCancellationRequested())
    {
        return false;
    }

    if (configuration.exportSatelliteTiles &&
        !std::filesystem::exists(configuration.satelliteOutputPath / "meta.json"))
    {
        error = "卫星 XYZ 瓦片未生成完整元数据。";
        return false;
    }
    if (configuration.exportSatelliteMergedImage && !std::filesystem::exists(mergedImagePath))
    {
        error = "卫星合并大图未生成。";
        return false;
    }
    if (exportFbx && !std::filesystem::exists(
            configuration.satelliteOutputPath / "textures" / "fbx" /
            std::to_string(configuration.fbxTextureSize) / "manifest.json"))
    {
        error = "卫星 FBX 贴图清单未生成。";
        return false;
    }

    UpdateProgress(queuedExport.taskId, "卫星图导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunGisExportGroup(
    const QueuedExport& queuedExport, const GisExportGroup& group,
    size_t stepIndex, size_t stepCount, std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (group.categories.empty())
    {
        error = "GIS 导出组不能为空。";
        return false;
    }

    const std::string groupName = SanitizeFileName(MakeGroupName(group));
    const std::filesystem::path temporaryRoot =
        std::filesystem::u8path("cache/高德地图/gis数据/临时");
    std::error_code cleanupError;
    std::filesystem::remove_all(temporaryRoot, cleanupError);

    DownTool::AmapVectorPngDownloadOptions options;
    options.vectorDownload.centerWgs84 = configuration.centerWgs84;
    options.vectorDownload.radiusMeters = configuration.radiusMeters;
    options.vectorDownload.zoom = configuration.zoom;
    options.vectorDownload.includeBaseLayers = true;
    options.vectorDownload.includeLabels =
        group.categories.contains(DownTool::AmapVectorRenderCategory::Point);
    options.vectorDownload.maxConcurrency = 0;
    options.vectorDownload.cancellationRequested =
        [this] { return IsCancellationRequested(); };
    options.vectorDownload.progressCallback =
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::AmapVectorDownloadProgress& progress)
        {
            const double subProgress = ProgressRatio(
                progress.completedTaskCount, progress.totalTaskCount) * 0.3;
            UpdateProgress(taskId, "下载 GIS PBF · " + groupName,
                           (static_cast<double>(stepIndex) + subProgress) /
                               static_cast<double>(stepCount),
                           progress.completedTaskCount, progress.totalTaskCount);
        };
    options.render.categories = group.categories;
    options.render.colors = configuration.gisColors;
    options.render.colorBuildingsByHeight = configuration.colorBuildingsByHeight;
    options.render.pngTileCachePath = temporaryRoot;
    options.render.maxConcurrency = 0;
    options.render.cancellationRequested =
        [this] { return IsCancellationRequested(); };
    options.render.progressCallback =
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::AmapVectorRenderProgress& progress)
        {
            const double subProgress = 0.3 + ProgressRatio(
                progress.completedTileCount, progress.totalTileCount) * 0.35;
            UpdateProgress(taskId, "渲染 GIS PNG · " + groupName,
                           (static_cast<double>(stepIndex) + subProgress) /
                               static_cast<double>(stepCount),
                           progress.completedTileCount, progress.totalTileCount);
        };

    const std::filesystem::path tileOutputPath =
        configuration.gisOutputPath / std::filesystem::u8path("瓦片") /
        std::filesystem::u8path(groupName);
    const std::filesystem::path postProcessPath = configuration.exportGisTiles ?
        tileOutputPath : temporaryRoot / std::filesystem::u8path("后处理");
    std::filesystem::path mergedImagePath =
        configuration.gisOutputPath / std::filesystem::u8path("大图") /
        std::filesystem::u8path(groupName);
    mergedImagePath.replace_extension(configuration.mergedImageFormat);
    const std::filesystem::path fbxOutputPath =
        configuration.gisOutputPath / "FBX" / std::filesystem::u8path(groupName);

    DownTool::AmapVectorPngDownloader downloader;
    downloader.SetPostProcessOutputPath(PathToUtf8(postProcessPath));
    downloader.SetPostProcessEnabled(configuration.exportGisTiles);
    downloader.SetPostProcessMergedImageEnabled(configuration.exportGisMergedImage);
    downloader.SetPostProcessMergedImageOutputPath(PathToUtf8(mergedImagePath));
    downloader.SetPostProcessMergedImageResolution(configuration.mergedImageResolution);
    const bool exportFbx = configuration.exportFbx &&
        configuration.fbxSource == ExportFbxSource::Gis;
    downloader.SetPostProcessFbxEnabled(exportFbx);
    downloader.SetFbxOutputPath(PathToUtf8(fbxOutputPath));
    downloader.SetFbxTextureSize(configuration.fbxTextureSize);
    downloader.SetMaxDownQueue(std::max(1u, std::thread::hardware_concurrency()));
    downloader.SetProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::DownToolProgress& progress)
        {
            const double subProgress = 0.65 + DownToolPhaseProgress(progress) * 0.35;
            UpdateProgress(taskId, progress.stage + " · " + groupName,
                           (static_cast<double>(stepIndex) + subProgress) /
                               static_cast<double>(stepCount),
                           progress.completed, progress.total);
        });
    SetActiveDownloader(&downloader);
    DownTool::AmapVectorPngDownloadResult result;
    try
    {
        result = downloader.DownloadPngTiles(options);
    }
    catch (...)
    {
        ClearActiveDownloader();
        throw;
    }
    ClearActiveDownloader();
    if (!result.success)
    {
        error = result.error;
        return false;
    }
    if (configuration.exportGisTiles &&
        !std::filesystem::exists(tileOutputPath / "meta.json"))
    {
        error = "GIS XYZ 瓦片未生成完整元数据：" + groupName;
        return false;
    }
    if (configuration.exportGisMergedImage && !std::filesystem::exists(mergedImagePath))
    {
        error = "GIS 合并大图未生成：" + groupName;
        return false;
    }
    if (exportFbx && !std::filesystem::exists(
            fbxOutputPath / "textures" / "fbx" /
            std::to_string(configuration.fbxTextureSize) / "manifest.json"))
    {
        error = "GIS FBX 贴图清单未生成：" + groupName;
        return false;
    }

    UpdateProgress(queuedExport.taskId, "GIS 导出完成 · " + groupName,
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

void DomDownToolPreview::ExportTaskManager::UpdateProgress(
    std::string_view taskId, std::string stage, double progress,
    uint64_t completed, uint64_t total)
{
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end() || record->state != ExportTaskState::Running)
        {
            return;
        }
        record->stage = std::move(stage);
        record->progress = std::max(record->progress, std::clamp(progress, 0., 1.));
        record->completed = completed;
        record->total = total;
    }
    SaveRecords(false);
    NotifyChanged();
}

void DomDownToolPreview::ExportTaskManager::FinishTask(
    std::string_view taskId, ExportTaskState state, std::string error)
{
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end())
        {
            return;
        }
        record->state = state;
        record->stage = ExportTaskStateName(state);
        record->finishedAt = CurrentTimeText();
        record->error = std::move(error);
        if (state == ExportTaskState::Succeeded)
        {
            record->progress = 1.;
        }
    }
    SaveRecords(true);
    NotifyChanged();
}

void DomDownToolPreview::ExportTaskManager::SetActiveDownloader(
    DownTool::DownToolBase* downloader)
{
    const std::lock_guard lock(mutex_);
    activeDownloader_ = downloader;
}

void DomDownToolPreview::ExportTaskManager::ClearActiveDownloader()
{
    const std::lock_guard lock(mutex_);
    activeDownloader_ = nullptr;
}

void DomDownToolPreview::ExportTaskManager::LoadRecords()
{
    std::ifstream input(recordsPath_, std::ios::binary);
    if (!input.is_open())
    {
        return;
    }
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    input.close();
    CPLJSONDocument document;
    if (!document.LoadMemory(json))
    {
        return;
    }

    const CPLJSONArray tasks = document.GetRoot().GetArray("tasks");
    bool recordsChanged = false;
    for (int index = 0; index < tasks.Size(); ++index)
    {
        const CPLJSONObject item = tasks[index];
        ExportTaskRecord record;
        record.id = item.GetString("id");
        record.title = item.GetString("title");
        record.detail = item.GetString("detail");
        record.outputPath = item.GetString("outputPath");
        record.createdAt = item.GetString("createdAt");
        record.finishedAt = item.GetString("finishedAt");
        record.state = ParseTaskState(item.GetString("state"));
        record.stage = item.GetString("stage");
        record.progress = item.GetDouble("progress");
        record.completed = static_cast<uint64_t>(std::max(0., item.GetDouble("completed")));
        record.total = static_cast<uint64_t>(std::max(0., item.GetDouble("total")));
        record.error = item.GetString("error");
        if (record.id.empty())
        {
            continue;
        }
        if (record.state == ExportTaskState::Queued ||
            record.state == ExportTaskState::Running)
        {
            record.state = ExportTaskState::Failed;
            record.stage = "已中断";
            record.finishedAt = CurrentTimeText();
            record.error = "DomDownToolPreview 上次退出时任务尚未完成。";
            recordsChanged = true;
        }
        records_.push_back(std::move(record));
    }
    if (recordsChanged)
    {
        SaveRecords(true);
    }
}

void DomDownToolPreview::ExportTaskManager::SaveRecords(bool force)
{
    const std::lock_guard fileLock(recordsFileMutex_);
    std::vector<ExportTaskRecord> records;
    {
        const std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastSaveTime_ != std::chrono::steady_clock::time_point{} &&
            now - lastSaveTime_ < std::chrono::milliseconds(500))
        {
            return;
        }
        lastSaveTime_ = now;
        records = records_;
    }

    CPLJSONArray tasks;
    for (const ExportTaskRecord& record : records)
    {
        CPLJSONObject item;
        item.Add("id", record.id);
        item.Add("title", record.title);
        item.Add("detail", record.detail);
        item.Add("outputPath", record.outputPath);
        item.Add("createdAt", record.createdAt);
        item.Add("finishedAt", record.finishedAt);
        item.Add("state", TaskStateValue(record.state));
        item.Add("stage", record.stage);
        item.Add("progress", record.progress);
        item.Add("completed", static_cast<double>(record.completed));
        item.Add("total", static_cast<double>(record.total));
        item.Add("error", record.error);
        tasks.Add(item);
    }
    CPLJSONObject root;
    root.Add("version", 1);
    root.Add("tasks", tasks);

    std::error_code directoryError;
    if (!recordsPath_.parent_path().empty())
    {
        std::filesystem::create_directories(recordsPath_.parent_path(), directoryError);
    }
    if (directoryError)
    {
        return;
    }
    std::ofstream output(recordsPath_, std::ios::binary | std::ios::trunc);
    if (output.is_open())
    {
        output << root.Format(CPLJSONObject::PrettyFormat::Pretty) << '\n';
    }
}

void DomDownToolPreview::ExportTaskManager::NotifyChanged() const
{
    if (changeCallback_)
    {
        changeCallback_();
    }
}

bool DomDownToolPreview::ExportTaskManager::IsCancellationRequested() const
{
    return cancelActive_.load(std::memory_order_relaxed);
}
