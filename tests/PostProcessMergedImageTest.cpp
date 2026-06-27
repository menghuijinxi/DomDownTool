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

        const std::filesystem::path metadataPath =
            postprocess_test::MakePathWithUtf8Suffix(tool.MergedImagePath(), ".meta.json");
        if (!postprocess_test::RequireImageSize(tool.MergedImagePath(), 64, 64) ||
            !postprocess_test::RequireNonEmptyFile(
                metadataPath))
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

    bool RunTest()
    {
        return RunMergedImageOnlyTest() && RunMergedImageFromPyramidTest();
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
