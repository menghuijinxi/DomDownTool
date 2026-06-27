#include "PostProcessTestSupport.h"

namespace
{
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

        std::cout << "PostProcessFbxTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
