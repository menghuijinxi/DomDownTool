#include "PostProcessTestSupport.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "TianDiTuDown.h"

namespace
{
    class TestTianDiTuDown : public DownTool::TianDiTuDown
    {
    public:
        using DownTool::TianDiTuDown::GetRequestHeaders;
        using DownTool::TianDiTuDown::IsUnavailableTileResponseHeader;
    };

    bool ContainsText(const std::string& text, const std::string& expected)
    {
        return text.find(expected) != std::string::npos;
    }

    bool HasHeaderWithPrefix(
        const std::vector<std::string>& headers,
        const std::string& prefix)
    {
        for (const std::string& header : headers)
        {
            if (header.rfind(prefix, 0) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool RunTest()
    {
        TestTianDiTuDown tool;
        const DownTool::Tile tile(53970, 24831, 16);
        const std::string url = tool.MakeUri(tile);
        if (!postprocess_test::Require(
            ContainsText(url, "tianditu.gov.cn/img_w/wmts") &&
                ContainsText(url, "TILEMATRIXSET=w") &&
                ContainsText(url, "TILECOL=53970") &&
                ContainsText(url, "TILEROW=24831") &&
                ContainsText(url, "TILEMATRIX=16"),
            "TianDiTu URL did not match the expected WMTS template"))
        {
            return false;
        }

        const std::vector<std::string> headers = tool.GetRequestHeaders(tile);
        if (!postprocess_test::Require(
            HasHeaderWithPrefix(headers, "Referer: https://map.tianditu.gov.cn/") &&
                HasHeaderWithPrefix(headers, "User-Agent: Mozilla/5.0"),
            "TianDiTu request headers did not include browser headers"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            tool.IsUnavailableTileResponseHeader(
                { { "content-type", "application/json; charset=utf-8" } }, tile),
            "TianDiTu JSON response was not detected as unavailable"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            !tool.IsUnavailableTileResponseHeader({ { "content-type", "image/jpg" } }, tile),
            "TianDiTu image response was incorrectly detected as unavailable"))
        {
            return false;
        }

        std::cout << "TianDiTuDownloadTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
