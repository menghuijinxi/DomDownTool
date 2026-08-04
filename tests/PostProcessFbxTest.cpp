#include "PostProcessTestSupport.h"

namespace
{
    std::filesystem::path FindFirstPng(const std::filesystem::path& directory)
    {
        std::error_code iteratorError;
        for (std::filesystem::directory_iterator iterator(directory, iteratorError), end;
            !iteratorError && iterator != end;
            iterator.increment(iteratorError))
        {
            if (iterator->is_regular_file() && iterator->path().extension() == ".png")
            {
                return iterator->path();
            }
        }
        return {};
    }

    bool RunTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("fbx");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        postprocess_test::ConfigureTool(tool, "fbx");
        tool.SetPostProcessEnabled(false);
        tool.SetPostProcessMergedImageEnabled(false);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxTextureSize(64);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
        tool.SetFbxMeshSubdivision(2);
        tool.SetMaxDownQueue(2);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        const std::filesystem::path textureRoot =
            tool.FbxRootPath() / "textures" / "fbx" / "64";
        const std::filesystem::path planeFbxPath =
            tool.FbxRootPath() / "mesh" / "plane" / "tiles.fbx";
        const std::filesystem::path sphereFbxPath =
            tool.FbxRootPath() / "mesh" / "sphere" / "tiles.fbx";

        if (!postprocess_test::RequireFileExists(textureRoot / "manifest.json") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(textureRoot / "manifest.json", "textures"),
                "manifest.json does not list textures") ||
            !postprocess_test::Require(
                postprocess_test::HasAnyPng(textureRoot),
                "FBX texture directory has no png output") ||
            !postprocess_test::RequireNonEmptyFile(planeFbxPath) ||
            !postprocess_test::RequireNonEmptyFile(sphereFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(planeFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(sphereFbxPath) ||
            !postprocess_test::RequirePlaneFbxUvMatchesNorthAxis(planeFbxPath))
        {
            return false;
        }

        const std::filesystem::path texturePath = FindFirstPng(textureRoot);
        const cv::Scalar staleColor(13, 17, 251, 255);
        if (!postprocess_test::Require(!texturePath.empty(), "FBX texture path is empty") ||
            !postprocess_test::WriteSourceTile(texturePath, 64, staleColor))
        {
            return false;
        }

        tool.RunPostProcessForTest();
        if (!postprocess_test::RequirePixelNotColor(
            texturePath, 1, 2, cv::Vec4b(13, 17, 251, 255)))
        {
            return false;
        }

        std::cout << "PostProcessFbxTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
