#include "PostProcessTestSupport.h"

#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "TianDiTuMultidateDown.h"

namespace
{
    class TestTianDiTuMultidateDown : public DownTool::TianDiTuMultidateDown
    {
    public:
        using DownTool::TianDiTuMultidateDown::GetOffsetTiles;
        using DownTool::TianDiTuMultidateDown::GetRequestHeaders;
        using DownTool::TianDiTuMultidateDown::IsUnavailableTileResponseHeader;
        using DownTool::TianDiTuMultidateDown::TryGetSourceTilePixelForLonLat;
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
        TestTianDiTuMultidateDown tool;
        tool.SetTileSize(256);

        const DownTool::Tile tile(107966, 18243, 17);
        const std::string url = tool.MakeUri(tile);
        if (!postprocess_test::Require(
            ContainsText(url, "tianditu.gov.cn/img_c/wmts") &&
                ContainsText(url, "TILEMATRIXSET=c") &&
                ContainsText(url, "TILECOL=107966") &&
                ContainsText(url, "TILEROW=18243") &&
                ContainsText(url, "TILEMATRIX=17") &&
                ContainsText(url, "tk=b88bfb160c81dab8d9d20aaa74846360"),
            "TianDiTu multidate URL did not match the expected WMTS template"))
        {
            return false;
        }

        const std::vector<std::string> headers = tool.GetRequestHeaders(tile);
        if (!postprocess_test::Require(
            HasHeaderWithPrefix(headers, "Referer: https://image.tianditu.gov.cn/") &&
                HasHeaderWithPrefix(
                    headers,
                    "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0"),
            "TianDiTu multidate request headers did not include browser context"))
        {
            return false;
        }

        tool.SetMultidateRegion("156110000", "北京");
        tool.SetYearKey("2024", "time-key-2024");
        const std::string yearUrl = tool.MakeUri(tile);
        if (!postprocess_test::Require(
            ContainsText(yearUrl, "image.tianditu.gov.cn/api/multidate/basic/tile") &&
                ContainsText(yearUrl, "x=107966") &&
                ContainsText(yearUrl, "y=18243") &&
                ContainsText(yearUrl, "z=17") &&
                ContainsText(yearUrl, "key=time-key-2024") &&
                ContainsText(yearUrl, "gb=156110000") &&
                ContainsText(yearUrl, "name=%E5%8C%97%E4%BA%AC"),
            "TianDiTu multidate selected year URL did not include key and region"))
        {
            return false;
        }

        tool.SetSessionCookie("SESSION=test-session");
        const std::vector<std::string> cookieHeaders = tool.GetRequestHeaders(tile);
        if (!postprocess_test::Require(
            HasHeaderWithPrefix(cookieHeaders, "Cookie: SESSION=test-session"),
            "TianDiTu multidate request headers did not include the configured cookie"))
        {
            return false;
        }

        tool.UseLatestYear();
        const std::string latestUrl = tool.MakeUri(tile);
        if (!postprocess_test::Require(
            ContainsText(latestUrl, "tianditu.gov.cn/img_c/wmts") &&
                ContainsText(latestUrl, "tk=b88bfb160c81dab8d9d20aaa74846360"),
            "TianDiTu multidate latest year did not restore the WMTS template"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            tool.IsUnavailableTileResponseHeader(
                { { "content-type", "text/html; charset=utf-8" } }, tile),
            "TianDiTu multidate text response was not detected as unavailable"))
        {
            return false;
        }

        const DownTool::LonLat beijing(116.468067, 39.912247);
        DownTool::Tile sourceTile;
        double pixelX = 0.;
        double pixelY = 0.;
        if (!postprocess_test::Require(
            tool.TryGetSourceTilePixelForLonLat(
                beijing, 17, sourceTile, pixelX, pixelY),
            "TianDiTu multidate lon/lat did not map to a geographic source tile"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            sourceTile.X == 107940 &&
                sourceTile.Y == 18236 &&
                sourceTile.Zoom == 17 &&
                std::abs(pixelX - 186.650897) < 0.001 &&
                std::abs(pixelY - 100.950198) < 0.001,
            "TianDiTu multidate geographic tile mapping changed unexpectedly"))
        {
            return false;
        }

        DownTool::Tile northTile;
        DownTool::Tile southTile;
        double northPixelX = 0.;
        double northPixelY = 0.;
        double southPixelX = 0.;
        double southPixelY = 0.;
        if (!postprocess_test::Require(
            tool.TryGetSourceTilePixelForLonLat(
                DownTool::LonLat(beijing.lon, beijing.lat + 0.000001),
                17,
                northTile,
                northPixelX,
                northPixelY) &&
                tool.TryGetSourceTilePixelForLonLat(
                    DownTool::LonLat(beijing.lon, beijing.lat - 0.000001),
                    17,
                    southTile,
                    southPixelX,
                    southPixelY) &&
                northTile.X == southTile.X &&
                northTile.Y == southTile.Y &&
                northPixelY < southPixelY,
            "TianDiTu multidate geographic matrix Y direction is inverted"))
        {
            return false;
        }

        const DownTool::TileRange offsetTiles =
            tool.GetOffsetTiles(DownTool::Tile(53980, 24828, 16));
        if (!postprocess_test::Require(
            offsetTiles.MinTile.X <= offsetTiles.MaxTile.X &&
                offsetTiles.MinTile.Y <= offsetTiles.MaxTile.Y &&
                offsetTiles.MinTile.Zoom == 16 &&
                offsetTiles.MaxTile.Zoom == 16,
            "TianDiTu multidate offset tile range is invalid"))
        {
            return false;
        }

        std::cout << "TianDiTuMultidateDownloadTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
