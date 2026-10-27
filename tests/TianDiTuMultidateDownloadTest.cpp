#include "PostProcessTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
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
        using DownTool::TianDiTuMultidateDown::ParseTimelineRecords;
        using DownTool::TianDiTuMultidateDown::TryGetSourceLonLatForTilePixel;
        using DownTool::TianDiTuMultidateDown::TryGetSourceTileMatrixSize;
        using DownTool::TianDiTuMultidateDown::TryGetSourceTilePixelForLonLat;
    };

    class EmptyTileTianDiTuMultidateDown final : public TestTianDiTuMultidateDown
    {
    public:
        explicit EmptyTileTianDiTuMultidateDown(std::string url) : url_(std::move(url))
        {
        }

        void SetResponseUrl(std::string url)
        {
            url_ = std::move(url);
        }

        std::string MakeUri(const DownTool::Tile& tile) override
        {
            (void)tile;
            return url_;
        }

    private:
        std::string url_;
    };

    class PartialUnavailableTianDiTuMultidateDown final : public TestTianDiTuMultidateDown
    {
    public:
        explicit PartialUnavailableTianDiTuMultidateDown(std::string url) : url_(std::move(url))
        {
        }

        std::string MakeUri(const DownTool::Tile& tile) override
        {
            (void)tile;
            return url_;
        }

    protected:
        bool IsUnavailableTileResponseHeader(
            const std::map<std::string, std::string>& responseHeaders,
            const DownTool::Tile& tile) const override
        {
            (void)responseHeaders;
            return tile.X != 26978 || tile.Y != 4560;
        }

    private:
        std::string url_;
    };

    std::string FileUrl(const std::filesystem::path& path)
    {
        std::string value = postprocess_test::PathToUtf8String(path);
        std::replace(value.begin(), value.end(), '\\', '/');
        return value.starts_with('/') ? "file://" + value : "file:///" + value;
    }

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

    std::string EncodeTianDiTuGetRegionBody(std::string_view decodedBody)
    {
        std::string encoded;
        encoded.reserve(decodedBody.size() * 4);
        for (const unsigned char character : decodedBody)
        {
            const uint32_t packedValue = static_cast<uint32_t>(character) << 2;
            encoded.push_back(static_cast<char>((packedValue >> 24) & 0xFF));
            encoded.push_back(static_cast<char>((packedValue >> 16) & 0xFF));
            encoded.push_back(static_cast<char>((packedValue >> 8) & 0xFF));
            encoded.push_back(static_cast<char>(packedValue & 0xFF));
        }

        return encoded;
    }

    bool RunEmptyHistoryTest();

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

        const std::string beijingLayer =
            DownTool::TianDiTuMultidateDown::BuildCacheLayerName(
                "2024", "time-key-2024", "156110000", "北京");
        const std::string langfangLayer =
            DownTool::TianDiTuMultidateDown::BuildCacheLayerName(
                "2024", "time-key-2024", "156130000", "廊坊市");
        const std::string otherKeyLayer =
            DownTool::TianDiTuMultidateDown::BuildCacheLayerName(
                "2024", "another-time-key-2024", "156110000", "北京");
        if (!postprocess_test::Require(
                beijingLayer != "2024" && beijingLayer != "latest" &&
                    beijingLayer != langfangLayer && beijingLayer != otherKeyLayer,
                "TianDiTu multidate cache layer did not isolate year, key and region"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            tool.PrepareTimelineQueryForBounds(
                DownTool::LonLat(116.17, 39.92),
                DownTool::LonLat(116.21, 39.95),
                16),
            "TianDiTu multidate timeline query did not use the configured region"))
        {
            return false;
        }

        const std::string timelineUrl = tool.GetTimelineListUrl();
        if (!postprocess_test::Require(
            ContainsText(timelineUrl, "image.tianditu.gov.cn/api/multidate/basic/GetRegion") &&
                ContainsText(timelineUrl, "gb=156110000") &&
                ContainsText(timelineUrl, "lbx=116.17000000") &&
                ContainsText(timelineUrl, "lby=39.92000000") &&
                ContainsText(timelineUrl, "rux=116.21000000") &&
                ContainsText(timelineUrl, "ruy=39.95000000") &&
                ContainsText(timelineUrl, "level=16"),
            "TianDiTu multidate timeline URL did not match the official GetRegion API"))
        {
            return false;
        }

        TestTianDiTuMultidateDown langfangTool;
        langfangTool.SetTileSize(256);
        langfangTool.SetCenterLonLat(DownTool::LonLat(116.671609, 39.537395));
        if (!postprocess_test::Require(
                langfangTool.PrepareTimelineQueryForBounds(
                    DownTool::LonLat(116.66, 39.53),
                    DownTool::LonLat(116.69, 39.55),
                    17) &&
                    langfangTool.GetRegionGbCode() == "156130000" &&
                    langfangTool.GetRegionName() == "廊坊市" &&
                    ContainsText(langfangTool.GetTimelineListUrl(), "gb=156130000"),
                "TianDiTu multidate city region should use province parentGbcode with city name"))
        {
            return false;
        }

        DownTool::Tile langfangTile;
        double langfangPixelX = 0.;
        double langfangPixelY = 0.;
        if (!postprocess_test::Require(
                langfangTool.TryGetSourceTilePixelForLonLat(
                    DownTool::LonLat(116.671609, 39.537395),
                    17,
                    langfangTile,
                    langfangPixelX,
                    langfangPixelY) &&
                    langfangTile.X == 108014 && langfangTile.Y == 18372 &&
                    langfangTile.Zoom == 17,
                "TianDiTu multidate Langfang coordinate did not match the JS-proven tile"))
        {
            return false;
        }

        struct VerifiedLangfangTile
        {
            std::string_view year;
            std::string_view key;
        };
        // 这些样例来自 JS 登录态探针实际下载成功的廊坊同坐标瓦片。
        const std::vector<VerifiedLangfangTile> verifiedLangfangTiles = {
            {"2022",
             "68747470733a2f2f68656265692e7469616e646974752e676f762e636e2f67656f6573622f70726f78792f33316331336531343636383834326463393235383137623864306234323439652f38383665363062623765303134663232613730376465323365366636353035643f736572766963653d574d545326726571756573743d47657454696c652676657273696f6e3d312e302e30266c617965723d59585432303232267374796c653d64656661756c7426666f726d61743d696d6167652f706e672654696c654d61747269785365743d437573746f6d435253343332365363616c65595854323032322654696c654d61747269783d7b6c6576656c7d2654696c65526f773d7b726f777d2654696c65436f6c3d7b636f6c7d32303232"},
            {"2023",
             "68747470733a2f2f68656265692e7469616e646974752e676f762e636e2f67656f6573622f70726f78792f31316637313865326533306334373462393633383663363630663039323736302f38383665363062623765303134663232613730376465323365366636353035643f736572766963653d574d545326726571756573743d47657454696c652676657273696f6e3d312e302e30266c617965723d59585432303233267374796c653d64656661756c7426666f726d61743d696d6167652f706e672654696c654d61747269785365743d437573746f6d435253343332365363616c65595854323032332654696c654d61747269783d7b6c6576656c7d2654696c65526f773d7b726f777d2654696c65436f6c3d7b636f6c7d32303233"},
            {"2024",
             "68747470733a2f2f68656265692e7469616e646974752e676f762e636e2f67656f6573622f70726f78792f32393334623037616465393434663137393263363636353164386361626636372f38383665363062623765303134663232613730376465323365366636353035643f736572766963653d574d545326726571756573743d47657454696c652676657273696f6e3d312e302e30266c617965723d4842595832303234267374796c653d64656661756c7426666f726d61743d696d6167652f706e672654696c654d61747269785365743d437573746f6d435253343332365363616c6548425958323032342654696c654d61747269783d7b6c6576656c7d2654696c65526f773d7b726f777d2654696c65436f6c3d7b636f6c7d32303234"}};
        for (const VerifiedLangfangTile& verifiedTile : verifiedLangfangTiles)
        {
            langfangTool.SetYearKey(std::string(verifiedTile.year),
                                    std::string(verifiedTile.key));
            const std::string tileUrl = langfangTool.MakeUri(langfangTile);
            if (!postprocess_test::Require(
                    ContainsText(tileUrl, "x=108014") &&
                        ContainsText(tileUrl, "y=18372") &&
                        ContainsText(tileUrl, "z=17") &&
                        ContainsText(tileUrl, std::string("key=") +
                                                 std::string(verifiedTile.key)) &&
                        ContainsText(tileUrl, "gb=156130000") &&
                        ContainsText(tileUrl, "name=%E5%BB%8A%E5%9D%8A%E5%B8%82"),
                    "TianDiTu multidate Langfang tile URL diverged from JS probe"))
            {
                return false;
            }
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

        DownTool::LonLat roundTrip;
        if (!postprocess_test::Require(
            tool.TryGetSourceLonLatForTilePixel(
                sourceTile, pixelX, pixelY, roundTrip) &&
                std::abs(roundTrip.lon - beijing.lon) < 0.000001 &&
                std::abs(roundTrip.lat - beijing.lat) < 0.000001,
            "TianDiTu multidate geographic tile reverse projection failed"))
        {
            return false;
        }

        uint32_t matrixColumns = 0;
        uint32_t matrixRows = 0;
        if (!postprocess_test::Require(
            tool.TryGetSourceTileMatrixSize(17, matrixColumns, matrixRows) &&
                matrixColumns == 131072 && matrixRows == 65536,
            "TianDiTu multidate matrix size did not match EPSG:4326 layout"))
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

        std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo> records;
        if (!postprocess_test::Require(
            tool.ParseTimelineRecords(
                R"json({"status":200,"data":[{"uuid":"official-uuid-2024","title":"2024年影像","minlevel":10,"maxlevel":18}]})json",
                records) &&
                records.size() == 1 && records.front().year == "2024" &&
                records.front().key == "official-uuid-2024" &&
                records.front().time == "2024年影像" &&
                records.front().minZoom == 10 && records.front().maxZoom == 18,
            "TianDiTu multidate timeline parser did not accept official uuid/title fields"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            tool.TryParseTimelineRecords(R"json({"status":200,"data":[]})json", records) &&
                records.empty(),
            "TianDiTu multidate timeline parser should accept an empty official list"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            tool.TryParseTimelineRecords(
                R"json({"status":200,"message":"成功","data":{"page":{"records":[{"uuid":"page-uuid-2023","title":"2023年影像","minlevel":17,"maxlevel":18}],"total":1}}})json",
                records) &&
                records.size() == 1 && records.front().year == "2023" &&
                records.front().key == "page-uuid-2023" &&
                records.front().minZoom == 17 && records.front().maxZoom == 18,
            "TianDiTu multidate timeline parser did not accept data.page.records"))
        {
            return false;
        }

        const std::string encodedGetRegionBody = EncodeTianDiTuGetRegionBody(
            R"json({"status":200,"msg":"ok","data":"[{\"minlevel\":12,\"maxlevel\":18,\"time\":\"1951\",\"projection\":\"c\",\"key\":\"encoded-key-1951\"}]"})json");
        if (!postprocess_test::Require(
            tool.TryParseTimelineRecords(encodedGetRegionBody, records) &&
                records.size() == 1 && records.front().year == "1951" &&
                records.front().key == "encoded-key-1951" &&
                records.front().projection == "c" &&
                records.front().minZoom == 12 && records.front().maxZoom == 18,
            "TianDiTu multidate timeline parser did not decode GetRegion response"))
        {
            return false;
        }

        return RunEmptyHistoryTest();
    }

    bool RunEmptyHistoryTest()
    {
        const std::filesystem::path emptyHistoryRoot =
            postprocess_test::MakeTestRoot("tianditu-multidate-empty-history");
        const std::filesystem::path emptyResponsePath = emptyHistoryRoot / "empty.bin";
        const std::filesystem::path mergedImagePath = emptyHistoryRoot / "merged.png";
        if (!postprocess_test::Require(
                postprocess_test::ResetDirectory(emptyHistoryRoot),
                "无法准备多时相空瓦片测试目录"))
        {
            return false;
        }

        std::ofstream(emptyResponsePath, std::ios::binary | std::ios::trunc).close();
        EmptyTileTianDiTuMultidateDown emptyTileTool(FileUrl(emptyResponsePath));
        emptyTileTool.SetCenterLonLat(DownTool::LonLat(116.397389, 39.908722));
        emptyTileTool.SetRadius(1.);
        emptyTileTool.SetZoom(15);
        emptyTileTool.SetMaxDownQueue(1);
        emptyTileTool.SetCachePath(
            postprocess_test::PathToUtf8String(emptyHistoryRoot / "cache"));
        emptyTileTool.SetMultidateRegion("156110000", "北京");
        emptyTileTool.SetYearKey("1966", "empty-response-key");
        emptyTileTool.SetTransparentPlaceholderForEmptyTiles(true);
        emptyTileTool.SetPostProcessMergedImageEnabled(true);
        emptyTileTool.SetPostProcessMergedImageOutputPath(
            postprocess_test::PathToUtf8String(mergedImagePath));
        emptyTileTool.SetPostProcessMergedImageResolution(256);
        emptyTileTool.StartDown();
        const cv::Mat emptyHistoryImage = cv::imread(
            postprocess_test::PathToUtf8String(mergedImagePath), cv::IMREAD_UNCHANGED);
        const cv::Vec4b emptyHistoryPixel = emptyHistoryImage.empty() ?
            cv::Vec4b(1, 1, 1, 1) : emptyHistoryImage.at<cv::Vec4b>(0, 0);
        if (!postprocess_test::Require(
                emptyTileTool.GetDownloadError().empty() && !emptyHistoryImage.empty() &&
                    emptyHistoryImage.channels() == 4 && emptyHistoryPixel[3] == 0,
                "多时相历史空瓦片没有生成透明合并图"))
        {
            return false;
        }

        const std::filesystem::path validResponsePath = emptyHistoryRoot / "valid.png";
        const std::filesystem::path refreshedMergedImagePath = emptyHistoryRoot / "refreshed.png";
        const cv::Mat validTile(4, 4, CV_8UC4, cv::Scalar(20, 40, 60, 255));
        if (!postprocess_test::Require(
                cv::imwrite(postprocess_test::PathToUtf8String(validResponsePath), validTile),
                "无法准备多时相有效瓦片测试数据"))
        {
            return false;
        }
        emptyTileTool.SetResponseUrl(FileUrl(validResponsePath));
        emptyTileTool.SetPostProcessMergedImageOutputPath(
            postprocess_test::PathToUtf8String(refreshedMergedImagePath));
        emptyTileTool.StartDown();
        const cv::Mat refreshedHistoryImage = cv::imread(
            postprocess_test::PathToUtf8String(refreshedMergedImagePath), cv::IMREAD_UNCHANGED);
        const cv::Vec4b refreshedHistoryPixel = refreshedHistoryImage.empty() ?
            cv::Vec4b(0, 0, 0, 0) : refreshedHistoryImage.at<cv::Vec4b>(0, 0);
        if (!postprocess_test::Require(
                !refreshedHistoryImage.empty() && refreshedHistoryPixel[3] == 255 &&
                    std::abs(static_cast<int>(refreshedHistoryPixel[0]) - 20) <= 2 &&
                    std::abs(static_cast<int>(refreshedHistoryPixel[1]) - 40) <= 2 &&
                    std::abs(static_cast<int>(refreshedHistoryPixel[2]) - 60) <= 2,
                "多时相历史导出没有清理旧透明占位并重新下载真实瓦片"))
        {
            return false;
        }

        const std::filesystem::path partialHistoryRoot =
            postprocess_test::MakeTestRoot("tianditu-multidate-partial-history");
        const std::filesystem::path partialMergedImagePath = partialHistoryRoot / "merged.png";
        if (!postprocess_test::Require(
                postprocess_test::ResetDirectory(partialHistoryRoot),
                "无法准备多时相部分失败瓦片测试目录"))
        {
            return false;
        }

        PartialUnavailableTianDiTuMultidateDown partialTileTool(FileUrl(validResponsePath));
        partialTileTool.SetCenterLonLat(DownTool::LonLat(116.397389, 39.908722));
        partialTileTool.SetRadius(1.);
        partialTileTool.SetZoom(15);
        partialTileTool.SetMaxDownQueue(1);
        partialTileTool.SetCachePath(
            postprocess_test::PathToUtf8String(partialHistoryRoot / "cache"));
        partialTileTool.SetMultidateRegion("156110000", "北京");
        partialTileTool.SetYearKey("2024", "partial-response-key");
        partialTileTool.SetTransparentPlaceholderForEmptyTiles(true);
        partialTileTool.SetPostProcessMergedImageEnabled(true);
        partialTileTool.SetPostProcessMergedImageOutputPath(
            postprocess_test::PathToUtf8String(partialMergedImagePath));
        partialTileTool.SetPostProcessMergedImageResolution(256);
        partialTileTool.StartDown();
        const cv::Mat partialHistoryImage = cv::imread(
            postprocess_test::PathToUtf8String(partialMergedImagePath), cv::IMREAD_UNCHANGED);
        if (!postprocess_test::Require(partialTileTool.GetDownloadError().empty() &&
                                           !partialHistoryImage.empty() &&
                                           partialHistoryImage.channels() == 4,
                                       "多时相历史覆盖部分瓦片失败时没有补透明并生成合并图"))
        {
            return false;
        }

        std::cout << "TianDiTuMultidateDownloadTest passed\n";
        return true;
    }
}

int main(int argc, char** argv)
{
    return postprocess_test::RunGuarded(
        argc > 1 && std::string(argv[1]) == "--empty-history" ? RunEmptyHistoryTest : RunTest);
}
