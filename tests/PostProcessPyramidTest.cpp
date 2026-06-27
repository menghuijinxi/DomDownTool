#include "PostProcessTestSupport.h"

namespace
{
    bool RunTest()
    {
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

        tool.RunPostProcessForTest();

        const int tileSize = static_cast<int>(tool.GetTileSize());
        for (uint32_t x = tool.GetMinTile().X; x <= tool.GetMaxTile().X; x++)
        {
            for (uint32_t y = tool.GetMinTile().Y; y <= tool.GetMaxTile().Y; y++)
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

        std::cout << "PostProcessPyramidTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
