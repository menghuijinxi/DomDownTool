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
    std::string multidateYear;
    std::string multidateKey;
    std::string multidateGbCode;
    std::string multidateRegionName;
    // 登录态只用于当前进程内导出，不写入任务记录，避免保存一次性 cookie。
    std::string multidateCookie;
};

using ExportDownloaderFactory =
    std::function<std::unique_ptr<DownTool::DownToolBase>(std::string_view)>;

enum class ExportComponentKind
{
    MergedImage,
    StandardXyz,
    RawTiles,
    BasePlate,
    BuildingModel,
    GeoJson
};

enum class ExportSourceKind
{
    Imagery,
    Gis
};

struct ExportSourceEntry
{
    std::string id;
    std::string name;
    ExportSourceKind kind = ExportSourceKind::Imagery;
    std::filesystem::path cachePath;
    DownTool::AmapVectorRenderCategory gisCategory =
        DownTool::AmapVectorRenderCategory::Road;
    DownTool::AmapVectorColor color{};
    std::string multidateYear;
    std::string multidateKey;
    std::string multidateGbCode;
    std::string multidateRegionName;
    // 登录态只活在当前进程，任务记录不保存 cookie。
    std::string multidateCookie;
};

struct ExportComponent
{
    std::string id;
    ExportComponentKind kind = ExportComponentKind::MergedImage;
    std::vector<ExportSourceEntry> sources;
    std::string imageFormat = "png";
    uint32_t maxEdge = 16384;
    uint32_t xyzTileSize = 256;
    uint32_t textureSize = 4096;
    std::string textureFormat = "png";
    DownTool::FbxPlanetShape shape = DownTool::FbxPlanetShape::Sphere;
    bool exportElevation = false;
    double elevationSampleMeters = 30.;
    uint32_t meshSubdivision = 64;
    std::filesystem::path wallTexturePath = std::filesystem::u8path("贴图/墙体");
    std::filesystem::path roofTexturePath = std::filesystem::u8path("贴图/房顶");
    DownTool::AmapBuildingFbxTextureMode wallTextureMode =
        DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
    DownTool::AmapBuildingFbxRoofTextureMode roofTextureMode =
        DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
    DownTool::AmapBuildingFbxAtlasImageFormat atlasImageFormat =
        DownTool::AmapBuildingFbxAtlasImageFormat::Jpg;
    double layerHeightCentimeters = 45.;
    uint32_t maxWallMaterialsPerBucket = 1;
    uint32_t textureRandomSeed = 0;
    bool followBaseElevation = false;
    std::string basePlateComponentId;
    bool includeBaseLayers = true;
    bool includeLabels = false;
};

struct ExportContentGroup
{
    std::string id;
    std::string name;
    bool enabled = true;
    std::vector<ExportComponent> components;
};

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
    DownTool::AmapBuildingFbxTextureMode buildingFbxWallTextureMode =
        DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
    DownTool::AmapBuildingFbxRoofTextureMode buildingFbxRoofTextureMode =
        DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
    DownTool::AmapBuildingFbxAtlasImageFormat buildingFbxAtlasImageFormat =
        DownTool::AmapBuildingFbxAtlasImageFormat::Jpg;
    double buildingLayerHeightCentimeters = 45.;
    uint32_t buildingMaxWallMaterialsPerBucket = 1;
    uint32_t buildingTextureRandomSeed = 0;
    uint32_t mergedImageResolution = 16384;
    std::string mergedImageFormat = "png";
    uint32_t fbxTextureSize = 4096;
    std::string fbxTextureFormat = "png";
    double fbxTerrainSampleRateMeters = 30.;
    std::vector<ExportContentGroup> groups;
    // 下面几项只在一次导出执行期间改写输出位置，不写入任务记录。
    std::filesystem::path executionRootOverride;
    bool flatSourceOutput = false;
    std::optional<bool> includeBaseLayersOverride;
    std::optional<bool> includeLabelsOverride;
    std::filesystem::path geoJsonOutputFileOverride;
    std::filesystem::path buildingFbxOutputOverride;
    uint32_t executionTileSize = 0;
    uint32_t executionFbxSubdivision = 0;
    std::optional<DownTool::FbxPlanetShape> executionFbxShape;
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

// 分组配置不合法时返回错误；没有分组时返回空字符串，交给旧导出校验。
[[nodiscard]] std::string GroupedExportError(const ExportConfiguration& configuration);

class ExportGroupedRunner;

class ExportTaskManager
{
    friend class ExportGroupedRunner;

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

    struct SatelliteDownloadOptions
    {
        std::filesystem::path outputRoot;
        std::string mergedImageFormat;
        std::string fbxTextureFormat;
        std::string progressStagePrefix;
        double stepProgressStart = 0.;
        double stepProgressSpan = 1.;
        uint32_t maxDownloadQueue = 0;
        bool transparentMultidateEmptyTiles = false;
        bool forceMultidateLatestWmts = false;
        bool reportStepComplete = true;
        std::optional<bool> exportTilesOverride;
        std::optional<bool> exportMergedImageOverride;
        std::optional<bool> exportFbxOverride;
        std::optional<uint32_t> mergedImageResolutionOverride;
        std::optional<uint32_t> fbxTextureSizeOverride;
        std::optional<uint32_t> tileSizeOverride;
        std::optional<uint32_t> fbxSubdivisionOverride;
        std::optional<DownTool::FbxPlanetShape> fbxShapeOverride;
        std::filesystem::path xyzOutputPathOverride;
        std::filesystem::path mergedImageOutputPathOverride;
        std::filesystem::path fbxOutputPathOverride;
    };

    void WorkerLoop();

    bool RunExport(const QueuedExport& queuedExport, std::string& error);

    bool RunGroupedExport(const QueuedExport& queuedExport, std::string& error);

    bool RunSatelliteExport(const QueuedExport& queuedExport,
                            const SatelliteExportSource& source,
                            const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
                            size_t stepIndex, size_t stepCount,
                            std::string& error);

    bool RunSatelliteDownloaderToRoot(
        const QueuedExport& queuedExport,
        const SatelliteExportSource& source,
        const SatelliteDownloadOptions& options,
        const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
        size_t stepIndex, size_t stepCount,
        std::string& error);

    bool RunTianDiTuMultidateCompositeExport(
        const QueuedExport& queuedExport,
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
                              const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
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

class ExportGroupedRunner
{
  public:
    static bool Run(ExportTaskManager& manager,
                    const ExportTaskManager::QueuedExport& queuedExport,
                    std::string& error);
};
} // namespace DomDownToolPreview
