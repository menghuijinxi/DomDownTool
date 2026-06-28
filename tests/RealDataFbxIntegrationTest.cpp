#include "PostProcessTestSupport.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "curl/curl.h"

#if defined(_WIN32)
#include "windows.h"
#endif

#include "BaiduDown.h"
#include "GaoDeDown.h"
#include "TencentDown.h"

namespace
{
    bool IsRealDataFbxTestEnabled()
    {
        const char* value = std::getenv("DOMDOWNTOOL_RUN_REAL_DATA_FBX_TESTS");
        return value != nullptr && std::string(value) == "1";
    }

    class CurlGlobalScope
    {
    public:
        CurlGlobalScope()
            : initialized(curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK)
        {
        }

        ~CurlGlobalScope()
        {
            if (initialized)
            {
                curl_global_cleanup();
            }
        }

        bool IsInitialized() const
        {
            return initialized;
        }

    private:
        bool initialized = false;
    };

    template<typename BaseTool>
    class TestRealFbxDownloadTool : public BaseTool
    {
    public:
        uint32_t GetTotalDownCountForTest() const
        {
            return this->TotalDownCount;
        }
    };

    bool RequireFbxOutput(
        const std::filesystem::path& outputRoot,
        uint32_t textureSize)
    {
        const std::filesystem::path textureRoot =
            outputRoot / "textures" / "fbx" / std::to_string(textureSize);
        const std::filesystem::path planeFbxPath =
            outputRoot / "mesh" / "plane" / "tiles.fbx";
        const std::filesystem::path sphereFbxPath =
            outputRoot / "mesh" / "sphere" / "tiles.fbx";
        return postprocess_test::RequireFileExists(textureRoot / "manifest.json") &&
            postprocess_test::Require(
                postprocess_test::ContainsText(textureRoot / "manifest.json", "textures"),
                "FBX manifest does not list textures") &&
            postprocess_test::Require(
                postprocess_test::HasAnyPng(textureRoot),
                "FBX texture directory has no png output") &&
            postprocess_test::RequireNonEmptyFile(planeFbxPath) &&
            postprocess_test::RequireNonEmptyFile(sphereFbxPath) &&
            postprocess_test::RequireFbxAxisAndUnit(planeFbxPath) &&
            postprocess_test::RequireFbxAxisAndUnit(sphereFbxPath) &&
            postprocess_test::RequirePlaneFbxUvMatchesNorthAxis(planeFbxPath);
    }

    template<typename MapTool>
    bool RunRealFbxDownloadCase(
        const std::string& caseName,
        const DownTool::LonLat& centerLonLat,
        uint32_t zoom)
    {
        constexpr uint32_t textureSize = 256;
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("real-data-fbx-" + caseName);
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        TestRealFbxDownloadTool<MapTool> tool;
        tool.SetCachePath(postprocess_test::PathToUtf8String(testRoot / "cache"));
        tool.SetPostProcessOutputPath(
            postprocess_test::PathToUtf8String(testRoot / "output"));
        tool.SetCenterLonLat(centerLonLat);
        tool.SetRadius(1.);
        tool.SetZoom(zoom);
        tool.SetMaxDownQueue(2);
        tool.SetPostProcessEnabled(false);
        tool.SetPostProcessMergedImageEnabled(false);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxTextureSize(textureSize);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
        tool.SetFbxMeshSubdivision(2);

        tool.StartDown();

        const std::filesystem::path outputRoot = testRoot / "output";
        return postprocess_test::Require(
            tool.GetTotalDownCountForTest() > 0,
            caseName + " did not calculate any target tile") &&
            postprocess_test::Require(
                tool.GetDowningCount() ==
                    static_cast<int32_t>(tool.GetTotalDownCountForTest()),
                caseName + " did not finish all target tiles") &&
            RequireFbxOutput(outputRoot, textureSize);
    }

    bool RunTest()
    {
        if (!IsRealDataFbxTestEnabled())
        {
            std::cout
                << "RealDataFbxIntegrationTest skipped. "
                << "Set DOMDOWNTOOL_RUN_REAL_DATA_FBX_TESTS=1 to run it.\n";
            return true;
        }

#if defined(_WIN32)
        SetConsoleOutputCP(CP_UTF8);
#endif

        CurlGlobalScope curlGlobalScope;
        if (!postprocess_test::Require(
            curlGlobalScope.IsInitialized(),
            "curl_global_init failed"))
        {
            return false;
        }

        bool success = true;
        success = RunRealFbxDownloadCase<DownTool::GaoDeDown>(
            "gaode",
            DownTool::LonLat(116.468067, 39.912247),
            16) &&
            success;
        success = RunRealFbxDownloadCase<DownTool::TencentDown>(
            "tencent",
            DownTool::LonLat(116.468067, 39.912247),
            16) &&
            success;
        success = RunRealFbxDownloadCase<DownTool::BaiduDown>(
            "baidu",
            DownTool::LonLat(116.468067, 39.912247),
            16) &&
            success;

        if (success)
        {
            std::cout << "RealDataFbxIntegrationTest passed\n";
        }
        return success;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
