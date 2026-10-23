#include "PostProcessTestSupport.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include "BaiduDown.h"

namespace
{
    class TestBaiduDown : public DownTool::BaiduDown
    {
    public:
        using DownTool::BaiduDown::GetOffsetTiles;
        using DownTool::BaiduDown::GetTileFilePath;
        using DownTool::BaiduDown::IsUnavailableTileFile;
        using DownTool::BaiduDown::ReadPostProcessTileRegion;
        using DownTool::BaiduDown::TryGetSourceTilePixelForLonLat;
    };

    bool ContainsText(const std::string& text, const std::string& expected)
    {
        return text.find(expected) != std::string::npos;
    }

    bool WriteVerticalGradientTile(const std::filesystem::path& path, uint32_t tileSize)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            return false;
        }

        cv::Mat image(static_cast<int>(tileSize), static_cast<int>(tileSize), CV_8UC4);
        for (int y = 0; y < image.rows; y++)
        {
            const unsigned char value = static_cast<unsigned char>(
                std::round(static_cast<double>(y) * 255. / (image.rows - 1)));
            for (int x = 0; x < image.cols; x++)
            {
                image.at<cv::Vec4b>(y, x) = cv::Vec4b(value, 0, 0, 255);
            }
        }

        return cv::imwrite(postprocess_test::PathToUtf8String(path), image);
    }

    bool GetSourceSampleForOutputRow(
        TestBaiduDown& tool,
        const DownTool::Tile& targetTile,
        int outputRow,
        DownTool::Tile& sourceTile,
        double& sourcePixelY)
    {
        constexpr int kOutputWidth = 1;
        constexpr int kOutputHeight = 256;
        const double targetPixelX =
            (0.5) * static_cast<double>(tool.GetTileSize()) / kOutputWidth;
        const double targetPixelY =
            (static_cast<double>(outputRow) + 0.5) *
            static_cast<double>(tool.GetTileSize()) / kOutputHeight;

        DownTool::LonLat targetLonLat;
        if (!tool.TryGetLonLatForTilePixel(
            targetTile, targetPixelX, targetPixelY, targetLonLat))
        {
            return false;
        }

        const DownTool::LonLat sourceLonLat =
            tool.ConvertTargetLonLatToSourceLonLat(targetLonLat);
        double sourcePixelX = 0.;
        return tool.TryGetSourceTilePixelForLonLat(
            sourceLonLat, targetTile.Zoom, sourceTile, sourcePixelX, sourcePixelY);
    }

    bool RunPostProcessSamplingDirectionTest(const DownTool::LonLat& wgs84Beijing)
    {
        constexpr uint32_t kZoom = 16;
        constexpr uint32_t kTileSize = 256;
        constexpr int kOutputHeight = 256;
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("baidu-projection-sampling");
        if (!postprocess_test::ResetDirectory(testRoot))
        {
            return false;
        }

        TestBaiduDown tool;
        tool.SetTileSize(kTileSize);
        tool.SetCachePath(postprocess_test::PathToUtf8String(testRoot / "cache"));

        DownTool::Tile targetTile;
        double targetPixelX = 0.;
        double targetPixelY = 0.;
        if (!postprocess_test::Require(
            tool.TryGetTilePixelForLonLat(
                wgs84Beijing, kZoom, targetTile, targetPixelX, targetPixelY),
            "WGS84 lon/lat did not map to a target tile"))
        {
            return false;
        }

        const DownTool::TileRange sourceTileRange = tool.GetOffsetTiles(targetTile);
        for (int32_t x = sourceTileRange.MinTile.X; x <= sourceTileRange.MaxTile.X; x++)
        {
            for (int32_t y = sourceTileRange.MinTile.Y; y <= sourceTileRange.MaxTile.Y; y++)
            {
                if (!WriteVerticalGradientTile(
                    tool.GetTileFilePath(DownTool::Tile(x, y, kZoom)),
                    kTileSize))
                {
                    return postprocess_test::Require(false, "write Baidu gradient tile failed");
                }
            }
        }

        cv::Mat outputImage =
            tool.ReadPostProcessTileRegion(targetTile, 1, kOutputHeight);
        if (!postprocess_test::Require(
            !outputImage.empty(),
            "Baidu postprocess sampling produced an empty image"))
        {
            return false;
        }

        const int centerRow = std::clamp(
            static_cast<int>(std::floor(targetPixelY)),
            1,
            kOutputHeight - 2);
        int northRow = 0;
        int southRow = 0;
        bool foundSameSourceTileRows = false;
        for (int offset = 1; offset <= 32; offset++)
        {
            const int candidateNorthRow = centerRow - offset;
            const int candidateSouthRow = centerRow + offset;
            if (candidateNorthRow < 0 || candidateSouthRow >= kOutputHeight)
            {
                continue;
            }

            DownTool::Tile northTile;
            DownTool::Tile southTile;
            double northSourcePixelY = 0.;
            double southSourcePixelY = 0.;
            if (!GetSourceSampleForOutputRow(
                tool, targetTile, candidateNorthRow, northTile, northSourcePixelY) ||
                !GetSourceSampleForOutputRow(
                    tool, targetTile, candidateSouthRow, southTile, southSourcePixelY))
            {
                continue;
            }

            if (northTile.X == southTile.X && northTile.Y == southTile.Y)
            {
                northRow = candidateNorthRow;
                southRow = candidateSouthRow;
                foundSameSourceTileRows = true;
                break;
            }
        }

        if (!postprocess_test::Require(
            foundSameSourceTileRows,
            "Baidu postprocess sampling test crossed source tile boundaries"))
        {
            return false;
        }

        const int northBlue = outputImage.at<cv::Vec4b>(northRow, 0)[0];
        const int southBlue = outputImage.at<cv::Vec4b>(southRow, 0)[0];
        return postprocess_test::Require(
            northBlue < southBlue,
            "Baidu postprocess sampling Y direction is inverted");
    }

    bool RunTest()
    {
        TestBaiduDown tool;
        tool.SetTileSize(256);
        tool.SetCachePath("tmp/baidu-projection/cache");

        TestBaiduDown defaultPathTool;
        const std::filesystem::path defaultTilePath =
            defaultPathTool.GetTileFilePath(DownTool::Tile(1, -2, 3));
        if (!postprocess_test::Require(
            ContainsText(
                postprocess_test::PathToUtf8String(defaultTilePath),
                "百度地图"),
            "Baidu default cache path did not preserve UTF-8 text"))
        {
            return false;
        }

        const std::filesystem::path unavailableTestRoot =
            postprocess_test::MakeTestRoot("baidu-unavailable-detection");
        if (!postprocess_test::ResetDirectory(unavailableTestRoot))
        {
            return false;
        }

        const std::filesystem::path tinyTilePath = unavailableTestRoot / "tiny.jpg";
        {
            std::ofstream tinyTileStream(tinyTilePath, std::ios::binary);
            tinyTileStream << "x";
        }
        if (!postprocess_test::Require(
            tool.IsUnavailableTileFile(tinyTilePath, DownTool::Tile(0, 0, 18)),
            "Baidu tiny error response was not treated as unavailable"))
        {
            return false;
        }

        const std::filesystem::path sameSizeDifferentMd5Path =
            unavailableTestRoot / "same-size-different-md5.jpg";
        {
            std::ofstream tileStream(sameSizeDifferentMd5Path, std::ios::binary);
            std::string content(2506, '\0');
            content[0] = static_cast<char>(0xFF);
            content[1] = static_cast<char>(0xD8);
            tileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        if (!postprocess_test::Require(
            !tool.IsUnavailableTileFile(
                sameSizeDifferentMd5Path, DownTool::Tile(0, 0, 18)),
            "Baidu unavailable detection used file size without MD5"))
        {
            return false;
        }

        const DownTool::LonLat wgs84Beijing(116.468067, 39.912247);
        const DownTool::LonLat bd09Beijing =
            DownTool::BaiduDown::WGS84ToBD09(wgs84Beijing);
        const DownTool::LonLat wgs84RoundTrip =
            DownTool::BaiduDown::BD09ToWGS84(bd09Beijing);

        if (!postprocess_test::Require(
            std::abs(bd09Beijing.lon - 116.4807666808) < 0.000001 &&
                std::abs(bd09Beijing.lat - 39.9192982015) < 0.000001,
            "BD09 conversion did not match the expected sample coordinate"))
        {
            return false;
        }

        if (!postprocess_test::Require(
            std::abs(wgs84RoundTrip.lon - wgs84Beijing.lon) < 0.0002 &&
                std::abs(wgs84RoundTrip.lat - wgs84Beijing.lat) < 0.0002,
            "BD09/WGS84 round trip is outside expected tolerance"))
        {
            return false;
        }

        DownTool::Tile sourceTile;
        double pixelX = 0.;
        double pixelY = 0.;
        if (!postprocess_test::Require(
            tool.TryGetSourceTilePixelForLonLat(
                bd09Beijing, 16, sourceTile, pixelX, pixelY),
            "BD09 lon/lat did not map to a Baidu tile") ||
            !postprocess_test::Require(
                pixelX >= 0. && pixelX < 256. && pixelY >= 0. && pixelY < 256.,
                "Baidu source pixel is outside the tile"))
        {
            return false;
        }

        bool foundSameTileNorthSouthPair = false;
        double northPixelY = 0.;
        double southPixelY = 0.;
        const double candidateDeltas[] = { 0.000001, 0.000002, 0.000005, 0.00001 };
        for (const double delta : candidateDeltas)
        {
            DownTool::Tile northTile;
            DownTool::Tile southTile;
            double northPixelX = 0.;
            double southPixelX = 0.;
            const DownTool::LonLat northBd09(bd09Beijing.lon, bd09Beijing.lat + delta);
            const DownTool::LonLat southBd09(bd09Beijing.lon, bd09Beijing.lat - delta);
            if (!tool.TryGetSourceTilePixelForLonLat(
                northBd09, 16, northTile, northPixelX, northPixelY) ||
                !tool.TryGetSourceTilePixelForLonLat(
                    southBd09, 16, southTile, southPixelX, southPixelY))
            {
                continue;
            }

            if (northTile.X == southTile.X && northTile.Y == southTile.Y)
            {
                foundSameTileNorthSouthPair = true;
                break;
            }
        }

        if (!postprocess_test::Require(
            foundSameTileNorthSouthPair,
            "Baidu north/south test points crossed tile boundaries") ||
            !postprocess_test::Require(
                northPixelY < southPixelY,
                "Baidu source pixel Y is not aligned with image row direction"))
        {
            return false;
        }

        if (!RunPostProcessSamplingDirectionTest(wgs84Beijing))
        {
            return false;
        }

        const std::string url = tool.MakeUri(sourceTile);
        if (!postprocess_test::Require(
            ContainsText(url, "maponline") &&
                ContainsText(url, "starpic") &&
                ContainsText(url, "z=16") &&
                !ContainsText(url, "214748"),
            "Baidu URL did not use readable signed tile coordinates"))
        {
            return false;
        }

        const std::filesystem::path tilePath = tool.GetTileFilePath(sourceTile);
        if (!postprocess_test::Require(
            ContainsText(tilePath.generic_string(), std::to_string(sourceTile.X)) &&
                ContainsText(tilePath.generic_string(), std::to_string(sourceTile.Y)),
            "Baidu cache path did not use signed tile coordinates"))
        {
            return false;
        }

        const DownTool::TileRange offsetTiles =
            tool.GetOffsetTiles(DownTool::Tile(54056, 24888, 16));
        if (!postprocess_test::Require(
            offsetTiles.MinTile.X <= offsetTiles.MaxTile.X &&
                offsetTiles.MinTile.Y <= offsetTiles.MaxTile.Y,
            "Baidu offset tile range is invalid"))
        {
            return false;
        }

        std::cout << "BaiduProjectionTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
