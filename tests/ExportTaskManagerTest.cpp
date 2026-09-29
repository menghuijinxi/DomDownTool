#include "PostProcessTestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

#include "DownToolBase.h"
#include "preview/ExportTaskManager.h"
#include "preview/ExportImageComposer.h"

namespace
{
using Json = nlohmann::json;

Json loadJsonFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return input.is_open() ? Json::parse(input, nullptr, false)
                           : Json::parse(std::string{}, nullptr, false);
}

const DomDownToolPreview::ExportTaskRecord* findRecord(
    const std::vector<DomDownToolPreview::ExportTaskRecord>& records,
    const std::string& id)
{
    const auto record = std::find_if(
        records.begin(), records.end(),
        [&id](const DomDownToolPreview::ExportTaskRecord& item) { return item.id == id; });
    return record == records.end() ? nullptr : &*record;
}

bool equalPolygons(
    const DownTool::Wgs84Polygons& left,
    const DownTool::Wgs84Polygons& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (size_t ringIndex = 0; ringIndex < left.size(); ++ringIndex)
    {
        if (left[ringIndex].size() != right[ringIndex].size())
        {
            return false;
        }
        for (size_t coordinateIndex = 0;
             coordinateIndex < left[ringIndex].size();
             ++coordinateIndex)
        {
            const DownTool::LonLat& leftCoordinate =
                left[ringIndex][coordinateIndex];
            const DownTool::LonLat& rightCoordinate =
                right[ringIndex][coordinateIndex];
            if (leftCoordinate.lon != rightCoordinate.lon ||
                leftCoordinate.lat != rightCoordinate.lat)
            {
                return false;
            }
        }
    }
    return true;
}

bool waitForFinished(DomDownToolPreview::ExportTaskManager& manager,
                     const std::string& taskId)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::vector<DomDownToolPreview::ExportTaskRecord> records = manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record = findRecord(records, taskId);
        if (record && record->state != DomDownToolPreview::ExportTaskState::Queued &&
            record->state != DomDownToolPreview::ExportTaskState::Running)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

template<typename Predicate>
bool waitForCondition(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct BlockingDownloaderControl
{
    std::atomic_bool started = false;
    std::atomic_bool stopObserved = false;
    std::atomic_bool allowFinish = false;
};

class BlockingDownloader final : public DownTool::DownToolBase
{
  public:
    explicit BlockingDownloader(std::shared_ptr<BlockingDownloaderControl> control)
        : control_(std::move(control))
    {
    }

    void StartDown() override
    {
        control_->started.store(true, std::memory_order_release);
        while (!bStop.load(std::memory_order_relaxed) &&
               !control_->allowFinish.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (bStop.load(std::memory_order_relaxed))
        {
            ReportProgress("取消请求后的进度", 1, 10);
            control_->stopObserved.store(true, std::memory_order_release);
        }
        while (!control_->allowFinish.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

  private:
    std::shared_ptr<BlockingDownloaderControl> control_;
};

class CountingDownloader final : public DownTool::DownToolBase
{
  public:
    explicit CountingDownloader(std::shared_ptr<std::atomic_uint32_t> startCount)
        : startCount_(std::move(startCount))
    {
    }

    void StartDown() override
    {
        startCount_->fetch_add(1, std::memory_order_relaxed);
    }

  private:
    std::shared_ptr<std::atomic_uint32_t> startCount_;
};

class ProgressBurstDownloader final : public DownTool::DownToolBase
{
  public:
    void StartDown() override
    {
        constexpr uint64_t kProgressCount = 20000;
        for (uint64_t completed = 1; completed <= kProgressCount; ++completed)
        {
            ReportProgress("下载瓦片", completed, kProgressCount);
        }

        const std::filesystem::path outputRoot =
            std::filesystem::u8path(GetPostProcessOutputPath());
        std::error_code directoryError;
        std::filesystem::create_directories(outputRoot, directoryError);
        if (!directoryError)
        {
            std::ofstream(outputRoot / "meta.json", std::ios::binary) << "{}\n";
        }
    }
};

class ImageOutputDownloader final : public DownTool::DownToolBase
{
  public:
    explicit ImageOutputDownloader(cv::Scalar color) : color_(color)
    {
    }

    void StartDown() override
    {
        ReportProgress("下载瓦片", 1, 2);
        if (GetPostProcessEnabled())
        {
            const std::filesystem::path outputRoot =
                std::filesystem::u8path(GetPostProcessOutputPath());
            const std::filesystem::path tilePath = outputRoot / "15" / "1" / "2.png";
            writeImage(tilePath);
            std::ofstream(outputRoot / "meta.json", std::ios::binary) << "{}\n";
        }
        if (GetPostProcessMergedImageEnabled())
        {
            writeImage(std::filesystem::u8path(GetPostProcessMergedImageOutputPath()));
        }
        if (GetPostProcessFbxEnabled())
        {
            const std::filesystem::path fbxRoot = std::filesystem::u8path(GetFbxOutputPath());
            writeImage(fbxRoot / "0_0.png");
            std::ofstream(fbxRoot / "manifest.json", std::ios::binary) << "{}\n";
        }
        ReportProgress("合并大图", 1, 1);
    }

  private:
    void writeImage(const std::filesystem::path& path) const
    {
        std::error_code directoryError;
        std::filesystem::create_directories(path.parent_path(), directoryError);
        if (directoryError)
        {
            return;
        }

        const cv::Mat image(2, 2, CV_8UC4, color_);
        std::string extension = path.extension().string();
        if (extension.empty())
        {
            extension = ".png";
        }

        cv::Mat convertedImage;
        const cv::Mat* imageToEncode = &image;
        if (extension == ".jpg" || extension == ".jpeg")
        {
            cv::cvtColor(image, convertedImage, cv::COLOR_BGRA2BGR);
            imageToEncode = &convertedImage;
        }

        std::vector<unsigned char> encoded;
        if (!cv::imencode(extension, *imageToEncode, encoded))
        {
            return;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return;
        }
        output.write(
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
    }

    cv::Scalar color_;
};

DomDownToolPreview::ExportConfiguration makeCancelableConfiguration(
    const std::filesystem::path& outputRoot)
{
    DomDownToolPreview::ExportConfiguration configuration;
    configuration.radiusMeters = 1000.;
    configuration.outputRoot = outputRoot;
    configuration.exportTiles = true;
    configuration.fbxTextureFormat = "jpg";
    configuration.fbxTerrainSampleRateMeters = 60.;
    configuration.SetTerrainMaximumTriangles(8192);
    configuration.satelliteSources.push_back(
        {"test.satellite", "测试卫星", outputRoot / "cache"});
    return configuration;
}

bool verifyGroupedMergedImageWorkingFormat(const std::filesystem::path& testRoot)
{
    DomDownToolPreview::ExportComponent mixedMergedImage;
    mixedMergedImage.kind = DomDownToolPreview::ExportComponentKind::MergedImage;
    mixedMergedImage.imageFormat = "jpg";
    mixedMergedImage.sources.push_back({
        "test.satellite", "测试卫星",
        DomDownToolPreview::ExportSourceKind::Imagery,
        testRoot / "mixed-format-satellite-cache"});
    mixedMergedImage.sources.push_back({
        "test.gis", "道路",
        DomDownToolPreview::ExportSourceKind::Gis,
        testRoot / "mixed-format-gis-cache"});
    DomDownToolPreview::ExportComponent imageryOnlyMergedImage = mixedMergedImage;
    imageryOnlyMergedImage.sources.pop_back();
    return postprocess_test::Require(
               DomDownToolPreview::GroupedMergedImageWorkingFormat(
                   mixedMergedImage) == "png",
               "影像与 GIS 组合大图没有固定使用 PNG 中间格式") &&
        postprocess_test::Require(
               DomDownToolPreview::GroupedMergedImageWorkingFormat(
                   imageryOnlyMergedImage) == "jpg",
               "纯影像大图不应改写用户选择的最终格式");
}

bool verifyFinalJpegComposition(const std::filesystem::path& testRoot)
{
    const cv::Mat baseImage(32, 32, CV_8UC4, cv::Scalar(10, 20, 30, 255));
    const cv::Mat overlayImage(32, 32, CV_8UC4, cv::Scalar(110, 120, 130, 128));
    const std::filesystem::path baseImagePath = testRoot / "final-base.png";
    const std::filesystem::path overlayImagePath = testRoot / "final-overlay.png";
    const std::filesystem::path jpegPath = testRoot / "final-composed.jpg";
    std::string composeError;
    if (!postprocess_test::Require(
            cv::imwrite(
                postprocess_test::PathToUtf8String(baseImagePath),
                baseImage) &&
                cv::imwrite(
                    postprocess_test::PathToUtf8String(overlayImagePath),
                    overlayImage) &&
                DomDownToolPreview::ExportImageComposer::CompositeImage(
                    baseImagePath, overlayImagePath, jpegPath, composeError),
            "PNG 中间图无法生成用户选择的 JPG 最终大图：" + composeError) ||
        !postprocess_test::RequireJpegEncoding(jpegPath))
    {
        return false;
    }

    const cv::Mat jpegImage = cv::imread(
        postprocess_test::PathToUtf8String(jpegPath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec3b pixel = jpegImage.empty() ?
        cv::Vec3b(0, 0, 0) : jpegImage.at<cv::Vec3b>(16, 16);
    return postprocess_test::Require(
        jpegImage.channels() == 3 &&
            std::abs(static_cast<int>(pixel[0]) - 60) <= 3 &&
            std::abs(static_cast<int>(pixel[1]) - 70) <= 3 &&
            std::abs(static_cast<int>(pixel[2]) - 80) <= 3,
        "最终 JPG 没有保留 PNG 中间图的透明叠加结果");
}

bool runTest()
{
    const std::filesystem::path testRoot =
        std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "tmp" / "export-task-manager-tests";
    const std::filesystem::path recordsPath = testRoot / "records.json";
    std::error_code fileError;
    std::filesystem::remove_all(testRoot, fileError);
    std::filesystem::create_directories(testRoot, fileError);
    if (!postprocess_test::Require(!fileError, "无法准备导出任务测试目录"))
    {
        return false;
    }

    DomDownToolPreview::ExportConfiguration defaultOutputConfiguration;
    DomDownToolPreview::ExportComponent defaultBuildingComponent;
    defaultBuildingComponent.kind =
        DomDownToolPreview::ExportComponentKind::BuildingModel;
    DownTool::AmapVectorRenderOptions defaultRenderOptions;
    if (!postprocess_test::Require(
            !defaultOutputConfiguration.exportBuildingFbx &&
                !defaultRenderOptions.exportBuildingsToFbx,
            "建筑 FBX 不应在配置或渲染选项中默认启用") ||
        !postprocess_test::Require(
            defaultOutputConfiguration.buildingFbxWallTextureMode ==
                    DownTool::AmapBuildingFbxTextureMode::MergedAtlas &&
                defaultOutputConfiguration.buildingFbxRoofTextureMode ==
                    DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas &&
                defaultOutputConfiguration.buildingFbxAtlasImageFormat ==
                    DownTool::AmapBuildingFbxAtlasImageFormat::Jpg,
            "旧版建筑导出配置没有使用合并贴图和 JPG 默认值") ||
        !postprocess_test::Require(
            defaultBuildingComponent.wallTextureMode ==
                    DownTool::AmapBuildingFbxTextureMode::MergedAtlas &&
                defaultBuildingComponent.roofTextureMode ==
                    DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas &&
                defaultBuildingComponent.atlasImageFormat ==
                    DownTool::AmapBuildingFbxAtlasImageFormat::Jpg &&
                defaultBuildingComponent.shape ==
                    DownTool::FbxPlanetShape::Sphere,
            "新建建筑模型组件没有使用球面、合并贴图和 JPG 默认值"))
    {
        return false;
    }

    const std::filesystem::path buildingDefaultsRecordsPath =
        testRoot / "building-defaults-records.json";
    std::ofstream buildingDefaultsOutput(
        buildingDefaultsRecordsPath, std::ios::binary | std::ios::trunc);
    buildingDefaultsOutput << R"json({
  "version": 1,
  "tasks": [
    {
      "id": "building-missing-defaults",
      "title": "缺省建筑参数",
      "detail": "",
      "outputPath": "",
      "createdAt": "2026-09-24 09:00:00",
      "finishedAt": "2026-09-24 09:00:01",
      "state": "succeeded",
      "stage": "已完成",
      "progress": 1,
      "completed": 1,
      "total": 1,
      "error": "",
      "configuration": {
        "centerWgs84": {"lon": 116.397389, "lat": 39.908722},
        "groups": [{
          "id": "g1",
          "name": "默认值",
          "enabled": true,
          "components": [{"id": "c1", "kind": "building-model"}]
        }]
      }
    },
    {
      "id": "building-explicit-legacy",
      "title": "显式旧建筑参数",
      "detail": "",
      "outputPath": "",
      "createdAt": "2026-09-24 09:01:00",
      "finishedAt": "2026-09-24 09:01:01",
      "state": "succeeded",
      "stage": "已完成",
      "progress": 1,
      "completed": 1,
      "total": 1,
      "error": "",
      "configuration": {
        "centerWgs84": {"lon": 116.397389, "lat": 39.908722},
        "buildingFbxWallTextureMode": "scattered",
        "buildingFbxRoofTextureMode": "scattered",
        "buildingFbxAtlasImageFormat": "png",
        "groups": [{
          "id": "g2",
          "name": "旧值",
          "enabled": true,
          "components": [{
            "id": "c2",
            "kind": "building-model",
            "wallTextureMode": "scattered",
            "roofTextureMode": "scattered",
            "atlasImageFormat": "png"
          }]
        }]
      }
    }
  ]
})json";
    buildingDefaultsOutput.close();
    {
        DomDownToolPreview::ExportTaskManager manager(
            {}, buildingDefaultsRecordsPath);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* missingDefaults =
            findRecord(records, "building-missing-defaults");
        const DomDownToolPreview::ExportTaskRecord* explicitLegacy =
            findRecord(records, "building-explicit-legacy");
        const auto componentFrom = [](const DomDownToolPreview::ExportTaskRecord* record)
            -> const DomDownToolPreview::ExportComponent*
        {
            if (record == nullptr || !record->configuration.has_value() ||
                record->configuration->groups.empty() ||
                record->configuration->groups.front().components.empty())
            {
                return nullptr;
            }
            return &record->configuration->groups.front().components.front();
        };
        const DomDownToolPreview::ExportComponent* missingComponent =
            componentFrom(missingDefaults);
        const DomDownToolPreview::ExportComponent* explicitComponent =
            componentFrom(explicitLegacy);
        if (!postprocess_test::Require(
                missingDefaults && missingDefaults->configuration.has_value() &&
                    missingDefaults->configuration->buildingFbxWallTextureMode ==
                        DownTool::AmapBuildingFbxTextureMode::MergedAtlas &&
                    missingDefaults->configuration->buildingFbxRoofTextureMode ==
                        DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas &&
                    missingDefaults->configuration->buildingFbxAtlasImageFormat ==
                        DownTool::AmapBuildingFbxAtlasImageFormat::Jpg &&
                    missingComponent &&
                    missingComponent->wallTextureMode ==
                        DownTool::AmapBuildingFbxTextureMode::MergedAtlas &&
                    missingComponent->roofTextureMode ==
                        DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas &&
                    missingComponent->atlasImageFormat ==
                        DownTool::AmapBuildingFbxAtlasImageFormat::Jpg,
                "缺少建筑参数字段的记录没有采用新默认值") ||
            !postprocess_test::Require(
                explicitLegacy && explicitLegacy->configuration.has_value() &&
                    explicitLegacy->configuration->buildingFbxWallTextureMode ==
                        DownTool::AmapBuildingFbxTextureMode::Scattered &&
                    explicitLegacy->configuration->buildingFbxRoofTextureMode ==
                        DownTool::AmapBuildingFbxRoofTextureMode::Scattered &&
                    explicitLegacy->configuration->buildingFbxAtlasImageFormat ==
                        DownTool::AmapBuildingFbxAtlasImageFormat::Png &&
                    explicitComponent &&
                    explicitComponent->wallTextureMode ==
                        DownTool::AmapBuildingFbxTextureMode::Scattered &&
                    explicitComponent->roofTextureMode ==
                        DownTool::AmapBuildingFbxRoofTextureMode::Scattered &&
                    explicitComponent->atlasImageFormat ==
                        DownTool::AmapBuildingFbxAtlasImageFormat::Png,
                "显式保存的旧建筑参数没有保持原值"))
        {
            return false;
        }
    }

    DomDownToolPreview::ExportConfiguration xyzConfiguration;
    xyzConfiguration.centerWgs84 = {116.397389, 39.908722};
    xyzConfiguration.radiusMeters = 1000.;
    xyzConfiguration.zoom = 15;
    DomDownToolPreview::ExportContentGroup xyzGroup;
    xyzGroup.id = "g1";
    xyzGroup.name = "XYZ";
    DomDownToolPreview::ExportComponent xyzComponent;
    xyzComponent.id = "c1";
    xyzComponent.kind = DomDownToolPreview::ExportComponentKind::StandardXyz;
    xyzComponent.sources.push_back({
        "test.satellite", "测试卫星",
        DomDownToolPreview::ExportSourceKind::Imagery,
        testRoot / "xyz-validation-cache"});
    xyzGroup.components.push_back(std::move(xyzComponent));
    xyzConfiguration.groups.push_back(std::move(xyzGroup));
    for (const uint32_t tileSize : {256u, 512u, 1024u})
    {
        xyzConfiguration.groups.front().components.front().xyzTileSize = tileSize;
        if (!postprocess_test::Require(
                DomDownToolPreview::GroupedExportError(xyzConfiguration).empty(),
                "标准 XYZ 合法分辨率被拒绝：" + std::to_string(tileSize)))
        {
            return false;
        }
    }
    xyzConfiguration.groups.front().components.front().xyzTileSize = 300;
    if (!postprocess_test::Require(
            DomDownToolPreview::GroupedExportError(xyzConfiguration) ==
                "标准 XYZ 分辨率只支持 256、512 或 1024。",
            "标准 XYZ 没有拒绝未支持的分辨率"))
    {
        return false;
    }
    if (!verifyGroupedMergedImageWorkingFormat(testRoot))
    {
        return false;
    }
    for (const uint32_t tileSize : {512u, 1024u})
    {
        const std::filesystem::path xyzRoot =
            testRoot / ("xyz-" + std::to_string(tileSize));
        postprocess_test::TestPostProcessDown tool;
        const DownTool::Tile tile(1, 1, 2);
        tool.Configure(
            xyzRoot / "cache", xyzRoot / "output", tile.Zoom, tileSize,
            tile, tile, {0., 0.});
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(false);
        tool.SetPostProcessFbxEnabled(false);
        if (!postprocess_test::WriteSourceTile(
                tool.TilePath(tile), 256, cv::Scalar(10, 20, 30, 255)))
        {
            return false;
        }
        tool.RunPostProcessForTest();
        if (!postprocess_test::RequireImageSize(
                tool.PostProcessTilePath(tile),
                static_cast<int>(tileSize),
                static_cast<int>(tileSize)))
        {
            return false;
        }
    }

    DomDownToolPreview::ExportConfiguration selectedOutputConfiguration;
    selectedOutputConfiguration.outputRoot = testRoot / "selected-output";
    if (!postprocess_test::Require(
            DomDownToolPreview::ExportTaskOutputRoot(
                defaultOutputConfiguration, "task-id") ==
                std::filesystem::u8path("cache/导出任务/task-id") &&
                DomDownToolPreview::ExportTaskOutputRoot(
                    selectedOutputConfiguration, "task-id") ==
                selectedOutputConfiguration.outputRoot /
                    std::filesystem::u8path("导出任务/task-id"),
            "默认输出目录和用户选择输出目录没有使用统一的任务结构"))
    {
        return false;
    }

    const std::filesystem::path gisTemporaryRoot =
        testRoot / std::filesystem::u8path("GIS 临时目录");
    const std::filesystem::path gisTileOutputRoot = gisTemporaryRoot / "XYZ";
    if (!postprocess_test::Require(
            DomDownToolPreview::ExportGisOverlayInputRoot(
                false, gisTileOutputRoot, gisTemporaryRoot) ==
                gisTemporaryRoot / std::filesystem::u8path("后处理") &&
                DomDownToolPreview::ExportGisOverlayInputRoot(
                    true, gisTileOutputRoot, gisTemporaryRoot) == gisTileOutputRoot,
            "GIS 叠加输入目录没有正确兼容中文后处理路径"))
    {
        return false;
    }

    const std::filesystem::path imageRoot =
        testRoot / std::filesystem::u8path("中文图像目录");
    const std::filesystem::path baseImagePath =
        imageRoot / std::filesystem::u8path("卫星底图.png");
    const std::filesystem::path overlayImagePath =
        imageRoot / std::filesystem::u8path("GIS叠加层.png");
    const std::filesystem::path composedImagePath =
        imageRoot / std::filesystem::u8path("合成结果.png");
    std::filesystem::create_directories(baseImagePath.parent_path(), fileError);
    cv::Mat baseImage(2, 2, CV_8UC4, cv::Scalar(10, 20, 30, 255));
    cv::Mat overlayImage(2, 2, CV_8UC4, cv::Scalar(110, 120, 130, 128));
    baseImage.at<cv::Vec4b>(0, 1) = cv::Vec4b(0, 0, 0, 0);
    const std::filesystem::path asciiBaseImagePath = testRoot / "base.png";
    const std::filesystem::path asciiOverlayImagePath = testRoot / "overlay.png";
    if (!postprocess_test::Require(
            cv::imwrite(
                postprocess_test::PathToUtf8String(asciiBaseImagePath), baseImage) &&
                cv::imwrite(
                    postprocess_test::PathToUtf8String(asciiOverlayImagePath), overlayImage),
            "无法准备 GIS alpha 合成测试图"))
    {
        return false;
    }
    std::filesystem::copy_file(asciiBaseImagePath, baseImagePath, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(asciiOverlayImagePath, overlayImagePath, fileError);
    }
    std::string composeError;
    if (!postprocess_test::Require(
            !fileError &&
                DomDownToolPreview::ExportImageComposer::CompositeImage(
                    baseImagePath, overlayImagePath, composedImagePath, composeError),
            "无法生成 GIS alpha 合成测试图：" + composeError))
    {
        return false;
    }
    const std::filesystem::path asciiComposedImagePath =
        testRoot / "composed.png";
    std::filesystem::copy_file(
        composedImagePath,
        asciiComposedImagePath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    const cv::Mat composedImage = cv::imread(
        postprocess_test::PathToUtf8String(asciiComposedImagePath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec4b composedPixel = composedImage.at<cv::Vec4b>(0, 0);
    const cv::Vec4b transparentBasePixel = composedImage.at<cv::Vec4b>(0, 1);
    if (!postprocess_test::Require(
            composedImage.channels() == 4 && composedPixel[0] == 60 &&
                composedPixel[1] == 70 && composedPixel[2] == 80 &&
                composedPixel[3] == 255 && transparentBasePixel[0] == 110 &&
                transparentBasePixel[1] == 120 && transparentBasePixel[2] == 130 &&
                transparentBasePixel[3] == 128,
            "GIS alpha 合成结果不符合标准 source-over 公式"))
    {
        return false;
    }
    if (!verifyFinalJpegComposition(testRoot))
    {
        return false;
    }

    const std::filesystem::path canceledComposedImagePath =
        imageRoot / std::filesystem::u8path("取消合成.png");
    composeError.clear();
    if (!postprocess_test::Require(
            !DomDownToolPreview::ExportImageComposer::CompositeImage(
                baseImagePath,
                overlayImagePath,
                canceledComposedImagePath,
                composeError,
                [] { return true; }) &&
                !std::filesystem::exists(canceledComposedImagePath) &&
                composeError == "导出任务已取消。",
            "GIS 图像合成没有响应取消请求"))
    {
        return false;
    }

    const std::filesystem::path tileRelativePath =
        std::filesystem::path("14") / "1" / "2.png";
    const std::filesystem::path baseTileRoot =
        testRoot / std::filesystem::u8path("瓦片树/卫星XYZ");
    const std::filesystem::path overlayTileRoot =
        testRoot / std::filesystem::u8path("瓦片树/GIS图层");
    const std::filesystem::path outputTileRoot =
        testRoot / std::filesystem::u8path("瓦片树/叠加结果");
    const std::filesystem::path asciiBaseTilePath = testRoot / "base-tile.jpg";
    const cv::Mat baseTile(2, 2, CV_8UC3, cv::Scalar(10, 20, 30));
    if (!postprocess_test::Require(
            cv::imwrite(
                postprocess_test::PathToUtf8String(asciiBaseTilePath), baseTile),
            "无法准备 JPEG 卫星瓦片"))
    {
        return false;
    }
    std::filesystem::create_directories(
        (baseTileRoot / tileRelativePath).parent_path(), fileError);
    std::filesystem::create_directories(
        (overlayTileRoot / tileRelativePath).parent_path(), fileError);
    std::filesystem::copy_file(
        asciiBaseTilePath, baseTileRoot / tileRelativePath, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(
            asciiOverlayImagePath, overlayTileRoot / tileRelativePath, fileError);
    }
    composeError.clear();
    if (!postprocess_test::Require(
            !fileError && DomDownToolPreview::ExportImageComposer::CopyDirectory(
                baseTileRoot, outputTileRoot, composeError) &&
                DomDownToolPreview::ExportImageComposer::CompositeXyzTileTree(
                    outputTileRoot, overlayTileRoot, outputTileRoot, composeError),
            "中文 XYZ 目录无法叠加 GIS 图层：" + composeError))
    {
        return false;
    }
    const std::filesystem::path asciiComposedTilePath =
        testRoot / "composed-tile.png";
    std::filesystem::copy_file(
        outputTileRoot / tileRelativePath,
        asciiComposedTilePath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    const cv::Mat composedTile = cv::imread(
        postprocess_test::PathToUtf8String(asciiComposedTilePath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec3b composedTilePixel = composedTile.at<cv::Vec3b>(0, 0);
    if (!postprocess_test::Require(
            !fileError &&
                postprocess_test::RequireJpegEncoding(
                    outputTileRoot / tileRelativePath) &&
                composedTile.channels() == 3 &&
                composedTilePixel[0] == 60 && composedTilePixel[1] == 70 &&
                composedTilePixel[2] == 80,
            "XYZ 卫星瓦片与 GIS 图层没有正确合成"))
    {
        return false;
    }

    const std::filesystem::path fbxTextureRelativePath = "0_0.png";
    const std::filesystem::path baseFbxRoot =
        testRoot / std::filesystem::u8path("FBX贴图/卫星图");
    const std::filesystem::path overlayFbxRoot =
        testRoot / std::filesystem::u8path("FBX贴图/GIS图层");
    std::filesystem::create_directories(baseFbxRoot, fileError);
    std::filesystem::create_directories(overlayFbxRoot, fileError);
    std::filesystem::copy_file(
        asciiBaseImagePath, baseFbxRoot / fbxTextureRelativePath, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(
            asciiOverlayImagePath,
            overlayFbxRoot / fbxTextureRelativePath,
            fileError);
    }
    composeError.clear();
    if (!postprocess_test::Require(
            !fileError && DomDownToolPreview::ExportImageComposer::CompositeTileTree(
                baseFbxRoot, overlayFbxRoot, baseFbxRoot, composeError),
            "中文 FBX 贴图目录无法叠加 GIS 图层：" + composeError))
    {
        return false;
    }
    const std::filesystem::path asciiComposedFbxTexturePath =
        testRoot / "composed-fbx-texture.png";
    std::filesystem::copy_file(
        baseFbxRoot / fbxTextureRelativePath,
        asciiComposedFbxTexturePath,
        std::filesystem::copy_options::overwrite_existing,
        fileError);
    const cv::Mat composedFbxTexture = cv::imread(
        postprocess_test::PathToUtf8String(asciiComposedFbxTexturePath),
        cv::IMREAD_UNCHANGED);
    const cv::Vec4b composedFbxPixel = composedFbxTexture.at<cv::Vec4b>(0, 0);
    if (!postprocess_test::Require(
            !fileError &&
                postprocess_test::RequirePngEncoding(
                    baseFbxRoot / fbxTextureRelativePath) &&
                composedFbxTexture.channels() == 4 && composedFbxPixel[0] == 60 &&
                composedFbxPixel[1] == 70 && composedFbxPixel[2] == 80 &&
                composedFbxPixel[3] == 255,
            "FBX 卫星贴图与 GIS 图层没有正确合成"))
    {
        return false;
    }

    const std::filesystem::path fbxJpgTextureRelativePath = "0_0.jpg";
    const std::filesystem::path baseJpgFbxRoot =
        testRoot / std::filesystem::u8path("FBX贴图/卫星图JPG");
    const std::filesystem::path overlayJpgFbxRoot =
        testRoot / std::filesystem::u8path("FBX贴图/GIS图层JPG");
    std::filesystem::create_directories(baseJpgFbxRoot, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(
            asciiBaseTilePath,
            baseJpgFbxRoot / fbxJpgTextureRelativePath,
            std::filesystem::copy_options::overwrite_existing,
            fileError);
    }
    std::filesystem::create_directories(overlayJpgFbxRoot, fileError);
    if (!fileError)
    {
        std::filesystem::copy_file(
            asciiOverlayImagePath,
            overlayJpgFbxRoot / fbxTextureRelativePath,
            std::filesystem::copy_options::overwrite_existing,
            fileError);
    }
    if (!postprocess_test::Require(!fileError, "无法准备 JPG FBX 叠加图层"))
    {
        return false;
    }
    composeError.clear();
    if (!postprocess_test::Require(
            DomDownToolPreview::ExportImageComposer::CompositeTileTree(
                baseJpgFbxRoot, overlayJpgFbxRoot, baseJpgFbxRoot, composeError),
            "JPG FBX 卫星贴图无法叠加 GIS PNG 图层：" + composeError) ||
        !postprocess_test::RequireJpegEncoding(
            baseJpgFbxRoot / fbxJpgTextureRelativePath))
    {
        return false;
    }

    std::ofstream output(recordsPath, std::ios::binary | std::ios::trunc);
    output << R"json({
  "version": 1,
  "tasks": [
    {"id":"queued","title":"排队任务","detail":"GIS XYZ","outputPath":"gis",
     "createdAt":"2026-08-10 10:00:00","finishedAt":"","state":"queued",
     "stage":"等待执行","progress":0,"completed":0,"total":10,"error":""},
    {"id":"running","title":"运行任务","detail":"卫星大图","outputPath":"satellite",
     "createdAt":"2026-08-10 10:01:00","finishedAt":"","state":"running",
     "stage":"下载瓦片","progress":0.5,"completed":5,"total":10,"error":""},
    {"id":"done","title":"完成任务","detail":"GIS 大图","outputPath":"gis",
     "createdAt":"2026-08-10 09:00:00","finishedAt":"2026-08-10 09:01:00",
     "state":"succeeded","stage":"已完成","progress":1,"completed":1,"total":1,
     "error":""}
  ]
})json";
    output.close();

    std::atomic_uint32_t changeCount = 0;
    {
        DomDownToolPreview::ExportTaskManager manager(
            [&changeCount] { changeCount.fetch_add(1, std::memory_order_relaxed); },
            recordsPath);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records = manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* queued = findRecord(records, "queued");
        const DomDownToolPreview::ExportTaskRecord* running = findRecord(records, "running");
        const DomDownToolPreview::ExportTaskRecord* done = findRecord(records, "done");
        if (!postprocess_test::Require(
                queued && queued->state == DomDownToolPreview::ExportTaskState::Failed &&
                    queued->stage == "已中断" && !queued->finishedAt.empty(),
                "重启后排队任务没有标记为已中断") ||
            !postprocess_test::Require(
                running && running->state == DomDownToolPreview::ExportTaskState::Failed &&
                    running->stage == "已中断" && !running->finishedAt.empty(),
                "重启后运行任务没有标记为已中断") ||
            !postprocess_test::Require(
                done && done->state == DomDownToolPreview::ExportTaskState::Succeeded,
                "已完成任务的历史状态被修改"))
        {
            return false;
        }

        const Json persistedRoot = loadJsonFile(recordsPath);
        if (!postprocess_test::Require(
                !persistedRoot.is_discarded() && persistedRoot.is_object(),
                "无法读取立即写回的导出任务记录"))
        {
            return false;
        }
        const auto persistedTasks = persistedRoot.find("tasks");
        if (!postprocess_test::Require(
                persistedTasks != persistedRoot.end() && persistedTasks->is_array() &&
                    persistedTasks->size() == 3 &&
                    (*persistedTasks)[0].value("state", "") == "failed" &&
                    (*persistedTasks)[1].value("state", "") == "failed",
                "中断状态没有立即持久化"))
        {
            return false;
        }

        manager.ClearFinished();
        if (!postprocess_test::Require(manager.GetRecords().empty(),
                                       "清除已结束记录没有移除历史任务"))
        {
            return false;
        }

        DomDownToolPreview::ExportConfiguration invalidConfiguration;
        invalidConfiguration.radiusMeters = 1000.;
        invalidConfiguration.outputRoot = testRoot / "invalid-output";
        invalidConfiguration.satelliteSources.push_back({
            "gaode.satellite", "高德卫星", testRoot / "satellite-cache"});
        const std::string invalidTaskId = manager.Enqueue(invalidConfiguration);
        if (!postprocess_test::Require(waitForFinished(manager, invalidTaskId),
                                       "无效导出任务没有结束"))
        {
            return false;
        }
        const std::vector<DomDownToolPreview::ExportTaskRecord> invalidRecords =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* invalidRecord =
            findRecord(invalidRecords, invalidTaskId);
        if (!postprocess_test::Require(
                invalidRecord &&
                    invalidRecord->state == DomDownToolPreview::ExportTaskState::Failed &&
                    invalidRecord->error == "没有选择任何导出内容。",
                "工作线程没有记录导出失败原因"))
        {
            return false;
        }
        const std::filesystem::path conversionRecord =
            invalidConfiguration.outputRoot / std::filesystem::u8path("导出任务") /
            std::filesystem::u8path(invalidTaskId) /
            std::filesystem::u8path("转换记录-" + invalidTaskId + ".txt");
        if (!postprocess_test::Require(
                postprocess_test::ContainsText(conversionRecord, "状态: 失败") &&
                    postprocess_test::ContainsText(
                        conversionRecord, "结果: 没有选择任何导出内容。") &&
                    postprocess_test::ContainsText(conversionRecord, "半径 (米): 1000") &&
                    postprocess_test::ContainsText(conversionRecord, "XYZ 目录:") &&
                    postprocess_test::ContainsText(conversionRecord, "/XYZ") &&
                    postprocess_test::ContainsText(conversionRecord, "FBX 目录:") &&
                    postprocess_test::ContainsText(conversionRecord, "/FBX") &&
                    postprocess_test::ContainsText(conversionRecord, "FBX 导出高度: 否") &&
                    postprocess_test::ContainsText(
                        conversionRecord, "FBX 高度采样率 (米): 30"),
                "输出目录没有保存可复用的任务转换记录"))
        {
            return false;
        }

        if (!postprocess_test::Require(
                manager.RemoveFinished(invalidTaskId) &&
                    findRecord(manager.GetRecords(), invalidTaskId) == nullptr &&
                    std::filesystem::exists(conversionRecord),
                "移除单个任务记录时不应删除导出文件"))
        {
            return false;
        }

        const std::string deleteTaskId = manager.Enqueue(invalidConfiguration);
        if (!postprocess_test::Require(waitForFinished(manager, deleteTaskId),
                                       "永久删除测试任务没有结束"))
        {
            return false;
        }
        const std::filesystem::path deleteTaskOutput =
            DomDownToolPreview::ExportTaskOutputRoot(invalidConfiguration, deleteTaskId);
        std::string deleteError;
        if (!postprocess_test::Require(
                std::filesystem::exists(deleteTaskOutput) &&
                    manager.DeleteFinishedOutput(deleteTaskId, deleteError) &&
                    !std::filesystem::exists(deleteTaskOutput) &&
                    findRecord(manager.GetRecords(), deleteTaskId) == nullptr,
                "永久删除没有同时删除任务目录和任务记录：" + deleteError))
        {
            return false;
        }
    }

    const std::filesystem::path groupedRecordsPath =
        testRoot / "grouped-records.json";
    {
        DomDownToolPreview::ExportTaskManager manager(
            {}, groupedRecordsPath,
            [](std::string_view)
            {
                return std::make_unique<ImageOutputDownloader>(
                    cv::Scalar(10, 20, 30, 255));
            });
        DomDownToolPreview::ExportConfiguration configuration;
        configuration.centerWgs84 = {116.397389, 39.908722};
        configuration.radiusMeters = 1000.;
        configuration.zoom = 15;
        configuration.outputRoot = testRoot / "grouped-output";

        DomDownToolPreview::ExportContentGroup group;
        group.id = "g1";
        group.name = "卫星底图";
        DomDownToolPreview::ExportComponent component;
        component.id = "c1";
        component.kind = DomDownToolPreview::ExportComponentKind::MergedImage;
        component.imageFormat = "png";
        component.maxEdge = 8192;
        component.sources.push_back({
            "test.satellite.first", "测试卫星",
            DomDownToolPreview::ExportSourceKind::Imagery,
            testRoot / "grouped-cache-first"});
        component.sources.push_back({
            "test.satellite.second", "测试卫星",
            DomDownToolPreview::ExportSourceKind::Imagery,
            testRoot / "grouped-cache-second"});
        component.SetTerrainMaximumTriangles(4096);
        group.components.push_back(std::move(component));
        configuration.groups.push_back(std::move(group));

        const std::string taskId = manager.Enqueue(configuration);
        const bool finished = waitForFinished(manager, taskId);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record =
            findRecord(records, taskId);
        const std::filesystem::path componentRoot =
            DomDownToolPreview::ExportTaskOutputRoot(configuration, taskId) /
            std::filesystem::u8path("卫星底图") /
            std::filesystem::u8path("大图");
        if (!postprocess_test::Require(
                finished && record &&
                    record->state == DomDownToolPreview::ExportTaskState::Succeeded &&
                    record->configuration.has_value() &&
                    record->configuration->groups.size() == 1 &&
                    record->configuration->groups.front().components.size() == 1 &&
                    record->configuration->groups.front().components.front().maxEdge == 8192 &&
                    record->configuration->groups.front().components.front().
                        GetTerrainMaximumTriangles() == 4096 &&
                    std::filesystem::exists(
                        componentRoot / std::filesystem::u8path("测试卫星.png")) &&
                    std::filesystem::exists(
                        componentRoot / std::filesystem::u8path("测试卫星 (2).png")),
                "分组大图没有使用稳定的同名图源输出路径，或组件参数没有写入任务记录"))
        {
            return false;
        }
    }
    {
        DomDownToolPreview::ExportTaskManager manager({}, groupedRecordsPath);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const std::optional<DomDownToolPreview::ExportConfiguration> configuration =
            records.size() == 1 ? records.front().configuration : std::nullopt;
        if (!postprocess_test::Require(
                configuration.has_value() && configuration->groups.size() == 1 &&
                    configuration->groups.front().name == "卫星底图" &&
                    configuration->groups.front().components.size() == 1 &&
                    configuration->groups.front().components.front().sources.size() == 2 &&
                    configuration->groups.front().components.front().maxEdge == 8192 &&
                    configuration->groups.front().components.front().
                        GetTerrainMaximumTriangles() == 4096,
                "重启后没有恢复内容组、组件和图源参数"))
        {
            return false;
        }
    }

    {
        const auto control = std::make_shared<BlockingDownloaderControl>();
        DomDownToolPreview::ExportTaskManager manager(
            {}, testRoot / "cancel-records.json",
            [control](std::string_view)
            {
                return std::make_unique<BlockingDownloader>(control);
            });
        const DomDownToolPreview::ExportConfiguration configuration =
            makeCancelableConfiguration(testRoot / "cancel-output");
        const std::string activeTaskId = manager.Enqueue(configuration);
        const bool activeStarted = waitForCondition(
            [&control] { return control->started.load(std::memory_order_acquire); });

        const std::string queuedTaskId = manager.Enqueue(configuration);
        const bool queuedCanceled = manager.Cancel(queuedTaskId);
        const std::vector<DomDownToolPreview::ExportTaskRecord> queuedRecords =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* queuedRecord =
            findRecord(queuedRecords, queuedTaskId);
        const bool queuedStateIsCanceled = queuedRecord &&
            queuedRecord->state == DomDownToolPreview::ExportTaskState::Canceled;

        const bool activeCanceled = manager.Cancel(activeTaskId);
        const bool stopObserved = waitForCondition(
            [&control] { return control->stopObserved.load(std::memory_order_acquire); });
        const std::vector<DomDownToolPreview::ExportTaskRecord> cancelingRecords =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* cancelingRecord =
            findRecord(cancelingRecords, activeTaskId);
        const bool cancelingStateIsVisible = cancelingRecord &&
            cancelingRecord->state == DomDownToolPreview::ExportTaskState::Running &&
            cancelingRecord->stage == "正在取消";

        control->allowFinish.store(true, std::memory_order_release);
        const bool activeFinished = waitForFinished(manager, activeTaskId);
        const std::vector<DomDownToolPreview::ExportTaskRecord> canceledRecords =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* canceledRecord =
            findRecord(canceledRecords, activeTaskId);
        const bool activeStateIsCanceled = canceledRecord &&
            canceledRecord->state == DomDownToolPreview::ExportTaskState::Canceled;
        if (!postprocess_test::Require(
                activeStarted && queuedCanceled && queuedStateIsCanceled &&
                    activeCanceled && stopObserved && cancelingStateIsVisible &&
                    activeFinished && activeStateIsCanceled,
                "导出任务取消没有保持正在取消状态并最终结束"))
        {
            return false;
        }
    }

    {
        const auto factoryEntered = std::make_shared<std::atomic_bool>(false);
        const auto allowFactoryReturn = std::make_shared<std::atomic_bool>(false);
        const auto downloaderStartCount = std::make_shared<std::atomic_uint32_t>(0);
        DomDownToolPreview::ExportTaskManager manager(
            {}, testRoot / "cancel-before-downloader-records.json",
            [factoryEntered, allowFactoryReturn, downloaderStartCount](std::string_view)
            {
                factoryEntered->store(true, std::memory_order_release);
                while (!allowFactoryReturn->load(std::memory_order_acquire))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return std::make_unique<CountingDownloader>(downloaderStartCount);
            });
        const DomDownToolPreview::ExportConfiguration configuration =
            makeCancelableConfiguration(testRoot / "cancel-before-downloader-output");
        const std::string taskId = manager.Enqueue(configuration);
        const bool enteredFactory = waitForCondition(
            [&factoryEntered]
            {
                return factoryEntered->load(std::memory_order_acquire);
            });
        const bool canceled = manager.Cancel(taskId);
        allowFactoryReturn->store(true, std::memory_order_release);
        const bool finished = waitForFinished(manager, taskId);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record =
            findRecord(records, taskId);
        if (!postprocess_test::Require(
                enteredFactory && canceled && finished && record &&
                    record->state == DomDownToolPreview::ExportTaskState::Canceled &&
                    downloaderStartCount->load(std::memory_order_relaxed) == 0,
                "下载器注册前取消后仍启动了卫星下载"))
        {
            return false;
        }
    }

    {
        std::atomic_uint32_t burstChangeCount = 0;
        DomDownToolPreview::ExportTaskManager manager(
            [&burstChangeCount]
            {
                burstChangeCount.fetch_add(1, std::memory_order_relaxed);
            },
            testRoot / "progress-burst-records.json",
            [](std::string_view)
            {
                return std::make_unique<ProgressBurstDownloader>();
            });
        DomDownToolPreview::ExportConfiguration configuration =
            makeCancelableConfiguration(testRoot / "progress-burst-output");
        configuration.rangeMode =
            DomDownToolPreview::ExportRangeMode::KmlPolygon;
        configuration.centerWgs84 = {116.15, 39.95};
        configuration.polygonsWgs84 = {{
            {116.1, 39.9},
            {116.2, 39.9},
            {116.2, 40.0},
            {116.1, 40.0}
        }};
        configuration.kmlFileName = "persisted-range.kml";
        const std::string taskId = manager.Enqueue(configuration);
        const bool finished = waitForFinished(manager, taskId);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record = findRecord(records, taskId);
        const Json persisted = loadJsonFile(testRoot / "progress-burst-records.json");
        bool persistedHasConfiguration = false;
        if (persisted.is_object())
        {
            const auto persistedTasks = persisted.find("tasks");
            if (persistedTasks != persisted.end() && persistedTasks->is_array())
            {
                const auto persistedTask = std::find_if(
                    persistedTasks->begin(), persistedTasks->end(),
                    [&taskId](const Json& item) { return item.value("id", "") == taskId; });
                persistedHasConfiguration = persistedTask != persistedTasks->end() &&
                    persistedTask->contains("configuration");
            }
        }
        if (!postprocess_test::Require(
                finished && record &&
                    record->state == DomDownToolPreview::ExportTaskState::Succeeded &&
                    record->configuration.has_value() &&
                    record->configuration->zoom == configuration.zoom &&
                    record->configuration->exportTiles == configuration.exportTiles &&
                    record->configuration->rangeMode ==
                        DomDownToolPreview::ExportRangeMode::KmlPolygon &&
                    equalPolygons(
                        record->configuration->polygonsWgs84,
                        configuration.polygonsWgs84) &&
                    record->configuration->kmlFileName ==
                        configuration.kmlFileName &&
                    !record->configuration->exportBuildingFbx &&
                    record->configuration->fbxTextureFormat == "jpg" &&
                    record->configuration->fbxTerrainSampleRateMeters == 60. &&
                    record->configuration->GetTerrainMaximumTriangles() == 8192 &&
                    burstChangeCount.load(std::memory_order_relaxed) <= 10 &&
                    persistedHasConfiguration,
                "任务记录没有保存可恢复的导出参数或高频进度触发了过量界面刷新"))
        {
            return false;
        }
    }
    {
        DomDownToolPreview::ExportTaskManager manager(
            {}, testRoot / "progress-burst-records.json");
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const std::optional<DomDownToolPreview::ExportConfiguration> configuration =
            records.size() == 1 ? records.front().configuration : std::nullopt;
        if (!postprocess_test::Require(
                configuration.has_value() && configuration->radiusMeters == 1000. &&
                    configuration->rangeMode ==
                        DomDownToolPreview::ExportRangeMode::KmlPolygon &&
                    configuration->polygonsWgs84.size() == 1 &&
                    configuration->polygonsWgs84.front().size() == 4 &&
                    configuration->kmlFileName == "persisted-range.kml" &&
                    configuration->zoom == 15 && configuration->exportTiles &&
                    !configuration->exportBuildingFbx &&
                    configuration->fbxTextureFormat == "jpg" &&
                    configuration->fbxTerrainSampleRateMeters == 60. &&
                    configuration->GetTerrainMaximumTriangles() == 8192 &&
                    configuration->outputRoot == testRoot / "progress-burst-output" &&
                    configuration->satelliteSources.size() == 1 &&
                    configuration->satelliteSources.front().id == "test.satellite",
                "重启后没有完整恢复任务导出参数"))
        {
            return false;
        }
    }

    {
        const std::filesystem::path multidateRecordsPath =
            testRoot / "multidate-records.json";
        const auto multidateDownloaderIndex = std::make_shared<std::atomic_uint32_t>(0);
        DomDownToolPreview::ExportTaskManager manager(
            {}, multidateRecordsPath,
            [multidateDownloaderIndex](std::string_view)
            {
                const uint32_t index = multidateDownloaderIndex->fetch_add(
                    1, std::memory_order_relaxed);
                const bool historyOverlay = index % 2 == 1;
                return std::make_unique<ImageOutputDownloader>(historyOverlay ?
                    cv::Scalar(110, 120, 130, 128) :
                    cv::Scalar(10, 20, 30, 255));
            });
        DomDownToolPreview::ExportConfiguration configuration =
            makeCancelableConfiguration(testRoot / "multidate-output");
        configuration.exportMergedImage = true;
        configuration.mergedImageFormat = "png";
        configuration.satelliteSources.clear();
        DomDownToolPreview::SatelliteExportSource source{
            "tianditu.multidate", "天地图多时相 2024", testRoot / "multidate-cache"};
        source.multidateYear = "2024";
        source.multidateKey = "key-2024";
        source.multidateGbCode = "156110000";
        source.multidateRegionName = "北京";
        source.multidateCookie = "session-cookie";
        configuration.satelliteSources.push_back(source);
        DomDownToolPreview::SatelliteExportSource secondSource{
            "tianditu.multidate", "天地图多时相 2023", testRoot / "multidate-cache"};
        secondSource.multidateYear = "2023";
        secondSource.multidateKey = "key-2023";
        secondSource.multidateGbCode = "156110000";
        secondSource.multidateRegionName = "北京";
        secondSource.multidateCookie = "session-cookie";
        configuration.satelliteSources.push_back(secondSource);
        const std::string taskId = manager.Enqueue(configuration);
        const bool finished = waitForFinished(manager, taskId);
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record = findRecord(records, taskId);
        const Json persisted = loadJsonFile(multidateRecordsPath);
        bool persistedHasMultidate = false;
        bool persistedDroppedCookie = false;
        if (persisted.is_object())
        {
            const auto tasks = persisted.find("tasks");
            if (tasks != persisted.end() && tasks->is_array() && !tasks->empty())
            {
                const Json& savedTask = tasks->front();
                const auto savedConfiguration = savedTask.find("configuration");
                const Json* savedSources = nullptr;
                if (savedConfiguration != savedTask.end() && savedConfiguration->is_object())
                {
                    const auto sources = savedConfiguration->find("satelliteSources");
                    if (sources != savedConfiguration->end())
                    {
                        savedSources = &*sources;
                    }
                }
                if (savedSources && savedSources->is_array() && savedSources->size() == 2)
                {
                    const Json& savedSource = (*savedSources)[0];
                    const Json& secondSavedSource = (*savedSources)[1];
                    persistedHasMultidate =
                        savedSource.value("multidateYear", "") == "2024" &&
                        savedSource.value("multidateKey", "") == "key-2024" &&
                        savedSource.value("multidateGbCode", "") == "156110000" &&
                        savedSource.value("multidateRegionName", "") == "北京" &&
                        secondSavedSource.value("multidateYear", "") == "2023" &&
                        secondSavedSource.value("multidateKey", "") == "key-2023" &&
                        secondSavedSource.value("multidateGbCode", "") == "156110000" &&
                        secondSavedSource.value("multidateRegionName", "") == "北京";
                    persistedDroppedCookie = !savedSource.contains("multidateCookie") &&
                        !secondSavedSource.contains("multidateCookie");
                }
            }
        }
        const std::filesystem::path conversionRecord =
            configuration.outputRoot / std::filesystem::u8path("导出任务") /
            std::filesystem::u8path(taskId) /
            std::filesystem::u8path("转换记录-" + taskId + ".txt");
        const std::filesystem::path firstMergedImage =
            configuration.outputRoot / std::filesystem::u8path("导出任务") /
            std::filesystem::u8path(taskId) / std::filesystem::u8path("卫星图") /
            std::filesystem::u8path("天地图多时相 2024") / "merged.png";
        const std::filesystem::path asciiMultidateMergedImage =
            testRoot / "multidate-composed.png";
        std::filesystem::copy_file(
            firstMergedImage,
            asciiMultidateMergedImage,
            std::filesystem::copy_options::overwrite_existing,
            fileError);
        const cv::Mat composedMultidateImage = cv::imread(
            postprocess_test::PathToUtf8String(asciiMultidateMergedImage),
            cv::IMREAD_UNCHANGED);
        const cv::Vec4b composedMultidatePixel = composedMultidateImage.empty() ?
            cv::Vec4b(0, 0, 0, 0) : composedMultidateImage.at<cv::Vec4b>(0, 0);
        if (!postprocess_test::Require(
                finished && record &&
                    record->state == DomDownToolPreview::ExportTaskState::Succeeded &&
                    persistedHasMultidate && persistedDroppedCookie &&
                    !fileError &&
                    multidateDownloaderIndex->load(std::memory_order_relaxed) == 4 &&
                    composedMultidatePixel[0] == 60 && composedMultidatePixel[1] == 70 &&
                    composedMultidatePixel[2] == 80 && composedMultidatePixel[3] == 255 &&
                    postprocess_test::ContainsText(conversionRecord, "多时相年份: 2024") &&
                    postprocess_test::ContainsText(conversionRecord, "多时相年份: 2023") &&
                    postprocess_test::ContainsText(conversionRecord, "多时相区域: 北京") &&
                    !postprocess_test::ContainsText(conversionRecord, "session-cookie"),
                "天地图多时相导出参数没有正确持久化，或泄露了一次性登录 cookie"))
        {
            return false;
        }
    }

    {
        DomDownToolPreview::ExportTaskManager manager(
            {}, testRoot / "multidate-records.json");
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const std::optional<DomDownToolPreview::ExportConfiguration> configuration =
            records.size() == 1 ? records.front().configuration : std::nullopt;
        if (!postprocess_test::Require(
                configuration.has_value() && configuration->satelliteSources.size() == 2 &&
                    configuration->satelliteSources[0].id == "tianditu.multidate" &&
                    configuration->satelliteSources[0].multidateYear == "2024" &&
                    configuration->satelliteSources[0].multidateKey == "key-2024" &&
                    configuration->satelliteSources[0].multidateGbCode == "156110000" &&
                    configuration->satelliteSources[0].multidateRegionName == "北京" &&
                    configuration->satelliteSources[0].multidateCookie.empty() &&
                    configuration->satelliteSources[1].id == "tianditu.multidate" &&
                    configuration->satelliteSources[1].multidateYear == "2023" &&
                    configuration->satelliteSources[1].multidateKey == "key-2023" &&
                    configuration->satelliteSources[1].multidateGbCode == "156110000" &&
                    configuration->satelliteSources[1].multidateRegionName == "北京" &&
                    configuration->satelliteSources[1].multidateCookie.empty(),
                "重启后没有恢复天地图多时相导出参数，或恢复了不该持久化的 cookie"))
        {
            return false;
        }
    }

    const std::filesystem::path protectedOutput = testRoot / "protected-output";
    std::filesystem::create_directories(protectedOutput, fileError);
    std::ofstream(protectedOutput / "keep.txt") << "keep";
    const std::filesystem::path unsafeRecordsPath = testRoot / "unsafe-records.json";
    std::ofstream unsafeRecords(unsafeRecordsPath, std::ios::binary | std::ios::trunc);
    unsafeRecords << "{\"version\":1,\"tasks\":[{\"id\":\"unsafe\","
                     "\"title\":\"异常路径\",\"detail\":\"test\",\"outputPath\":\""
                  << protectedOutput.generic_string()
                  << "\",\"createdAt\":\"2026-08-10 10:00:00\",\"finishedAt\":"
                     "\"2026-08-10 10:01:00\",\"state\":\"succeeded\","
                     "\"stage\":\"已完成\",\"progress\":1,\"completed\":1,"
                     "\"total\":1,\"error\":\"\"}]}";
    unsafeRecords.close();
    {
        DomDownToolPreview::ExportTaskManager manager({}, unsafeRecordsPath);
        std::string deleteError;
        if (!postprocess_test::Require(
                !manager.DeleteFinishedOutput("unsafe", deleteError) &&
                    std::filesystem::exists(protectedOutput / "keep.txt") &&
                    findRecord(manager.GetRecords(), "unsafe") != nullptr &&
                    !deleteError.empty(),
                "永久删除没有拒绝异常任务路径"))
        {
            return false;
        }
    }

    const std::filesystem::path concurrentRecordsPath =
        testRoot / "concurrent-records.json";
    {
        DomDownToolPreview::ExportTaskManager manager(
            [&changeCount] { changeCount.fetch_add(1, std::memory_order_relaxed); },
            concurrentRecordsPath);
        DomDownToolPreview::ExportConfiguration invalidConfiguration;
        invalidConfiguration.radiusMeters = 1000.;
        invalidConfiguration.outputRoot = testRoot / "concurrent-output";

        std::mutex taskIdsMutex;
        std::vector<std::string> taskIds;
        std::vector<std::thread> producers;
        for (size_t producerIndex = 0; producerIndex < 4; ++producerIndex)
        {
            producers.emplace_back(
                [&manager, &invalidConfiguration, &taskIdsMutex, &taskIds]
                {
                    for (size_t taskIndex = 0; taskIndex < 6; ++taskIndex)
                    {
                        const std::string taskId = manager.Enqueue(invalidConfiguration);
                        const std::lock_guard lock(taskIdsMutex);
                        taskIds.push_back(taskId);
                    }
                });
        }
        for (std::thread& producer : producers)
        {
            producer.join();
        }
        for (const std::string& taskId : taskIds)
        {
            if (!postprocess_test::Require(
                    waitForFinished(manager, taskId),
                    "并发写入时导出任务没有结束"))
            {
                return false;
            }
        }

        Json concurrentRoot;
        const bool concurrentRecordsPersisted = waitForCondition(
            [&]
            {
                concurrentRoot = loadJsonFile(concurrentRecordsPath);
                const auto tasks = concurrentRoot.is_object()
                    ? concurrentRoot.find("tasks")
                    : concurrentRoot.end();
                return !concurrentRoot.is_discarded() && concurrentRoot.is_object() &&
                    tasks != concurrentRoot.end() && tasks->is_array() &&
                    tasks->size() == taskIds.size();
            });
        const auto concurrentTasks = concurrentRoot.is_object()
            ? concurrentRoot.find("tasks")
            : concurrentRoot.end();
        if (!postprocess_test::Require(
                concurrentRecordsPersisted && !concurrentRoot.is_discarded() &&
                    concurrentRoot.is_object(),
                "并发更新后导出任务记录不是有效 JSON") ||
            !postprocess_test::Require(
                concurrentTasks != concurrentRoot.end() && concurrentTasks->is_array() &&
                    concurrentTasks->size() == taskIds.size(),
                "并发更新后导出任务记录数量不完整"))
        {
            return false;
        }
    }

    return postprocess_test::Require(
        changeCount.load(std::memory_order_relaxed) >= 3,
        "导出任务状态变化没有通知界面");
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(runTest);
}
