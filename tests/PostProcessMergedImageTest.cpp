#include "PostProcessTestSupport.h"

namespace
{
    bool RunMergedImageBoundarySamplingTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("merged-image-boundary-sampling");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        tool.Configure(
            testRoot / "cache",
            testRoot / "output",
            2,
            32,
            DownTool::Tile(1, 1, 2),
            DownTool::Tile(2, 2, 2),
            DownTool::LonLat(45., 0.));
        tool.SetPostProcessEnabled(false);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(48);
        tool.SetPostProcessFbxEnabled(false);

        cv::Mat sourceCanvas(64, 64, CV_8UC4);
        for (int y = 0; y < sourceCanvas.rows; y++)
        {
            for (int x = 0; x < sourceCanvas.cols; x++)
            {
                const uint8_t checker = static_cast<uint8_t>(((x + y) % 2) * 255);
                sourceCanvas.at<cv::Vec4b>(y, x) =
                    cv::Vec4b(checker, static_cast<uint8_t>(x * 3),
                        static_cast<uint8_t>(y * 3), 255);
            }
        }

        for (int32_t tileX = 1; tileX <= 2; tileX++)
        {
            for (int32_t tileY = 1; tileY <= 2; tileY++)
            {
                const cv::Mat tile = sourceCanvas(cv::Rect(
                    (tileX - 1) * 32,
                    (tileY - 1) * 32,
                    32,
                    32));
                std::error_code createError;
                std::filesystem::create_directories(
                    tool.TilePath(DownTool::Tile(tileX, tileY, 2)).parent_path(),
                    createError);
                if (createError || !cv::imwrite(
                        postprocess_test::PathToUtf8String(
                            tool.TilePath(DownTool::Tile(tileX, tileY, 2))),
                        tile))
                {
                    return false;
                }
            }
        }

        tool.RunPostProcessForTest();
        const cv::Mat actual = cv::imread(
            postprocess_test::PathToUtf8String(tool.MergedImagePath()),
            cv::IMREAD_UNCHANGED);
        if (!postprocess_test::Require(
                !actual.empty() && actual.size() == cv::Size(48, 48),
                "合并大图边界采样测试输出尺寸错误"))
        {
            return false;
        }

        cv::Mat expected;
        cv::resize(sourceCanvas, expected, cv::Size(48, 48), 0., 0., cv::INTER_LINEAR);
        cv::Mat difference;
        cv::absdiff(actual, expected, difference);
        double maximumDifference = 0.;
        cv::minMaxLoc(difference.reshape(1), &maximumDifference);
        return postprocess_test::Require(
            maximumDifference <= 16.,
            fmt::format(
                "合并大图在瓦片边界使用了独立插值，最大误差 {}",
                maximumDifference));
    }

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
        return RunMergedImageBoundarySamplingTest() &&
            RunMergedImageOnlyTest() &&
            RunMergedImageFromPyramidTest() &&
            RunMergedImageSingleHighZoomTileTest() &&
            RunMergedImageJpegTest();
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
