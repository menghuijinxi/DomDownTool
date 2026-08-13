#include "PostProcessTestSupport.h"

namespace
{
    bool RequireFbxTexturesBesideModel(
        const std::filesystem::path& fbxPath,
        const std::filesystem::path& fbxRoot)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!postprocess_test::Require(scene != nullptr, "FBX scene is null"))
        {
            return false;
        }

        bool foundTexture = false;
        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials;
             materialIndex++)
        {
            aiString texturePath;
            if (scene->mMaterials[materialIndex]->GetTexture(
                    aiTextureType_DIFFUSE, 0, &texturePath) != AI_SUCCESS)
            {
                continue;
            }

            foundTexture = true;
            const std::filesystem::path relativeTexturePath =
                std::filesystem::u8path(texturePath.C_Str());
            if (!postprocess_test::Require(
                    !relativeTexturePath.has_parent_path(),
                    std::string("FBX texture path is not flat: ") +
                        texturePath.C_Str()) ||
                !postprocess_test::RequireFileExists(fbxRoot / relativeTexturePath))
            {
                return false;
            }
        }

        return postprocess_test::Require(foundTexture, "FBX has no diffuse texture");
    }

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
        const std::filesystem::path outputRoot = testRoot / "output";
        const std::filesystem::path xyzRoot = outputRoot / "XYZ";
        const std::filesystem::path fbxRoot = outputRoot / "FBX";
        tool.Configure(
            testRoot / "cache",
            xyzRoot,
            2,
            32,
            DownTool::Tile(1, 1, 2),
            DownTool::Tile(2, 2, 2),
            DownTool::LonLat(45., 0.));
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(64);
        tool.SetPostProcessMergedImageOutputPath(
            postprocess_test::PathToUtf8String(outputRoot / "merged.png"));
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(fbxRoot));
        tool.SetFbxTextureSize(64);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
        tool.SetFbxMeshSubdivision(2);
        tool.SetMaxDownQueue(2);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        const std::filesystem::path planeFbxPath = fbxRoot / "plane.fbx";
        const std::filesystem::path sphereFbxPath = fbxRoot / "sphere.fbx";

        if (!postprocess_test::RequireFileExists(xyzRoot / "meta.json") ||
            !postprocess_test::RequireFileExists(xyzRoot / "tilemapresource.xml") ||
            !postprocess_test::RequireImageSize(outputRoot / "merged.png", 64, 64) ||
            !postprocess_test::RequireFileExists(fbxRoot / "manifest.json") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(fbxRoot / "manifest.json", "textures"),
                "manifest.json does not list textures") ||
            !postprocess_test::Require(
                postprocess_test::HasAnyPng(fbxRoot),
                "FBX texture directory has no png output") ||
            !postprocess_test::RequireNonEmptyFile(planeFbxPath) ||
            !postprocess_test::RequireNonEmptyFile(sphereFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(planeFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(sphereFbxPath) ||
            !postprocess_test::RequirePlaneFbxUvMatchesNorthAxis(planeFbxPath) ||
            !RequireFbxTexturesBesideModel(planeFbxPath, fbxRoot) ||
            !RequireFbxTexturesBesideModel(sphereFbxPath, fbxRoot) ||
            !postprocess_test::Require(
                !std::filesystem::exists(fbxRoot / "mesh") &&
                    !std::filesystem::exists(fbxRoot / "textures"),
                "FBX output still contains legacy nested directories"))
        {
            return false;
        }

        const std::filesystem::path texturePath = FindFirstPng(fbxRoot);
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
