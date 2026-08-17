#include "PostProcessTestSupport.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

namespace
{
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
