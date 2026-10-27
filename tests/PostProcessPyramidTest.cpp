#include "PostProcessTestSupport.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <thread>

#if defined(_WIN32)
#include "SlowTileHttpServer.h"
#endif

namespace
{
class RetryDownloadDown final : public DownTool::GoogleDownBase
{
  public:
    RetryDownloadDown(std::filesystem::path emptyResponse, std::filesystem::path validResponse,
                      uint32_t failedAttempts)
        : emptyResponse_(std::move(emptyResponse)), validResponse_(std::move(validResponse)),
          failedAttempts_(failedAttempts)
    {
    }

    void CalculateDownRange() override
    {
        MinTile = DownTool::Tile(0, 0, Zoom);
        MaxTile = DownTool::Tile(2, 0, Zoom);
    }

    std::string MakeUri(const DownTool::Tile& tile) override
    {
        const uint32_t attempt = ++requestCounts_[tile.X];
        const bool fail = tile.X == 1 && attempt <= failedAttempts_;
        return "file:///" + (fail ? emptyResponse_ : validResponse_).generic_string();
    }

    uint32_t GetRequestCount(int32_t x) const
    {
        const auto found = requestCounts_.find(x);
        return found == requestCounts_.end() ? 0 : found->second;
    }

  private:
    std::filesystem::path emptyResponse_;
    std::filesystem::path validResponse_;
    uint32_t failedAttempts_ = 0;
    std::map<int32_t, uint32_t> requestCounts_;
};

bool PrepareDownloadRetryTest(const std::filesystem::path& testRoot)
{
    if (!postprocess_test::ResetDirectory(testRoot))
    {
        return false;
    }
    std::ofstream(testRoot / "empty.bin", std::ios::binary).close();
    return postprocess_test::WriteSourceTile(testRoot / "valid.png", 32,
                                             cv::Scalar(23, 47, 89, 255));
}

void ConfigureDownloadRetryTool(DownTool::DownToolBase& tool, const std::filesystem::path& testRoot)
{
    tool.SetZoom(2);
    tool.SetTileSize(32);
    tool.SetCachePath(postprocess_test::PathToUtf8String(testRoot / "cache"));
    tool.SetPostProcessOutputPath(postprocess_test::PathToUtf8String(testRoot / "output"));
    tool.SetPostProcessEnabled(true);
    tool.SetMaxDownQueue(2);
}

bool RunDownloadRetryRecoveryTest(uint32_t failedAttempts)
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("download-retry-recovery-" + std::to_string(failedAttempts));
    if (!PrepareDownloadRetryTest(testRoot))
    {
        return false;
    }
    const std::filesystem::path emptyResponse = testRoot / "empty.bin";
    const std::filesystem::path validResponse = testRoot / "valid.png";

    RetryDownloadDown tool(emptyResponse, validResponse, failedAttempts);
    ConfigureDownloadRetryTool(tool, testRoot);
    const std::filesystem::path cachedTile = testRoot / "cache" / "2" / "0" / "0.png";
    if (!postprocess_test::WriteSourceTile(cachedTile, 32, cv::Scalar(91, 63, 17, 255)))
    {
        return false;
    }
    std::atomic_bool progressWithinTotal = true;
    tool.SetProgressCallback([&progressWithinTotal](const DownTool::DownToolProgress& progress) {
        if (progress.completed > progress.total)
        {
            progressWithinTotal.store(false);
        }
    });
    tool.StartDown();

    return postprocess_test::Require(tool.GetRequestCount(1) == failedAttempts + 1 &&
                                         tool.GetDownloadError().empty(),
                                     "首次返回空文件后没有在当前下载任务内重试失败瓦片") &&
           postprocess_test::Require(tool.GetRequestCount(0) == 0 && tool.GetRequestCount(2) == 1,
                                     "重试失败瓦片时重复下载了有效缓存或本轮已经成功的瓦片") &&
           postprocess_test::Require(progressWithinTotal.load(),
                                     "重试使下载完成数超过了目标总数") &&
           postprocess_test::RequireReadableImage(testRoot / "output" / "2" / "1" / "0.png") &&
           postprocess_test::RequireNonEmptyFile(testRoot / "output" / "meta.json");
}

bool RunDownloadRetryExhaustionTest()
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("download-retry-exhaustion");
    if (!PrepareDownloadRetryTest(testRoot))
    {
        return false;
    }
    RetryDownloadDown tool(testRoot / "empty.bin", testRoot / "valid.png", 4);
    ConfigureDownloadRetryTool(tool, testRoot);
    uint64_t finalFailedCount = 0;
    tool.SetProgressCallback([&finalFailedCount](const DownTool::DownToolProgress& progress) {
        if (progress.stage == "下载失败")
        {
            finalFailedCount = progress.failed;
        }
    });
    tool.StartDown();

    return postprocess_test::Require(tool.GetRequestCount(0) == 1 && tool.GetRequestCount(1) == 4 &&
                                         tool.GetRequestCount(2) == 1 && finalFailedCount == 1,
                                     "失败重试没有遵守三轮上限，或重复下载了成功瓦片") &&
           postprocess_test::Require(
               tool.GetDownloadError().find("仍有 1 个瓦片不可用") != std::string::npos &&
                   tool.GetDownloadError().find("z2 (1, 0)") != std::string::npos &&
                   !std::filesystem::exists(testRoot / "output" / "meta.json"),
               "重试耗尽后没有报告失败详情，或仍然执行了后处理");
}

bool RunDownloadRetryCancellationTest()
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("download-retry-cancellation");
    if (!PrepareDownloadRetryTest(testRoot))
    {
        return false;
    }
    RetryDownloadDown tool(testRoot / "empty.bin", testRoot / "valid.png", 4);
    ConfigureDownloadRetryTool(tool, testRoot);
    std::atomic_bool waitingForRetry = false;
    tool.SetProgressCallback([&waitingForRetry](const DownTool::DownToolProgress& progress) {
        if (progress.stage.find("等待重试") == 0)
        {
            waitingForRetry.store(true);
        }
    });
    std::thread worker([&tool] { tool.StartDown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!waitingForRetry.load() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto canceledAt = std::chrono::steady_clock::now();
    tool.StopDown();
    worker.join();

    return postprocess_test::Require(waitingForRetry.load() && tool.GetRequestCount(1) == 1 &&
                                         std::chrono::steady_clock::now() - canceledAt <
                                             std::chrono::milliseconds(500),
                                     "等待重试时取消没有及时退出，或取消后继续发起下载") &&
           postprocess_test::Require(
               tool.GetDownloadError().empty() &&
                   !std::filesystem::exists(testRoot / "output" / "meta.json"),
               "取消重试被误报为下载失败，或仍然执行了后处理");
}

#if defined(_WIN32)
class HttpRetryDownloadDown final : public DownTool::GoogleDownBase
{
  public:
    void CalculateDownRange() override
    {
        MinTile = DownTool::Tile(1, 0, Zoom);
        MaxTile = MinTile;
    }
};

bool RunIncompleteResponseRetryTest()
{
    const cv::Mat image(32, 32, CV_8UC4, cv::Scalar(23, 47, 89, 255));
    std::vector<unsigned char> encoded;
    if (!cv::imencode(".png", image, encoded))
    {
        return false;
    }
    tile_queue_test::SlowTileHttpServer server(encoded);
    for (const std::string route : {"retry-partial", "retry-http-error"})
    {
        const std::filesystem::path testRoot = postprocess_test::MakeTestRoot(route);
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }
        HttpRetryDownloadDown tool;
        ConfigureDownloadRetryTool(tool, testRoot);
        tool.SetProxy("");
        tool.SetUrlTemplate(server.GetUrl() + "/" + route);
        tool.StartDown();
        if (!postprocess_test::Require(tool.GetDownloadError().empty(),
                                       "HTTP 失败或传输中断没有在重试后恢复") ||
            !postprocess_test::RequireReadableImage(testRoot / "output" / "2" / "1" / "0.png") ||
            !postprocess_test::RequireNonEmptyFile(testRoot / "output" / "meta.json"))
        {
            return false;
        }
    }
    return postprocess_test::Require(server.GetPartialRequestCount() == 2 &&
                                         server.GetHttpErrorRequestCount() == 2,
                                     "带图片头的半截响应或 HTTP 错误图片被缓存检查误判为成功");
}
#endif

struct BlockingPostProcessControl
{
    std::atomic_uint32_t callCount = 0;
    std::atomic_bool started = false;
};

    class BlockingPostProcessDown final : public postprocess_test::TestPostProcessDown
    {
      public:
        explicit BlockingPostProcessDown(
            std::shared_ptr<BlockingPostProcessControl> control)
            : control_(std::move(control))
        {
        }

      protected:
        bool WritePostProcessMaxZoomTile(
            const DownTool::Tile&,
            const std::filesystem::path&) const override
        {
            control_->callCount.fetch_add(1, std::memory_order_relaxed);
            control_->started.store(true, std::memory_order_release);
            while (!bStop.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }

      private:
        std::shared_ptr<BlockingPostProcessControl> control_;
    };

    class UnavailableTileDown final : public DownTool::DownToolBase
    {
      public:
        explicit UnavailableTileDown(std::filesystem::path fixturePath)
            : fixturePath_(std::move(fixturePath))
        {
        }

        void CalculateDownRange() override
        {
            MinTile = DownTool::Tile(26447, 12163, Zoom);
            MaxTile = MinTile;
        }

        std::string MakeUri(const DownTool::Tile& tile) override
        {
            minimumRequestedZoom_ = std::min(minimumRequestedZoom_, tile.Zoom);
            return "file:///" + fixturePath_.generic_string();
        }

        uint32_t GetMinimumRequestedZoom() const
        {
            return minimumRequestedZoom_;
        }

      protected:
        bool IsUnavailableTileFile(
            const std::filesystem::path&, const DownTool::Tile&) const override
        {
            return true;
        }

      private:
        std::filesystem::path fixturePath_;
        uint32_t minimumRequestedZoom_ = std::numeric_limits<uint32_t>::max();
    };

    class FallbackCacheDown final : public DownTool::DownToolBase
    {
      public:
        FallbackCacheDown(
            std::filesystem::path unavailableFixturePath,
            std::filesystem::path parentFixturePath)
            : unavailableFixturePath_(std::move(unavailableFixturePath)),
              parentFixturePath_(std::move(parentFixturePath))
        {
        }

        void CalculateDownRange() override
        {
            MinTile = TargetTile();
            MaxTile = MinTile;
        }

        std::string MakeUri(const DownTool::Tile& tile) override
        {
            if (tile.Zoom == TargetTile().Zoom)
            {
                targetRequestCount_++;
            }
            const std::filesystem::path& fixturePath =
                tile.Zoom == ParentTile().Zoom ? parentFixturePath_ : unavailableFixturePath_;
            return "file:///" + fixturePath.generic_string();
        }

        uint32_t GetTargetRequestCount() const
        {
            return targetRequestCount_;
        }

        std::filesystem::path TargetTilePath() const
        {
            return GetTileFilePath(TargetTile());
        }

        std::filesystem::path FallbackTargetTilePath() const
        {
            return std::filesystem::u8path(CachePath) /
                "fallback" /
                std::to_string(TargetTile().Zoom) /
                std::to_string(TargetTile().X) /
                (std::to_string(TargetTile().Y) + ".png");
        }

        std::filesystem::path ParentTilePath() const
        {
            return GetTileFilePath(ParentTile());
        }

        std::filesystem::path PostProcessSourceTilePath() const
        {
            return GetPostProcessSourceTileFilePath(TargetTile());
        }

        static DownTool::Tile TargetTile()
        {
            return DownTool::Tile(2, 3, 2);
        }

        static DownTool::Tile ParentTile()
        {
            return DownTool::Tile(1, 1, 1);
        }

      protected:
        bool IsUnavailableTileFile(
            const std::filesystem::path&, const DownTool::Tile& tile) const override
        {
            return tile.Zoom == TargetTile().Zoom;
        }

        uint32_t GetMinFallbackZoom() const override
        {
            return ParentTile().Zoom;
        }

      private:
        std::filesystem::path unavailableFixturePath_;
        std::filesystem::path parentFixturePath_;
        uint32_t targetRequestCount_ = 0;
    };

    bool RunFallbackCacheIsolationTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("fallback-cache-isolation");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        const std::filesystem::path unavailableFixturePath = testRoot / "unavailable.png";
        const std::filesystem::path parentFixturePath = testRoot / "parent.png";
        if (!postprocess_test::WriteSourceTile(
                unavailableFixturePath, 32, cv::Scalar(23, 47, 89, 255)) ||
            !postprocess_test::WriteSourceTile(
                parentFixturePath, 32, cv::Scalar(91, 63, 17, 255)))
        {
            return false;
        }

        FallbackCacheDown tool(unavailableFixturePath, parentFixturePath);
        tool.SetZoom(FallbackCacheDown::TargetTile().Zoom);
        tool.SetTileSize(32);
        tool.SetCachePath(postprocess_test::PathToUtf8String(testRoot / "cache"));
        tool.SetMaxDownQueue(1);
        if (!postprocess_test::WriteSourceTile(
                tool.ParentTilePath(), tool.GetTileSize(), cv::Scalar(91, 63, 17, 255)))
        {
            return false;
        }
        tool.StartDown();

        std::error_code targetExistsError;
        const bool targetExists =
            std::filesystem::exists(tool.TargetTilePath(), targetExistsError);
        return postprocess_test::Require(
                   !targetExistsError && !targetExists,
                   "降级瓦片被写入了目标层级的标准缓存目录") &&
            postprocess_test::Require(
                tool.GetTargetRequestCount() == 1,
                "目标层级瓦片被已有的低级别缓存静默替代，未发起目标层级请求") &&
            postprocess_test::RequireReadableImage(tool.FallbackTargetTilePath()) &&
            postprocess_test::Require(
                tool.PostProcessSourceTilePath() == tool.FallbackTargetTilePath(),
                "导出阶段没有读取隔离的降级瓦片");
    }

    bool RunFallbackRepairDepthTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("fallback-repair-depth");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        const std::filesystem::path fixturePath = testRoot / "unavailable.png";
        if (!postprocess_test::WriteSourceTile(
                fixturePath, 32, cv::Scalar(23, 47, 89, 255)))
        {
            return false;
        }

        UnavailableTileDown tool(fixturePath);
        tool.SetZoom(15);
        tool.SetCachePath(postprocess_test::PathToUtf8String(testRoot / "cache"));
        tool.SetMaxDownQueue(1);
        tool.StartDown();

        return postprocess_test::Require(
            tool.GetMinimumRequestedZoom() == 9,
            "z15 不可用瓦片回退到了无法批量修复的过低层级");
    }

    bool RunCancellationTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("pyramid-cancellation");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        const auto control = std::make_shared<BlockingPostProcessControl>();
        BlockingPostProcessDown tool(control);
        tool.Configure(
            testRoot / "cache",
            testRoot / "output",
            4,
            32,
            DownTool::Tile(0, 0, 4),
            DownTool::Tile(9, 9, 4),
            DownTool::LonLat(0., 0.));
        tool.SetMaxDownQueue(1);
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(false);
        tool.SetPostProcessFbxEnabled(false);

        std::thread worker([&tool] { tool.RunPostProcessForTest(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!control->started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        tool.StopDown();
        worker.join();

        return postprocess_test::Require(
            control->started.load(std::memory_order_acquire) &&
                control->callCount.load(std::memory_order_relaxed) == 1,
            "post-process workers continued claiming tiles after cancellation");
    }

    bool RunTest()
    {
        if (!RunDownloadRetryRecoveryTest(1) || !RunDownloadRetryRecoveryTest(3) ||
            !RunDownloadRetryExhaustionTest() || !RunDownloadRetryCancellationTest())
        {
            return false;
        }
#if defined(_WIN32)
        if (!RunIncompleteResponseRetryTest())
        {
            return false;
        }
#endif

        if (!RunFallbackCacheIsolationTest())
        {
            return false;
        }

        if (!RunFallbackRepairDepthTest())
        {
            return false;
        }

        if (!RunCancellationTest())
        {
            return false;
        }

        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("pyramid");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        postprocess_test::ConfigureTool(tool, "pyramid");
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(false);
        tool.SetPostProcessFbxEnabled(false);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        const DownTool::Tile transparentTile(1, 1, tool.GetZoom());
        if (!postprocess_test::WriteSourceTile(
            tool.TilePath(transparentTile),
            tool.GetTileSize(),
            cv::Scalar(23, 47, 89, 64)))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        const std::filesystem::path opaqueTilePath =
            tool.PostProcessTilePath(DownTool::Tile(2, 2, tool.GetZoom()));
        const std::filesystem::path transparentTilePath =
            tool.PostProcessTilePath(transparentTile);
        if (!postprocess_test::Require(opaqueTilePath.extension() == ".png",
                "opaque XYZ tile extension is not .png") ||
            !postprocess_test::RequireJpegEncoding(opaqueTilePath) ||
            !postprocess_test::Require(transparentTilePath.extension() == ".png",
                "transparent XYZ tile extension is not .png") ||
            !postprocess_test::RequirePngEncoding(transparentTilePath) ||
            !postprocess_test::RequireReadableImage(opaqueTilePath) ||
            !postprocess_test::RequireReadableImage(transparentTilePath))
        {
            return false;
        }

        const int tileSize = static_cast<int>(tool.GetTileSize());
        for (int32_t x = tool.GetMinTile().X; x <= tool.GetMaxTile().X; x++)
        {
            for (int32_t y = tool.GetMinTile().Y; y <= tool.GetMaxTile().Y; y++)
            {
                if (!postprocess_test::RequireImageSize(
                    tool.PostProcessTilePath(DownTool::Tile(x, y, tool.GetZoom())),
                    tileSize,
                    tileSize))
                {
                    return false;
                }
            }
        }

        if (!postprocess_test::RequireImageSize(
            tool.PostProcessTilePath(DownTool::Tile(0, 0, 1)), tileSize, tileSize) ||
            !postprocess_test::RequirePngEncoding(
                tool.PostProcessTilePath(DownTool::Tile(0, 0, 1))) ||
            !postprocess_test::RequireImageSize(
                tool.PostProcessTilePath(DownTool::Tile(1, 1, 1)), tileSize, tileSize) ||
            !postprocess_test::RequireImageSize(
                tool.PostProcessTilePath(DownTool::Tile(0, 0, 0)), tileSize, tileSize) ||
            !postprocess_test::RequireNonEmptyFile(
                tool.PostProcessRootPath() / "meta.json") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(
                    tool.PostProcessRootPath() / "meta.json",
                    "\"proj\":3857"),
                "XYZ meta.json 没有声明 EPSG:3857") ||
            !postprocess_test::RequireNonEmptyFile(
                tool.PostProcessRootPath() / "tilemapresource.xml"))
        {
            return false;
        }

        const std::filesystem::path rootTilePath =
            tool.PostProcessTilePath(DownTool::Tile(0, 0, 0));
        const cv::Scalar staleColor(13, 17, 251, 255);
        if (!postprocess_test::WriteSourceTile(
            rootTilePath, tool.GetTileSize(), staleColor))
        {
            return false;
        }

        tool.RunPostProcessForTest();
        if (!postprocess_test::RequirePixelNotColor(
            rootTilePath, 1, 2, cv::Vec4b(13, 17, 251, 255)))
        {
            return false;
        }

        std::cout << "PostProcessPyramidTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
