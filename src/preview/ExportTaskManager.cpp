#include "preview/ExportTaskManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "ArcGISDown.h"
#include "BaiduDown.h"
#include "BingDown.h"
#include "DownToolBase.h"
#include "GaoDeDown.h"
#include "GoogleDown.h"
#include "TencentDown.h"
#include "TianDiTuDown.h"

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include "preview/ExportImageComposer.h"

namespace
{
using Json = nlohmann::json;
using DomDownToolPreview::ExportConfiguration;
using DomDownToolPreview::ExportTaskRecord;
using DomDownToolPreview::ExportTaskState;
using DomDownToolPreview::GisExportMode;
using DomDownToolPreview::SatelliteExportSource;

constexpr auto kProgressNotificationInterval = std::chrono::milliseconds(100);

std::string GetJsonString(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return {};
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_string()
        ? iterator->get<std::string>()
        : std::string{};
}

double GetJsonDouble(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return 0.;
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_number()
        ? iterator->get<double>()
        : 0.;
}

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

std::string MakeCategoryName(
    const std::set<DownTool::AmapVectorRenderCategory>& categories)
{
    std::string name;
    for (const DownTool::AmapVectorRenderCategory category : categories)
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

std::string GisModeName(GisExportMode mode)
{
    switch (mode)
    {
    case GisExportMode::OverlayOnSatellite:
        return "叠加卫星图";
    case GisExportMode::MergeSelectedLayers:
        return "GIS 合并";
    case GisExportMode::SeparateLayers:
        return "GIS 单层";
    }
    return "GIS";
}

std::filesystem::path TaskOutputRoot(const ExportConfiguration& configuration,
                                     const std::string& taskId)
{
    return DomDownToolPreview::ExportTaskOutputRoot(configuration, taskId);
}

std::filesystem::path SatelliteOutputRoot(const ExportConfiguration& configuration,
                                          const std::string& taskId,
                                          const SatelliteExportSource& source)
{
    const std::filesystem::path root = TaskOutputRoot(configuration, taskId);
    return root / std::filesystem::u8path("卫星图") /
        std::filesystem::u8path(SanitizeFileName(source.name));
}

std::filesystem::path GisOutputRoot(const ExportConfiguration& configuration,
                                    const std::string& taskId,
                                    std::string_view modeName,
                                    std::string_view categoryName)
{
    const std::filesystem::path root = TaskOutputRoot(configuration, taskId);
    return root / std::filesystem::u8path("GIS") /
        std::filesystem::u8path(modeName) /
        std::filesystem::u8path(categoryName);
}

std::filesystem::path OverlayOutputRoot(const ExportConfiguration& configuration,
                                        const std::string& taskId,
                                        const SatelliteExportSource& source)
{
    return TaskOutputRoot(configuration, taskId) /
        std::filesystem::u8path("叠加卫星图") /
        std::filesystem::u8path(SanitizeFileName(source.name));
}

std::string FormatSourceNames(const ExportConfiguration& configuration)
{
    std::string names;
    for (const SatelliteExportSource& source : configuration.satelliteSources)
    {
        if (!names.empty())
        {
            names += "、";
        }
        names += source.name;
    }
    return names;
}

std::string FormatGisModes(const ExportConfiguration& configuration)
{
    std::string modes;
    for (const GisExportMode mode : configuration.gisModes)
    {
        if (!modes.empty())
        {
            modes += "、";
        }
        modes += GisModeName(mode);
    }
    return modes;
}

std::string BuildTaskDetailsText(const ExportConfiguration& configuration)
{
    std::vector<std::string> outputs;
    if (configuration.exportTiles)
    {
        outputs.emplace_back("XYZ 切片");
    }
    if (configuration.exportMergedImage)
    {
        outputs.emplace_back("合并大图");
    }
    if (configuration.exportFbx)
    {
        outputs.emplace_back("FBX 切片与网格");
    }
    if (configuration.exportGeoJson)
    {
        outputs.emplace_back("GeoJSON");
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

bool WriteTaskRecordFile(const std::filesystem::path& outputRoot,
                         std::string_view taskId,
                         const ExportConfiguration& configuration,
                         std::string_view status,
                         std::string_view result,
                         std::string& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(outputRoot, directoryError);
    if (directoryError)
    {
        error = "无法创建导出记录目录：" + directoryError.message();
        return false;
    }

    std::ofstream output(
        outputRoot / std::filesystem::u8path(
            "转换记录-" + std::string(taskId) + ".txt"),
        std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        error = "无法创建导出转换记录。";
        return false;
    }

    output << "DomDownTool 导出转换记录\n"
           << "任务 ID: " << taskId << "\n"
           << "状态: " << status << "\n"
           << "记录更新时间: " << CurrentTimeText() << "\n"
           << std::fixed << std::setprecision(6)
           << "中心坐标 (WGS84): " << configuration.centerWgs84.lon << ", "
           << configuration.centerWgs84.lat << "\n"
           << std::setprecision(0)
           << "半径 (米): " << configuration.radiusMeters << "\n"
           << "层级: " << configuration.zoom << "\n"
           << "卫星图来源: " << FormatSourceNames(configuration) << "\n"
           << "用户指定输出根目录: "
           << (configuration.outputRoot.empty() ? "未指定，使用 cache" :
               PathToUtf8(configuration.outputRoot)) << "\n";
    for (const SatelliteExportSource& source : configuration.satelliteSources)
    {
        output << "卫星源: " << source.name << " [" << source.id << "]\n"
               << "  缓存目录: " << PathToUtf8(source.cachePath) << "\n"
               << "  产物目录: "
               << PathToUtf8(SatelliteOutputRoot(configuration, std::string(taskId), source))
               << "\n"
               << "  XYZ 目录: "
               << PathToUtf8(SatelliteOutputRoot(
                      configuration, std::string(taskId), source) / "XYZ")
               << "\n"
               << "  FBX 目录: "
               << PathToUtf8(SatelliteOutputRoot(
                      configuration, std::string(taskId), source) / "FBX")
               << "\n";
    }
    output
           << "GIS 产物根目录: "
           << PathToUtf8(TaskOutputRoot(configuration, std::string(taskId)) /
              std::filesystem::u8path("GIS")) << "\n"
           << "GIS 分类: " << MakeCategoryName(configuration.gisCategories) << "\n"
           << "GIS 组合方式: " << FormatGisModes(configuration) << "\n"
           << "导出类型: " << BuildTaskDetailsText(configuration) << "\n"
           << "合并大图: " << configuration.mergedImageFormat << "，最大边 "
           << configuration.mergedImageResolution << "\n"
           << "GIS 透明大图格式: PNG\n"
           << "FBX 贴图尺寸: " << configuration.fbxTextureSize << "\n"
           << "建筑按高度着色: "
           << (configuration.colorBuildingsByHeight ? "是" : "否") << "\n"
           << "结果: " << result << "\n";
    if (!output.good())
    {
        error = "写入导出转换记录失败。";
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> TaskRecordRoots(
    const ExportConfiguration& configuration, const std::string& taskId)
{
    return {TaskOutputRoot(configuration, taskId)};
}

bool WriteTaskRecordFiles(const ExportConfiguration& configuration,
                          const std::string& taskId,
                          std::string_view status,
                          std::string_view result,
                          std::string& error)
{
    for (const std::filesystem::path& root : TaskRecordRoots(configuration, taskId))
    {
        if (!WriteTaskRecordFile(root, taskId, configuration, status, result, error))
        {
            return false;
        }
    }
    return true;
}

DownTool::AmapVectorDecodedData FilterGisData(
    const DownTool::AmapVectorDecodedData& decodedData,
    const std::set<DownTool::AmapVectorRenderCategory>& categories,
    const std::function<bool()>& cancellationRequested)
{
    DownTool::AmapVectorDecodedData filteredData;
    filteredData.version = decodedData.version;
    filteredData.status = decodedData.status;
    filteredData.packetCount = decodedData.packetCount;
    filteredData.features.reserve(decodedData.features.size());
    for (const DownTool::AmapVectorFeature& feature : decodedData.features)
    {
        if (cancellationRequested && cancellationRequested())
        {
            break;
        }
        if (categories.contains(DownTool::ClassifyAmapVectorFeature(feature)))
        {
            filteredData.features.push_back(feature);
        }
    }
    return filteredData;
}

std::string BuildTaskTitle(const ExportConfiguration& configuration)
{
    std::string contents = FormatSourceNames(configuration);
    if (!configuration.gisCategories.empty())
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
    const std::string outputs = BuildTaskDetailsText(configuration);
    const std::string modes = FormatGisModes(configuration);
    return modes.empty() ? outputs : outputs + " · " + modes;
}

std::string BuildTaskOutputPath(const ExportConfiguration& configuration,
                                const std::string& taskId)
{
    std::string paths;
    for (const std::filesystem::path& root : TaskRecordRoots(configuration, taskId))
    {
        if (!paths.empty())
        {
            paths += "；";
        }
        paths += PathToUtf8(root);
    }
    return paths.empty() ? "无输出目录" : paths;
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

bool IsFinishedState(ExportTaskState state)
{
    return state == ExportTaskState::Succeeded || state == ExportTaskState::Failed ||
        state == ExportTaskState::Canceled;
}

std::optional<std::vector<std::filesystem::path>> ValidatedTaskOutputPaths(
    const ExportTaskRecord& record, std::string& error)
{
    constexpr std::string_view kSeparator = "；";
    std::vector<std::filesystem::path> paths;
    size_t start = 0;
    while (start <= record.outputPath.size())
    {
        const size_t end = record.outputPath.find(kSeparator, start);
        const std::string_view pathText = std::string_view(record.outputPath).substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (pathText.empty())
        {
            error = "任务导出路径为空，已阻止删除。";
            return std::nullopt;
        }

        std::error_code pathError;
        const std::filesystem::path path = std::filesystem::absolute(
            std::filesystem::u8path(std::string(pathText)), pathError).lexically_normal();
        if (pathError || path.filename() != std::filesystem::u8path(record.id) ||
            path.parent_path().filename() != std::filesystem::u8path("导出任务"))
        {
            error = "任务导出路径不符合“导出任务/任务ID”结构，已阻止删除。";
            return std::nullopt;
        }
        paths.push_back(path);
        if (end == std::string::npos)
        {
            break;
        }
        start = end + kSeparator.size();
    }
    return paths;
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

std::filesystem::path DomDownToolPreview::ExportTaskOutputRoot(
    const ExportConfiguration& configuration, std::string_view taskId)
{
    const std::filesystem::path outputRoot = configuration.outputRoot.empty() ?
        std::filesystem::u8path("cache") : configuration.outputRoot;
    return outputRoot / std::filesystem::u8path("导出任务") /
        std::filesystem::u8path(std::string(taskId));
}

DomDownToolPreview::ExportTaskManager::ExportTaskManager(
    std::function<void()> changeCallback, std::filesystem::path recordsPath,
    ExportDownloaderFactory downloaderFactory)
    : changeCallback_(std::move(changeCallback)),
      downloaderFactory_(std::move(downloaderFactory)),
      recordsPath_(std::move(recordsPath))
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
    record.outputPath = BuildTaskOutputPath(configuration, record.id);
    record.createdAt = CurrentTimeText();
    const std::string taskId = record.id;
    std::string recordError;
    const bool recordWritten = WriteTaskRecordFiles(
        configuration, taskId, "排队中", "等待执行", recordError);
    if (!recordWritten)
    {
        record.state = ExportTaskState::Failed;
        record.stage = "记录失败";
        record.finishedAt = CurrentTimeText();
        record.error = recordError;
    }
    {
        const std::lock_guard lock(mutex_);
        records_.insert(records_.begin(), record);
        if (recordWritten)
        {
            queue_.push_back(QueuedExport{taskId, configuration});
        }
    }
    SaveRecords(true);
    condition_.notify_one();
    NotifyChanged();
    return taskId;
}

bool DomDownToolPreview::ExportTaskManager::Cancel(std::string_view taskId)
{
    bool changed = false;
    std::optional<ExportConfiguration> queuedConfiguration;
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
            const auto queued = std::find_if(
                queue_.begin(), queue_.end(),
                [taskId](const QueuedExport& item) { return item.taskId == taskId; });
            if (queued != queue_.end())
            {
                queuedConfiguration = queued->configuration;
            }
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
        if (queuedConfiguration.has_value())
        {
            std::string recordError;
            WriteTaskRecordFiles(
                *queuedConfiguration, std::string(taskId), "已取消", "任务已取消", recordError);
        }
        SaveRecords(true);
        NotifyChanged();
    }
    return changed;
}

bool DomDownToolPreview::ExportTaskManager::RemoveFinished(std::string_view taskId)
{
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end() || !IsFinishedState(record->state))
        {
            return false;
        }
        records_.erase(record);
    }
    SaveRecords(true);
    NotifyChanged();
    return true;
}

bool DomDownToolPreview::ExportTaskManager::DeleteFinishedOutput(
    std::string_view taskId, std::string& error)
{
    ExportTaskRecord record;
    {
        const std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (found == records_.end() || !IsFinishedState(found->state))
        {
            error = "只能永久删除已完成、失败或已取消的任务。";
            return false;
        }
        record = *found;
    }

    const std::optional<std::vector<std::filesystem::path>> outputPaths =
        ValidatedTaskOutputPaths(record, error);
    if (!outputPaths.has_value())
    {
        return false;
    }
    for (const std::filesystem::path& outputPath : *outputPaths)
    {
        std::error_code removeError;
        std::filesystem::remove_all(outputPath, removeError);
        if (removeError)
        {
            error = fmt::format("无法删除导出目录：{}（{}）",
                                PathToUtf8(outputPath), removeError.message());
            return false;
        }
    }
    if (!RemoveFinished(taskId))
    {
        error = "导出文件已删除，但任务记录状态已变化，无法移除记录。";
        return false;
    }
    return true;
}

void DomDownToolPreview::ExportTaskManager::ClearFinished()
{
    {
        const std::lock_guard lock(mutex_);
        records_.erase(std::remove_if(
                           records_.begin(), records_.end(),
                           [](const ExportTaskRecord& record)
                           {
                               return IsFinishedState(record.state);
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
            lastProgressNotificationTime_ = {};
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

        ExportTaskState finalState = ExportTaskState::Failed;
        if (IsCancellationRequested())
        {
            finalState = ExportTaskState::Canceled;
        }
        else if (succeeded)
        {
            finalState = ExportTaskState::Succeeded;
        }
        const std::string result = finalState == ExportTaskState::Succeeded ?
            "导出完成" : finalState == ExportTaskState::Canceled ?
            "任务已取消" : error;
        std::string recordError;
        if (!WriteTaskRecordFiles(queuedExport.configuration, queuedExport.taskId,
                                  ExportTaskStateName(finalState), result, recordError))
        {
            finalState = ExportTaskState::Failed;
            error = error.empty() ? recordError : error + "；" + recordError;
        }
        FinishTask(queuedExport.taskId, finalState, std::move(error));
    }
}

bool DomDownToolPreview::ExportTaskManager::RunExport(
    const QueuedExport& queuedExport, std::string& error)
{
    if (IsCancellationRequested())
    {
        return false;
    }

    const ExportConfiguration& configuration = queuedExport.configuration;
    const bool satelliteRequested = !configuration.satelliteSources.empty() &&
        (configuration.exportTiles || configuration.exportMergedImage ||
         configuration.exportFbx);
    const bool gisVisualRequested = !configuration.gisCategories.empty() &&
        !configuration.gisModes.empty() &&
        (configuration.exportTiles || configuration.exportMergedImage ||
         configuration.exportFbx);
    if (!configuration.gisModes.empty() &&
        !configuration.exportTiles && !configuration.exportMergedImage &&
        !configuration.exportFbx)
    {
        error = "GIS 组合方式需要选择 XYZ、合并大图或 FBX。";
        return false;
    }
    if (!configuration.gisCategories.empty() &&
        (configuration.exportTiles || configuration.exportMergedImage ||
         configuration.exportFbx) && configuration.gisModes.empty() &&
        !configuration.exportGeoJson)
    {
        error = "所选 GIS 数据需要至少一种 GIS 组合方式。";
        return false;
    }
    const bool gisRequested = gisVisualRequested ||
        (!configuration.gisCategories.empty() && configuration.exportGeoJson);
    const size_t separateCount = gisVisualRequested && configuration.gisModes.contains(
        GisExportMode::SeparateLayers) ? configuration.gisCategories.size() : 0u;
    const size_t mergedCount = gisVisualRequested && configuration.gisModes.contains(
        GisExportMode::MergeSelectedLayers) ? 1u : 0u;
    const size_t overlayCount = gisVisualRequested && configuration.gisModes.contains(
        GisExportMode::OverlayOnSatellite) ? 1u : 0u;
    const size_t geoJsonCount = configuration.exportGeoJson ? 1u : 0u;
    const size_t stepCount = (satelliteRequested ? configuration.satelliteSources.size() : 0u) +
        (gisRequested ? separateCount + mergedCount + overlayCount + geoJsonCount : 0u);
    if (stepCount == 0)
    {
        error = "没有选择任何导出内容。";
        return false;
    }

    if (configuration.gisModes.contains(GisExportMode::OverlayOnSatellite) &&
        configuration.satelliteSources.empty())
    {
        error = "GIS 叠加输出至少需要选择一个卫星图来源。";
        return false;
    }

    size_t stepIndex = 0;
    if (satelliteRequested)
    {
        for (const SatelliteExportSource& source : configuration.satelliteSources)
        {
            if (IsCancellationRequested())
            {
                return false;
            }
            if (!RunSatelliteExport(queuedExport, source, stepIndex++, stepCount, error))
            {
                return false;
            }
        }
    }
    if (gisRequested)
    {
        if (IsCancellationRequested())
        {
            return false;
        }
        if (mergedCount != 0 &&
            !RunGisExport(queuedExport, configuration.gisCategories,
                          GisExportMode::MergeSelectedLayers, "合并图层",
                          stepIndex++, stepCount, error))
        {
            return false;
        }
        if (overlayCount != 0 &&
            !RunGisExport(queuedExport, configuration.gisCategories,
                          GisExportMode::OverlayOnSatellite, "叠加卫星图",
                          stepIndex++, stepCount, error))
        {
            return false;
        }
        if (separateCount != 0)
        {
            for (const DownTool::AmapVectorRenderCategory category :
                 configuration.gisCategories)
            {
                if (IsCancellationRequested())
                {
                    return false;
                }
                if (!RunGisExport(queuedExport, {category}, GisExportMode::SeparateLayers,
                                  CategoryName(category), stepIndex++, stepCount, error))
                {
                    return false;
                }
            }
        }
        if (geoJsonCount != 0 &&
            !RunGeoJsonExport(queuedExport, stepIndex++, stepCount, error))
        {
            return false;
        }
    }
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunSatelliteExport(
    const QueuedExport& queuedExport, const SatelliteExportSource& source,
    size_t stepIndex, size_t stepCount,
    std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return false;
    }
    std::unique_ptr<DownTool::DownToolBase> downloader =
        downloaderFactory_ ? downloaderFactory_(source.id) : CreateDownloader(source.id);
    if (!downloader)
    {
        error = "当前地图源不能导出卫星图。";
        return false;
    }
    downloader->SetCancellationRequestedCallback(
        [this] { return IsCancellationRequested(); });
    if (IsCancellationRequested())
    {
        return false;
    }

    downloader->SetCenterLonLat(configuration.centerWgs84);
    downloader->SetRadius(configuration.radiusMeters);
    downloader->SetZoom(configuration.zoom);
    downloader->SetCachePath(PathToUtf8(source.cachePath));
    downloader->SetMaxDownQueue(std::max(1u, std::thread::hardware_concurrency()));
    const std::filesystem::path outputRoot = SatelliteOutputRoot(
        configuration, queuedExport.taskId, source);
    std::error_code outputError;
    std::filesystem::create_directories(outputRoot, outputError);
    if (outputError)
    {
        error = "无法创建卫星图输出目录：" + outputError.message();
        return false;
    }
    const std::filesystem::path xyzOutputRoot = outputRoot / "XYZ";
    const std::filesystem::path fbxOutputRoot = outputRoot / "FBX";
    downloader->SetPostProcessOutputPath(PathToUtf8(xyzOutputRoot));
    downloader->SetPostProcessEnabled(configuration.exportTiles);
    downloader->SetPostProcessMergedImageEnabled(configuration.exportMergedImage);
    std::filesystem::path mergedImagePath = outputRoot / "merged";
    mergedImagePath.replace_extension(configuration.mergedImageFormat);
    downloader->SetPostProcessMergedImageOutputPath(PathToUtf8(mergedImagePath));
    downloader->SetPostProcessMergedImageResolution(configuration.mergedImageResolution);
    const bool exportFbx = configuration.exportFbx;
    downloader->SetPostProcessFbxEnabled(exportFbx);
    downloader->SetFbxOutputPath(PathToUtf8(fbxOutputRoot));
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
    if (IsCancellationRequested())
    {
        downloader->StopDown();
        ClearActiveDownloader();
        return false;
    }
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

    if (configuration.exportTiles &&
        !std::filesystem::exists(xyzOutputRoot / "meta.json"))
    {
        error = "卫星 XYZ 瓦片未生成完整元数据。";
        return false;
    }
    if (configuration.exportMergedImage && !std::filesystem::exists(mergedImagePath))
    {
        error = "卫星合并大图未生成。";
        return false;
    }
    if (exportFbx && !std::filesystem::exists(fbxOutputRoot / "manifest.json"))
    {
        error = "卫星 FBX 贴图清单未生成。";
        return false;
    }

    UpdateProgress(queuedExport.taskId, "卫星图导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunGisExport(
    const QueuedExport& queuedExport,
    const std::set<DownTool::AmapVectorRenderCategory>& categories,
    GisExportMode mode, std::string_view name,
    size_t stepIndex, size_t stepCount, std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return false;
    }
    if (categories.empty())
    {
        error = "GIS 导出组不能为空。";
        return false;
    }

    const std::string groupName = SanitizeFileName(std::string(name));
    const std::filesystem::path temporaryRoot =
        std::filesystem::u8path("cache/高德地图/gis数据/临时") /
        std::filesystem::u8path(queuedExport.taskId) /
        std::filesystem::u8path(groupName);
    std::error_code cleanupError;
    std::filesystem::remove_all(temporaryRoot, cleanupError);
    if (IsCancellationRequested())
    {
        return false;
    }

    DownTool::AmapVectorPngDownloadOptions options;
    options.vectorDownload.centerWgs84 = configuration.centerWgs84;
    options.vectorDownload.radiusMeters = configuration.radiusMeters;
    options.vectorDownload.zoom = configuration.zoom;
    options.vectorDownload.includeBaseLayers = true;
    options.vectorDownload.includeLabels =
        categories.contains(DownTool::AmapVectorRenderCategory::Point);
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
    options.render.categories = categories;
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

    const std::filesystem::path finalOutputRoot = GisOutputRoot(
        configuration, queuedExport.taskId, GisModeName(mode), groupName);
    const std::filesystem::path outputRoot = mode == GisExportMode::OverlayOnSatellite ?
        temporaryRoot / std::filesystem::u8path("叠加后处理") : finalOutputRoot;
    options.vectorDownload.geoJsonOutputPath.clear();
    const std::filesystem::path tileOutputPath = outputRoot / "XYZ";
    const std::filesystem::path postProcessPath = configuration.exportTiles ?
        tileOutputPath : temporaryRoot / std::filesystem::u8path("后处理");
    std::filesystem::path mergedImagePath = outputRoot / "merged";
    mergedImagePath.replace_extension("png");
    const std::filesystem::path fbxOutputPath = outputRoot / "FBX";

    DownTool::AmapVectorPngDownloader downloader;
    downloader.SetPostProcessOutputPath(PathToUtf8(postProcessPath));
    downloader.SetPostProcessEnabled(configuration.exportTiles);
    downloader.SetPostProcessMergedImageEnabled(configuration.exportMergedImage);
    downloader.SetPostProcessMergedImageOutputPath(PathToUtf8(mergedImagePath));
    downloader.SetPostProcessMergedImageResolution(configuration.mergedImageResolution);
    const bool exportFbx = configuration.exportFbx;
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
    if (IsCancellationRequested())
    {
        return false;
    }
    if (!result.success)
    {
        error = result.error;
        return false;
    }
    if (configuration.exportTiles &&
        !std::filesystem::exists(tileOutputPath / "meta.json"))
    {
        error = "GIS XYZ 瓦片未生成完整元数据：" + groupName;
        return false;
    }
    if (configuration.exportMergedImage && !std::filesystem::exists(mergedImagePath))
    {
        error = "GIS 合并大图未生成：" + groupName;
        return false;
    }
    if (exportFbx && !std::filesystem::exists(fbxOutputPath / "manifest.json"))
    {
        error = "GIS FBX 贴图清单未生成：" + groupName;
        return false;
    }

    if (mode == GisExportMode::OverlayOnSatellite)
    {
        const ExportCancellationCallback cancellationRequested =
            [this] { return IsCancellationRequested(); };
        for (const SatelliteExportSource& source : configuration.satelliteSources)
        {
            if (IsCancellationRequested())
            {
                return false;
            }
            const std::filesystem::path satelliteRoot = SatelliteOutputRoot(
                configuration, queuedExport.taskId, source);
            const std::filesystem::path overlayRoot = configuration.exportTiles ?
                tileOutputPath : temporaryRoot / "后处理";
            const std::filesystem::path compositeRoot = OverlayOutputRoot(
                configuration, queuedExport.taskId, source);
            if (!ExportImageComposer::CopyDirectory(
                    satelliteRoot, compositeRoot, error, cancellationRequested))
            {
                return false;
            }
            if ((configuration.exportTiles || configuration.exportFbx) &&
                !ExportImageComposer::CompositeXyzTileTree(
                    compositeRoot / "XYZ", overlayRoot,
                    compositeRoot / "XYZ", error, cancellationRequested))
            {
                return false;
            }
            if (configuration.exportFbx &&
                !ExportImageComposer::CompositeTileTree(
                    compositeRoot / "FBX", fbxOutputPath,
                    compositeRoot / "FBX", error, cancellationRequested))
            {
                return false;
            }
            if (configuration.exportMergedImage)
            {
                const std::filesystem::path satelliteImage = satelliteRoot /
                    std::filesystem::u8path(
                        "merged." + configuration.mergedImageFormat);
                const std::filesystem::path gisImage = mergedImagePath;
                const std::filesystem::path compositeImage = compositeRoot / "merged.png";
                if (!ExportImageComposer::CompositeImage(
                        satelliteImage, gisImage, compositeImage, error,
                        cancellationRequested))
                {
                    return false;
                }
                if (configuration.mergedImageFormat != "png")
                {
                    std::error_code removeError;
                    std::filesystem::remove(
                        compositeRoot /
                            std::filesystem::u8path(
                                "merged." + configuration.mergedImageFormat),
                        removeError);
                }
            }
        }
    }

    UpdateProgress(queuedExport.taskId, "GIS 导出完成 · " + groupName,
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunGeoJsonExport(
    const QueuedExport& queuedExport, size_t stepIndex, size_t stepCount,
    std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return false;
    }
    DownTool::AmapVectorDownloadOptions options;
    options.centerWgs84 = configuration.centerWgs84;
    options.radiusMeters = configuration.radiusMeters;
    options.zoom = configuration.zoom;
    options.includeBaseLayers = true;
    options.includeLabels = configuration.gisCategories.contains(
        DownTool::AmapVectorRenderCategory::Point);
    options.maxConcurrency = 0;
    options.cancellationRequested = [this] { return IsCancellationRequested(); };
    options.progressCallback =
        [this, taskId = queuedExport.taskId, stepIndex, stepCount](
            const DownTool::AmapVectorDownloadProgress& progress)
        {
            UpdateProgress(
                taskId, "下载 GIS GeoJSON",
                (static_cast<double>(stepIndex) +
                 ProgressRatio(progress.completedTaskCount, progress.totalTaskCount)) /
                    static_cast<double>(stepCount),
                progress.completedTaskCount, progress.totalTaskCount);
        };

    DownTool::AmapVectorDownloader downloader;
    const DownTool::AmapVectorDownloadResult result = downloader.Download(options);
    if (IsCancellationRequested())
    {
        return false;
    }
    if (!result.success)
    {
        error = result.error;
        return false;
    }
    const std::filesystem::path outputRoot = GisOutputRoot(
        configuration, queuedExport.taskId, "GeoJSON", "所选图层");
    const std::function<bool()> cancellationRequested =
        [this] { return IsCancellationRequested(); };
    const DownTool::AmapVectorDecodedData filteredData = FilterGisData(
        result.decodedData, configuration.gisCategories, cancellationRequested);
    if (IsCancellationRequested())
    {
        return false;
    }
    if (!DownTool::AmapVectorGeoJsonWriter::WriteFile(
            filteredData, outputRoot / "数据.geojson", error, cancellationRequested))
    {
        return false;
    }
    UpdateProgress(queuedExport.taskId, "GeoJSON 导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

void DomDownToolPreview::ExportTaskManager::UpdateProgress(
    std::string_view taskId, std::string stage, double progress,
    uint64_t completed, uint64_t total)
{
    bool notifyChanged = false;
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end() || record->state != ExportTaskState::Running)
        {
            return;
        }
        if (cancelActive_.load(std::memory_order_relaxed))
        {
            return;
        }
        const bool stageChanged = record->stage != stage;
        record->stage = std::move(stage);
        record->progress = std::max(record->progress, std::clamp(progress, 0., 1.));
        record->completed = completed;
        record->total = total;
        const auto now = std::chrono::steady_clock::now();
        const bool reachedEnd = total != 0 && completed >= total;
        notifyChanged = stageChanged || reachedEnd ||
            lastProgressNotificationTime_ == std::chrono::steady_clock::time_point{} ||
            now - lastProgressNotificationTime_ >= kProgressNotificationInterval;
        if (notifyChanged)
        {
            lastProgressNotificationTime_ = now;
        }
    }
    if (!notifyChanged)
    {
        return;
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
    const Json root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        return;
    }

    const auto tasks = root.find("tasks");
    if (tasks == root.end() || !tasks->is_array())
    {
        return;
    }
    bool recordsChanged = false;
    for (const Json& item : *tasks)
    {
        ExportTaskRecord record;
        record.id = GetJsonString(item, "id");
        record.title = GetJsonString(item, "title");
        record.detail = GetJsonString(item, "detail");
        record.outputPath = GetJsonString(item, "outputPath");
        record.createdAt = GetJsonString(item, "createdAt");
        record.finishedAt = GetJsonString(item, "finishedAt");
        record.state = ParseTaskState(GetJsonString(item, "state"));
        record.stage = GetJsonString(item, "stage");
        record.progress = GetJsonDouble(item, "progress");
        record.completed = static_cast<uint64_t>(
            std::max(0., GetJsonDouble(item, "completed")));
        record.total = static_cast<uint64_t>(
            std::max(0., GetJsonDouble(item, "total")));
        record.error = GetJsonString(item, "error");
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

    Json tasks = Json::array();
    for (const ExportTaskRecord& record : records)
    {
        tasks.push_back({
            {"id", record.id},
            {"title", record.title},
            {"detail", record.detail},
            {"outputPath", record.outputPath},
            {"createdAt", record.createdAt},
            {"finishedAt", record.finishedAt},
            {"state", TaskStateValue(record.state)},
            {"stage", record.stage},
            {"progress", record.progress},
            {"completed", static_cast<double>(record.completed)},
            {"total", static_cast<double>(record.total)},
            {"error", record.error}});
    }
    const Json root = {{"version", 1}, {"tasks", std::move(tasks)}};

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
        output << root.dump(2) << '\n';
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
