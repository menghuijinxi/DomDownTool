#include "PostProcessTestSupport.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "curl/curl.h"

#if defined(_WIN32)
#include "windows.h"
#endif

#include "BaiduDown.h"
#include "GaoDeDown.h"
#include "TencentDown.h"

namespace
{
    bool IsRealDataTestEnabled()
    {
        const char* value = std::getenv("DOMDOWNTOOL_RUN_REAL_DATA_TESTS");
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
    class TestRealDownloadTool : public BaseTool
    {
    public:
        uint32_t GetTotalDownCountForTest() const
        {
            return this->TotalDownCount;
        }
    };

    template<typename MapTool>
    bool RunRealDownloadCase(
        const std::string& caseName,
        const DownTool::LonLat& centerLonLat,
        uint32_t zoom)
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("real-data-" + caseName);
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        TestRealDownloadTool<MapTool> tool;
        tool.SetCachePath(postprocess_test::PathToUtf8String(testRoot / "cache"));
        tool.SetPostProcessOutputPath(
            postprocess_test::PathToUtf8String(testRoot / "output"));
        tool.SetCenterLonLat(centerLonLat);
        tool.SetRadius(1.);
        tool.SetZoom(zoom);
        tool.SetMaxDownQueue(2);
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(512);
        tool.SetPostProcessFbxEnabled(false);

        tool.StartDown();

        return postprocess_test::Require(
            tool.GetTotalDownCountForTest() > 0,
            caseName + " did not calculate any target tile") &&
            postprocess_test::Require(
                tool.GetDowningCount() ==
                    static_cast<int32_t>(tool.GetTotalDownCountForTest()),
                caseName + " did not finish all target tiles") &&
            postprocess_test::RequireNonEmptyFile(testRoot / "output" / "meta.json") &&
            postprocess_test::RequireNonEmptyFile(
                testRoot / "output" / "tilemapresource.xml") &&
            postprocess_test::RequireReadableImage(
                testRoot / "output" / "merged.png");
    }

    bool RunTest()
    {
        if (!IsRealDataTestEnabled())
        {
            std::cout
                << "RealDataIntegrationTest skipped. "
                << "Set DOMDOWNTOOL_RUN_REAL_DATA_TESTS=1 to run it.\n";
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
        success = RunRealDownloadCase<DownTool::GaoDeDown>(
            "gaode",
            DownTool::LonLat(116.468067, 39.912247),
            16) &&
            success;
        success = RunRealDownloadCase<DownTool::TencentDown>(
            "tencent",
            DownTool::LonLat(116.468067, 39.912247),
            16) &&
            success;
        success = RunRealDownloadCase<DownTool::BaiduDown>(
            "baidu",
            DownTool::LonLat(116.468067, 39.912247),
            16) &&
            success;

        if (success)
        {
            std::cout << "RealDataIntegrationTest passed\n";
        }
        return success;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
