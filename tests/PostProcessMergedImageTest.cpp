#include "PostProcessTestSupport.h"

namespace
{
    bool RunMergedImageOnlyTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("merged-image");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        postprocess_test::ConfigureTool(tool, "merged-image");
        tool.SetPostProcessEnabled(false);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(64);
        tool.SetPostProcessFbxEnabled(false);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        if (!postprocess_test::RequireImageSize(tool.MergedImagePath(), 64, 64))
        {
            return false;
        }

        const cv::Scalar staleColor(13, 17, 251, 255);
        if (!postprocess_test::WriteSourceTile(
            tool.MergedImagePath(), 64, staleColor))
        {
            return false;
        }

        tool.RunPostProcessForTest();
        if (!postprocess_test::RequirePixelNotColor(
            tool.MergedImagePath(), 1, 2, cv::Vec4b(13, 17, 251, 255)))
        {
            return false;
        }

        std::cout << "PostProcessMergedImageTest passed\n";
        return true;
    }

    bool RunMergedImageFromPyramidTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("merged-image-from-pyramid");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestGcj02PostProcessDown tool;
        const uint32_t zoom = 16;
        const DownTool::LonLat centerLonLat(116.468067, 39.912247);
        const DownTool::Tile centerTile =
            DownTool::GoogleDownBase::LonLatToTile(centerLonLat, zoom);
        tool.Configure(
            testRoot / "cache",
            testRoot / "output",
            zoom,
            32,
            centerTile,
            centerTile,
            centerLonLat);
        tool.SetMaxDownQueue(4);
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(512);
        tool.SetPostProcessFbxEnabled(false);

        if (!postprocess_test::WriteOffsetSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        return postprocess_test::RequireImageSize(tool.MergedImagePath(), 32, 32);
    }

    bool RunMergedImageSingleHighZoomTileTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("merged-image-single-high-zoom-tile");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        const uint32_t zoom = 16;
        const DownTool::Tile tile(43123, 19765, zoom);
        tool.Configure(
            testRoot / "cache",
            testRoot / "output",
            zoom,
            32,
            tile,
            tile,
            DownTool::LonLat(116.41, 39.92));
        tool.SetMaxDownQueue(2);
        tool.SetPostProcessEnabled(false);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(32);
        tool.SetPostProcessFbxEnabled(false);

        if (!postprocess_test::WriteSourceTile(
            tool.TilePath(tile),
            tool.GetTileSize(),
            cv::Scalar(31, 97, 181, 255)))
        {
            return false;
        }

        tool.RunPostProcessForTest();
        return postprocess_test::RequireImageSize(tool.MergedImagePath(), 32, 32);
    }

    bool RunMergedImageJpegTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("merged-image-jpeg");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        postprocess_test::ConfigureTool(tool, "merged-image-jpeg");
        tool.SetPostProcessEnabled(false);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(64);
        tool.SetPostProcessMergedImageOutputPath(
            postprocess_test::PathToUtf8String(testRoot / "output" / "merged.jpg"));
        tool.SetPostProcessFbxEnabled(false);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();
        const cv::Mat image = cv::imread(
            postprocess_test::PathToUtf8String(tool.MergedImagePath()),
            cv::IMREAD_UNCHANGED);
        return postprocess_test::Require(!image.empty(), "JPEG 合并大图无法读取") &&
            postprocess_test::Require(
                image.cols == 64 && image.rows == 64,
                "JPEG 合并大图尺寸错误") &&
            postprocess_test::Require(image.channels() == 3, "JPEG 合并大图不是三通道图像");
    }

    bool RunTest()
    {
        return RunMergedImageOnlyTest() &&
            RunMergedImageFromPyramidTest() &&
            RunMergedImageSingleHighZoomTileTest() &&
            RunMergedImageJpegTest();
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
