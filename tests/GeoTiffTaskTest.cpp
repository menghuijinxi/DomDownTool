#include "PostProcessTestSupport.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "DownToolBase.h"
#include "GoogleDownBase.h"
#include "preview/ExportTaskManager.h"
#include "tiffio.h"

namespace
{
class ImageOutputDownloader final : public DownTool::DownToolBase
{
  public:
    void StartDown() override
    {
        std::filesystem::path outputPath;
        cv::Mat image;
        if (GetPostProcessMaxZoomOnlyEnabled())
        {
            const DownTool::Tile tile =
                DownTool::GoogleDownBase::LonLatToTile(
                    GetCenterLonLat(),
                    GetZoom());
            outputPath =
                std::filesystem::u8path(GetPostProcessOutputPath()) /
                std::to_string(tile.Zoom) /
                std::to_string(tile.X) /
                (std::to_string(tile.Y) + ".png");
            image = cv::Mat(
                256,
                256,
                CV_8UC4,
                cv::Scalar(10, 20, 30, 255));
        }
        else
        {
            outputPath =
                std::filesystem::u8path(
                    GetPostProcessMergedImageOutputPath());
            image = cv::Mat(
                2,
                2,
                CV_8UC4,
                cv::Scalar(10, 20, 30, 255));
        }
        std::error_code directoryError;
        std::filesystem::create_directories(
            outputPath.parent_path(),
            directoryError);
        if (directoryError)
        {
            return;
        }

        std::vector<unsigned char> encoded;
        if (!cv::imencode(".png", image, encoded))
        {
            return;
        }
        std::ofstream output(
            outputPath,
            std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<std::streamsize>(encoded.size()));
    }
};

const DomDownToolPreview::ExportTaskRecord* FindRecord(
    const std::vector<DomDownToolPreview::ExportTaskRecord>& records,
    const std::string& id)
{
    const auto iterator = std::ranges::find_if(
        records,
        [&id](const DomDownToolPreview::ExportTaskRecord& record)
        {
            return record.id == id;
        });
    return iterator == records.end() ? nullptr : &*iterator;
}

bool WaitForFinished(
    DomDownToolPreview::ExportTaskManager& manager,
    const std::string& taskId)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const std::vector<DomDownToolPreview::ExportTaskRecord> records =
            manager.GetRecords();
        const DomDownToolPreview::ExportTaskRecord* record =
            FindRecord(records, taskId);
        if (record != nullptr &&
            record->state != DomDownToolPreview::ExportTaskState::Queued &&
            record->state != DomDownToolPreview::ExportTaskState::Running)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool HasImageSize(
    const std::filesystem::path& path,
    const uint32_t expectedWidth,
    const uint32_t expectedHeight)
{
#if defined(_WIN32)
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(
        TIFFOpenW(path.c_str(), "r"),
        TIFFClose);
#else
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(
        TIFFOpen(path.string().c_str(), "r"),
        TIFFClose);
#endif
    uint32_t width = 0;
    uint32_t height = 0;
    return tiff != nullptr &&
        TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) == 1 &&
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) == 1 &&
        width == expectedWidth &&
        height == expectedHeight;
}

bool RunTest()
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("geotiff-task");
    if (!postprocess_test::ResetDirectory(testRoot))
    {
        return false;
    }

    DomDownToolPreview::ExportConfiguration configuration;
    configuration.centerWgs84 = {116.397389, 39.908722};
    configuration.radiusMeters = 1.;
    configuration.zoom = 15;
    configuration.outputRoot = testRoot / "output";

    DomDownToolPreview::ExportContentGroup group;
    group.id = "g1";
    group.name = "GeoTIFF";
    DomDownToolPreview::ExportComponent component;
    component.id = "c1";
    component.kind = DomDownToolPreview::ExportComponentKind::DomGeoTiff;
    component.maxEdge = 2;
    component.sources.push_back({
        "test.satellite",
        "测试卫星",
        DomDownToolPreview::ExportSourceKind::Imagery,
        testRoot / "cache"
    });
    group.components.push_back(component);
    configuration.groups.push_back(group);

    DomDownToolPreview::ExportTaskManager manager(
        {},
        testRoot / "records.json",
        [](std::string_view)
        {
            return std::make_unique<ImageOutputDownloader>();
        });
    const std::string taskId = manager.Enqueue(configuration);
    const bool finished = WaitForFinished(manager, taskId);
    const std::vector<DomDownToolPreview::ExportTaskRecord> records =
        manager.GetRecords();
    const DomDownToolPreview::ExportTaskRecord* record =
        FindRecord(records, taskId);
    const std::filesystem::path domPath =
        DomDownToolPreview::ExportTaskOutputRoot(configuration, taskId) /
        std::filesystem::u8path("GeoTIFF") /
        std::filesystem::u8path("DOM GeoTIFF") /
        std::filesystem::u8path("测试卫星.tif");

    return postprocess_test::Require(
               finished && record != nullptr &&
                   record->state ==
                       DomDownToolPreview::ExportTaskState::Succeeded,
               record == nullptr ?
                   "DOM GeoTIFF 任务没有生成记录" :
                   "DOM GeoTIFF 任务失败：" + record->error) &&
        postprocess_test::RequireNonEmptyFile(domPath) &&
        postprocess_test::Require(
            HasImageSize(domPath, 256, 256),
            "DOM GeoTIFF 不应被最大边参数缩小");
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
