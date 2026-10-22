#include "GeoTiffExporter.h"
#include "PostProcessTestSupport.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

#include "CopernicusDem.h"
#include "opencv2/imgcodecs.hpp"
#include "tiffio.h"

namespace
{
constexpr uint32_t kTiffTagGeoPixelScale = 33550;
constexpr uint32_t kTiffTagGeoKeyDirectory = 34735;
constexpr uint32_t kTiffTagGdalNoData = 42113;

TIFFExtendProc previousTagExtender = nullptr;

void RegisterGeoTiffFields(TIFF* tiff)
{
    static const TIFFFieldInfo fields[] = {
        {33550, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
         FIELD_CUSTOM, true, true, "GeoPixelScale"},
        {33922, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
         FIELD_CUSTOM, true, true, "GeoTiePoints"},
        {34735, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SHORT,
         FIELD_CUSTOM, true, true, "GeoKeyDirectory"},
        {kTiffTagGdalNoData, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII,
         FIELD_CUSTOM, true, false, "GdalNoData"}
    };
    TIFFMergeFieldInfo(
        tiff,
        fields,
        static_cast<uint32_t>(std::size(fields)));
    if (previousTagExtender != nullptr)
    {
        previousTagExtender(tiff);
    }
}

std::unique_ptr<TIFF, decltype(&TIFFClose)> OpenGeoTiff(
    const std::filesystem::path& path)
{
    previousTagExtender = TIFFSetTagExtender(RegisterGeoTiffFields);
#if defined(_WIN32)
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(
        TIFFOpenW(path.c_str(), "r"),
        TIFFClose);
#else
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(
        TIFFOpen(path.string().c_str(), "r"),
        TIFFClose);
#endif
    TIFFSetTagExtender(previousTagExtender);
    return tiff;
}

bool HasProjectedCrs(
    const uint16_t keyCount, const uint16_t* geoKeys, const uint16_t crsCode)
{
    if (geoKeys == nullptr)
    {
        return false;
    }
    for (uint16_t index = 4; index + 3 < keyCount; index += 4)
    {
        if (geoKeys[index] == 3072 &&
            geoKeys[index + 1] == 0 &&
            geoKeys[index + 3] == crsCode)
        {
            return true;
        }
    }
    return false;
}

std::shared_ptr<DownTool::CopernicusDemData> PrepareDemData(
    const std::filesystem::path& testRoot,
    std::string& error)
{
    DownTool::CopernicusDemOptions options;
    options.centerWgs84 = {116.5, 39.5};
    options.radiusMeters = 1000.;
    options.cachePath = testRoot / "dem-cache";
    options.SetFlattenPolygonsWgs84(
        {{{116.25, 39.25}, {116.75, 39.25}, {116.75, 39.75}, {116.25, 39.75}}});
    options.downloadCallback =
        [](
            const std::string_view,
            const std::filesystem::path& outputPath,
            const DownTool::CopernicusDemCancellationCallback&,
            std::string& downloadError)
        {
            cv::Mat image(4, 4, CV_32FC1, cv::Scalar(100.F));
            image.at<float>(2, 2) = 50.F;
            std::vector<unsigned char> encoded;
            if (!cv::imencode(".tif", image, encoded))
            {
                downloadError = "无法编码测试 DEM。";
                return false;
            }
            std::ofstream output(
                outputPath,
                std::ios::binary | std::ios::trunc);
            output.write(
                reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
            if (!output.good())
            {
                downloadError = "无法写入测试 DEM。";
                return false;
            }
            return true;
        };
    return DownTool::CopernicusDemData::Prepare(options, error);
}

bool RunTest()
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("geotiff-exporter");
    if (!postprocess_test::ResetDirectory(testRoot))
    {
        return false;
    }

    const std::optional<DownTool::GeographicBounds> domBounds =
        DownTool::CalculateGeographicBounds({116.5, 39.5}, 1.);
    const std::optional<DownTool::WebMercatorTileGrid> domGrid =
        domBounds.has_value() ?
        DownTool::BuildWebMercatorTileGrid(*domBounds, 15, 16) :
        std::nullopt;
    if (!postprocess_test::Require(
            domGrid.has_value(),
            "无法计算 DOM GeoTIFF 测试网格"))
    {
        return false;
    }

    const std::filesystem::path tileRoot = testRoot / "tiles";
    const DownTool::Tile domTile = domGrid->tileRange.MinTile;
    const std::filesystem::path sourcePath =
        tileRoot /
        std::to_string(domTile.Zoom) /
        std::to_string(domTile.X) /
        (std::to_string(domTile.Y) + ".png");
    const std::filesystem::path outputPath =
        testRoot / std::filesystem::u8path("中文 DOM.tif");
    std::filesystem::create_directories(sourcePath.parent_path());
    cv::Mat source(16, 16, CV_8UC4, cv::Scalar(10, 20, 30, 255));
    if (!cv::imwrite(sourcePath.string(), source))
    {
        return false;
    }

    std::string error;
    if (!postprocess_test::Require(
            DownTool::WriteDomGeoTiff(
                tileRoot,
                {},
                outputPath,
                *domGrid,
                {},
                {},
                error),
            "DOM GeoTIFF 写入失败：" + error))
    {
        return false;
    }

    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff =
        OpenGeoTiff(outputPath);
    if (!postprocess_test::Require(tiff != nullptr, "无法重新读取 DOM GeoTIFF"))
    {
        return false;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t keyCount = 0;
    uint16_t* geoKeys = nullptr;
    uint16_t scaleCount = 0;
    double* pixelScale = nullptr;
    uint32_t tileWidth = 0;
    uint32_t tileHeight = 0;
    if (!postprocess_test::Require(
            TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width) == 1 &&
                TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height) == 1 &&
                TIFFGetField(tiff.get(), TIFFTAG_TILEWIDTH, &tileWidth) == 1 &&
                TIFFGetField(tiff.get(), TIFFTAG_TILELENGTH, &tileHeight) == 1 &&
                TIFFGetField(
                    tiff.get(),
                    kTiffTagGeoKeyDirectory,
                    &keyCount,
                    &geoKeys) == 1 &&
                TIFFGetField(
                    tiff.get(),
                    kTiffTagGeoPixelScale,
                    &scaleCount,
                    &pixelScale) == 1,
            "DOM GeoTIFF 缺少尺寸或地理标签"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            width == domGrid->raster.width &&
                height == domGrid->raster.height,
            "DOM GeoTIFF 尺寸错误") ||
        !postprocess_test::Require(
            TIFFIsTiled(tiff.get()) != 0 &&
                tileWidth == domGrid->tileSize &&
                tileHeight == domGrid->tileSize,
            "DOM GeoTIFF 没有按原始瓦片分块写出") ||
        !postprocess_test::Require(
            scaleCount == 3 && pixelScale != nullptr &&
                std::abs(
                    pixelScale[0] -
                    (domGrid->raster.east - domGrid->raster.west) /
                        domGrid->raster.width) < 1.e-9 &&
                std::abs(
                    pixelScale[1] -
                    (domGrid->raster.north - domGrid->raster.south) /
                        domGrid->raster.height) < 1.e-9,
            "DOM GeoTIFF 像元尺寸错误") ||
        !postprocess_test::Require(
            HasProjectedCrs(keyCount, geoKeys, 3857),
            "DOM GeoTIFF 没有声明 EPSG:3857"))
    {
        return false;
    }
    tiff.reset();

    const std::shared_ptr<DownTool::CopernicusDemData> demData =
        PrepareDemData(testRoot, error);
    if (!postprocess_test::Require(
            demData != nullptr,
            "无法准备 DEM GeoTIFF 测试数据：" + error))
    {
        return false;
    }
    const std::optional<DownTool::GeographicBounds> bounds =
        DownTool::CalculateGeographicBounds({116.5, 39.5}, 1000.);
    const std::optional<DownTool::WebMercatorRasterGrid> demGrid =
        bounds.has_value() ?
        DownTool::BuildWebMercatorSampleGrid(*bounds, 1000.) :
        std::nullopt;
    const std::filesystem::path demPath =
        testRoot / std::filesystem::u8path("中文 DEM.tif");
    const bool demWritten =
        demGrid.has_value() &&
        DownTool::WriteDemGeoTiff(
            *demData,
            demPath,
            *demGrid,
            {},
            {},
            {},
            error);
    if (!postprocess_test::Require(
            demWritten,
            "DEM GeoTIFF 写入失败：" + error))
    {
        return false;
    }

    tiff = OpenGeoTiff(demPath);
    uint16_t bitsPerSample = 0;
    uint16_t sampleFormat = 0;
    uint16_t samplesPerPixel = 0;
    char* noDataText = nullptr;
    keyCount = 0;
    geoKeys = nullptr;
    if (!postprocess_test::Require(
            tiff != nullptr &&
                TIFFGetField(
                    tiff.get(),
                    TIFFTAG_BITSPERSAMPLE,
                    &bitsPerSample) == 1 &&
                TIFFGetField(
                    tiff.get(),
                    TIFFTAG_SAMPLEFORMAT,
                    &sampleFormat) == 1 &&
                TIFFGetField(
                    tiff.get(),
                    TIFFTAG_SAMPLESPERPIXEL,
                    &samplesPerPixel) == 1 &&
                TIFFGetField(
                    tiff.get(),
                    kTiffTagGeoKeyDirectory,
                    &keyCount,
                    &geoKeys) == 1 &&
                TIFFGetField(
                    tiff.get(),
                    kTiffTagGdalNoData,
                    &noDataText) == 1,
            "无法读取 DEM GeoTIFF 像素格式或地理标签"))
    {
        return false;
    }
    const double northernWeight = std::cos(39.625 * std::numbers::pi / 180.);
    const double southernWeight = std::cos(39.375 * std::numbers::pi / 180.);
    const double expectedMean =
        (200. * northernWeight + 150. * southernWeight) / (2. * (northernWeight + southernWeight));
    std::vector<float> elevations(demGrid->width);
    for (uint32_t row = 0; row < demGrid->height; ++row)
    {
        if (!postprocess_test::Require(
                TIFFReadScanline(tiff.get(), elevations.data(), row) == 1 &&
                    std::ranges::all_of(elevations,
                                        [expectedMean](float elevation) {
                                            return std::abs(elevation - expectedMean) < 1.e-4;
                                        }),
                "DEM GeoTIFF 像元没有使用平整区域平均高程"))
        {
            return false;
        }
    }
    return postprocess_test::Require(
               bitsPerSample == 32 &&
                   sampleFormat == SAMPLEFORMAT_IEEEFP &&
                   samplesPerPixel == 1,
               "DEM GeoTIFF 不是单通道 32 位浮点格式") &&
        postprocess_test::Require(
            HasProjectedCrs(keyCount, geoKeys, 3857),
            "DEM GeoTIFF 没有声明 EPSG:3857") &&
        postprocess_test::Require(
            noDataText != nullptr && std::string_view(noDataText) == "nan",
            "DEM GeoTIFF 没有声明 NaN NoData") &&
        postprocess_test::RequireNonEmptyFile(demPath);
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
