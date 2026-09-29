#include "GeoTiffExporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

#include "CopernicusDem.h"
#include "GoogleDownBase.h"

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"
#include "tiffio.h"

namespace
{
constexpr double kEarthRadiusMeters = 6378137.;
constexpr double kWebMercatorHalfWorld =
    std::numbers::pi * kEarthRadiusMeters;
constexpr double kMaximumWebMercatorLatitude = 85.05112877980659;

constexpr uint32_t kTiffTagGeoPixelScale = 33550;
constexpr uint32_t kTiffTagGeoTiePoints = 33922;
constexpr uint32_t kTiffTagGeoKeyDirectory = 34735;
constexpr uint32_t kTiffTagGeoDoubleParams = 34736;
constexpr uint32_t kTiffTagGeoAsciiParams = 34737;
constexpr uint32_t kTiffTagGdalNoData = 42113;

using TiffPtr = std::unique_ptr<TIFF, decltype(&TIFFClose)>;

double LongitudeToWebMercatorX(const double longitude)
{
    return longitude * kWebMercatorHalfWorld / 180.;
}

double LatitudeToWebMercatorY(const double latitude)
{
    const double clampedLatitude = std::clamp(
        latitude,
        -kMaximumWebMercatorLatitude,
        kMaximumWebMercatorLatitude);
    const double radians = clampedLatitude * std::numbers::pi / 180.;
    return kEarthRadiusMeters *
        std::log(std::tan(std::numbers::pi / 4. + radians / 2.));
}

DownTool::LonLat WebMercatorToLonLat(
    const double mercatorX, const double mercatorY)
{
    return {
        mercatorX * 180. / kWebMercatorHalfWorld,
        std::atan(std::sinh(mercatorY / kEarthRadiusMeters)) *
            180. / std::numbers::pi
    };
}

bool IsValidGrid(const DownTool::WebMercatorRasterGrid& grid)
{
    return std::isfinite(grid.west) && std::isfinite(grid.east) &&
        std::isfinite(grid.south) && std::isfinite(grid.north) &&
        grid.west < grid.east && grid.south < grid.north &&
        grid.width > 0 && grid.height > 0;
}

void RegisterGeoTiffFields(TIFF& tiff)
{
    static const TIFFFieldInfo fields[] = {
        {kTiffTagGeoPixelScale, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
         FIELD_CUSTOM, true, true, "GeoPixelScale"},
        {kTiffTagGeoTiePoints, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
         FIELD_CUSTOM, true, true, "GeoTiePoints"},
        {kTiffTagGeoKeyDirectory, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_SHORT,
         FIELD_CUSTOM, true, true, "GeoKeyDirectory"},
        {kTiffTagGeoDoubleParams, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_DOUBLE,
         FIELD_CUSTOM, true, true, "GeoDoubleParams"},
        {kTiffTagGeoAsciiParams, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII,
         FIELD_CUSTOM, true, false, "GeoAsciiParams"},
        {kTiffTagGdalNoData, TIFF_VARIABLE, TIFF_VARIABLE, TIFF_ASCII,
         FIELD_CUSTOM, true, false, "GdalNoData"}
    };
    TIFFMergeFieldInfo(
        &tiff,
        fields,
        static_cast<uint32_t>(std::size(fields)));
}

TiffPtr OpenTiff(
    const std::filesystem::path& path,
    const uint64_t uncompressedBytes,
    std::string& error)
{
    const char* mode = uncompressedBytes >= 0xE0000000ULL ? "w8" : "w";
#if defined(_WIN32)
    TiffPtr tiff(TIFFOpenW(path.c_str(), mode), TIFFClose);
#else
    TiffPtr tiff(TIFFOpen(path.string().c_str(), mode), TIFFClose);
#endif
    if (!tiff)
    {
        error = "无法创建 GeoTIFF 文件。";
        return {nullptr, TIFFClose};
    }
    RegisterGeoTiffFields(*tiff);
    return tiff;
}

bool SetCommonGeoTiffFields(
    TIFF& tiff,
    const DownTool::WebMercatorRasterGrid& grid,
    const uint16_t samplesPerPixel,
    const uint16_t bitsPerSample,
    const uint16_t sampleFormat,
    const uint16_t photometric,
    std::string& error)
{
    const double pixelScale[] = {
        (grid.east - grid.west) / grid.width,
        (grid.north - grid.south) / grid.height,
        0.
    };
    const double tiePoints[] = {
        0., 0., 0.,
        grid.west, grid.north, 0.
    };
    const uint16_t geoKeys[] = {
        1, 1, 0, 6,
        1024, 0, 1, 1,
        1025, 0, 1, 1,
        2048, 0, 1, 4326,
        2054, 0, 1, 9102,
        3072, 0, 1, 3857,
        3076, 0, 1, 9001
    };

    const bool success =
        TIFFSetField(&tiff, TIFFTAG_IMAGEWIDTH, grid.width) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_IMAGELENGTH, grid.height) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_SAMPLESPERPIXEL, samplesPerPixel) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_BITSPERSAMPLE, bitsPerSample) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_SAMPLEFORMAT, sampleFormat) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_PHOTOMETRIC, photometric) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) == 1 &&
        TIFFSetField(&tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) == 1 &&
        TIFFSetField(
            &tiff,
            kTiffTagGeoPixelScale,
            static_cast<uint16_t>(std::size(pixelScale)),
            pixelScale) == 1 &&
        TIFFSetField(
            &tiff,
            kTiffTagGeoTiePoints,
            static_cast<uint16_t>(std::size(tiePoints)),
            tiePoints) == 1 &&
        TIFFSetField(
            &tiff,
            kTiffTagGeoKeyDirectory,
            static_cast<uint16_t>(std::size(geoKeys)),
            geoKeys) == 1;
    if (!success)
    {
        error = "无法写入 GeoTIFF 文件头。";
    }
    return success;
}

cv::Mat BuildPolygonMask(
    const DownTool::WebMercatorRasterGrid& grid,
    const DownTool::Wgs84Polygons& polygonsWgs84)
{
    if (polygonsWgs84.empty())
    {
        return {};
    }

    cv::Mat mask(
        static_cast<int>(grid.height),
        static_cast<int>(grid.width),
        CV_8UC1,
        cv::Scalar(0));
    std::vector<std::vector<cv::Point>> contours;
    contours.reserve(polygonsWgs84.size());
    for (const DownTool::Wgs84Ring& ring : polygonsWgs84)
    {
        std::vector<cv::Point> contour;
        contour.reserve(ring.size());
        for (const DownTool::LonLat& coordinate : ring)
        {
            const double mercatorX = LongitudeToWebMercatorX(coordinate.lon);
            const double mercatorY = LatitudeToWebMercatorY(coordinate.lat);
            const double pixelX =
                (mercatorX - grid.west) * grid.width /
                (grid.east - grid.west);
            const double pixelY =
                (grid.north - mercatorY) * grid.height /
                (grid.north - grid.south);
            contour.emplace_back(
                static_cast<int>(std::llround(pixelX)),
                static_cast<int>(std::llround(pixelY)));
        }
        if (contour.size() >= 3)
        {
            contours.push_back(std::move(contour));
        }
    }
    if (!contours.empty())
    {
        cv::fillPoly(mask, contours, cv::Scalar(255), cv::LINE_8);
    }
    return mask;
}

cv::Mat ReadImage(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {};
    }
    const std::vector<uint8_t> encoded(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>{});
    return encoded.empty() ?
        cv::Mat{} : cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
}

std::filesystem::path FindTilePath(
    const std::filesystem::path& root,
    const DownTool::Tile& tile)
{
    if (root.empty())
    {
        return {};
    }
    const std::filesystem::path tileBase =
        root / std::to_string(tile.Zoom) / std::to_string(tile.X) /
        std::to_string(tile.Y);
    for (const char* extension : {".png", ".jpg", ".jpeg"})
    {
        std::filesystem::path path = tileBase;
        path.replace_extension(extension);
        std::error_code existsError;
        if (std::filesystem::exists(path, existsError) && !existsError)
        {
            return path;
        }
    }
    return {};
}

cv::Mat ReadRgbaTile(
    const std::filesystem::path& root,
    const DownTool::Tile& tile,
    const uint32_t tileSize)
{
    const std::filesystem::path path = FindTilePath(root, tile);
    if (path.empty())
    {
        return {};
    }
    const cv::Mat source = ReadImage(path);
    if (source.empty())
    {
        return {};
    }

    cv::Mat rgba;
    if (source.channels() == 4)
    {
        cv::cvtColor(source, rgba, cv::COLOR_BGRA2RGBA);
    }
    else if (source.channels() == 3)
    {
        cv::cvtColor(source, rgba, cv::COLOR_BGR2RGBA);
    }
    else if (source.channels() == 1)
    {
        cv::cvtColor(source, rgba, cv::COLOR_GRAY2RGBA);
    }
    if (rgba.empty())
    {
        return {};
    }
    if (rgba.cols != static_cast<int>(tileSize) ||
        rgba.rows != static_cast<int>(tileSize))
    {
        cv::resize(
            rgba,
            rgba,
            cv::Size(static_cast<int>(tileSize), static_cast<int>(tileSize)),
            0.,
            0.,
            cv::INTER_LINEAR);
    }
    return rgba;
}

void CompositeRgba(const cv::Mat& overlay, cv::Mat& base)
{
    for (int row = 0; row < base.rows; ++row)
    {
        const cv::Vec4b* overlayPixels = overlay.ptr<cv::Vec4b>(row);
        cv::Vec4b* basePixels = base.ptr<cv::Vec4b>(row);
        for (int column = 0; column < base.cols; ++column)
        {
            const uint32_t overlayAlpha = overlayPixels[column][3];
            const uint32_t baseAlpha = basePixels[column][3];
            const uint32_t inverseOverlayAlpha = 255u - overlayAlpha;
            const uint32_t outputAlphaScale =
                overlayAlpha * 255u + baseAlpha * inverseOverlayAlpha;
            if (outputAlphaScale == 0)
            {
                basePixels[column] = cv::Vec4b(0, 0, 0, 0);
                continue;
            }
            for (size_t channel = 0; channel < 3; ++channel)
            {
                basePixels[column][channel] = static_cast<uint8_t>(
                    (static_cast<uint32_t>(overlayPixels[column][channel]) *
                         overlayAlpha * 255u +
                     static_cast<uint32_t>(basePixels[column][channel]) *
                         baseAlpha * inverseOverlayAlpha +
                     outputAlphaScale / 2u) /
                    outputAlphaScale);
            }
            basePixels[column][3] = static_cast<uint8_t>(
                (outputAlphaScale + 127u) / 255u);
        }
    }
}

cv::Mat BuildDomTile(
    const std::filesystem::path& baseTileRoot,
    const std::filesystem::path& overlayTileRoot,
    const DownTool::Tile& tile,
    const uint32_t tileSize,
    bool& hasSourceTile)
{
    cv::Mat base = ReadRgbaTile(baseTileRoot, tile, tileSize);
    const cv::Mat overlay = ReadRgbaTile(overlayTileRoot, tile, tileSize);
    hasSourceTile = !base.empty() || !overlay.empty();
    if (base.empty())
    {
        base = cv::Mat(
            static_cast<int>(tileSize),
            static_cast<int>(tileSize),
            CV_8UC4,
            cv::Scalar(0, 0, 0, 0));
    }
    if (!overlay.empty())
    {
        CompositeRgba(overlay, base);
    }
    return base;
}

std::optional<DownTool::WebMercatorRasterGrid> BuildWebMercatorGrid(
    const DownTool::GeographicBounds& geographicBounds,
    const uint32_t width,
    const uint32_t height)
{
    DownTool::WebMercatorRasterGrid grid{
        LongitudeToWebMercatorX(geographicBounds.west),
        LongitudeToWebMercatorX(geographicBounds.east),
        LatitudeToWebMercatorY(geographicBounds.south),
        LatitudeToWebMercatorY(geographicBounds.north),
        width,
        height
    };
    return IsValidGrid(grid) ?
        std::optional<DownTool::WebMercatorRasterGrid>(grid) : std::nullopt;
}
} // namespace

std::optional<DownTool::GeographicBounds>
DownTool::CalculateExportGeographicBounds(
    const LonLat& centerWgs84,
    const double radiusMeters,
    const Wgs84Polygons& polygonsWgs84)
{
    return polygonsWgs84.empty() ?
        CalculateGeographicBounds(centerWgs84, radiusMeters) :
        CalculateGeographicBounds(polygonsWgs84);
}

std::optional<DownTool::WebMercatorTileGrid>
DownTool::BuildWebMercatorTileGrid(
    const GeographicBounds& geographicBounds,
    const uint32_t zoom,
    const uint32_t tileSize)
{
    if (zoom >= 31 || tileSize == 0)
    {
        return std::nullopt;
    }

    const Tile northWest = GoogleDownBase::LonLatToTile(
        {geographicBounds.west, geographicBounds.north},
        zoom);
    const Tile southEast = GoogleDownBase::LonLatToTile(
        {geographicBounds.east, geographicBounds.south},
        zoom);
    if (northWest.X > southEast.X || northWest.Y > southEast.Y)
    {
        return std::nullopt;
    }

    const uint64_t tileWidth =
        static_cast<uint64_t>(southEast.X - northWest.X) + 1;
    const uint64_t tileHeight =
        static_cast<uint64_t>(southEast.Y - northWest.Y) + 1;
    const uint64_t sourceWidth = tileWidth * tileSize;
    const uint64_t sourceHeight = tileHeight * tileSize;
    if (sourceWidth == 0 || sourceHeight == 0 ||
        sourceWidth > std::numeric_limits<uint32_t>::max() ||
        sourceHeight > std::numeric_limits<uint32_t>::max())
    {
        return std::nullopt;
    }

    const uint64_t tileCount = uint64_t{1} << zoom;
    const double tileSpan =
        2. * kWebMercatorHalfWorld / static_cast<double>(tileCount);
    const WebMercatorTileGrid grid{
        {
            -kWebMercatorHalfWorld + northWest.X * tileSpan,
            -kWebMercatorHalfWorld +
                (static_cast<double>(southEast.X) + 1.) * tileSpan,
            kWebMercatorHalfWorld -
                (static_cast<double>(southEast.Y) + 1.) * tileSpan,
            kWebMercatorHalfWorld - northWest.Y * tileSpan,
            static_cast<uint32_t>(sourceWidth),
            static_cast<uint32_t>(sourceHeight)
        },
        {northWest, southEast},
        tileSize
    };
    return IsValidGrid(grid.raster) ?
        std::optional<WebMercatorTileGrid>(grid) : std::nullopt;
}

std::optional<DownTool::WebMercatorRasterGrid>
DownTool::BuildWebMercatorSampleGrid(
    const GeographicBounds& geographicBounds,
    const double sampleMeters)
{
    if (!std::isfinite(sampleMeters) || sampleMeters <= 0.)
    {
        return std::nullopt;
    }

    const double west = LongitudeToWebMercatorX(geographicBounds.west);
    const double east = LongitudeToWebMercatorX(geographicBounds.east);
    const double south = LatitudeToWebMercatorY(geographicBounds.south);
    const double north = LatitudeToWebMercatorY(geographicBounds.north);
    const double widthMeters = east - west;
    const double heightMeters = north - south;
    if (!(widthMeters > 0.) || !(heightMeters > 0.))
    {
        return std::nullopt;
    }

    const double width = std::ceil(widthMeters / sampleMeters);
    const double height = std::ceil(heightMeters / sampleMeters);
    if (width > std::numeric_limits<uint32_t>::max() ||
        height > std::numeric_limits<uint32_t>::max())
    {
        return std::nullopt;
    }
    return BuildWebMercatorGrid(
        geographicBounds,
        std::max(1u, static_cast<uint32_t>(width)),
        std::max(1u, static_cast<uint32_t>(height)));
}

bool DownTool::WriteDomGeoTiff(
    const std::filesystem::path& baseTileRoot,
    const std::filesystem::path& overlayTileRoot,
    const std::filesystem::path& outputPath,
    const WebMercatorTileGrid& grid,
    const GeoTiffCancellationCallback& cancellationRequested,
    const GeoTiffProgressCallback& progressCallback,
    std::string& error)
{
    if (!IsValidGrid(grid.raster) ||
        grid.tileSize == 0 ||
        grid.tileSize > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        grid.tileSize % 16 != 0 ||
        grid.tileRange.MinTile.Zoom != grid.tileRange.MaxTile.Zoom ||
        grid.tileRange.MinTile.X > grid.tileRange.MaxTile.X ||
        grid.tileRange.MinTile.Y > grid.tileRange.MaxTile.Y ||
        (baseTileRoot.empty() && overlayTileRoot.empty()))
    {
        error = "DOM GeoTIFF 网格无效。";
        return false;
    }

    constexpr uint16_t samplesPerPixel = 4;
    const uint64_t uncompressedBytes =
        static_cast<uint64_t>(grid.raster.width) *
        grid.raster.height *
        samplesPerPixel;
    TiffPtr tiff = OpenTiff(outputPath, uncompressedBytes, error);
    if (!tiff ||
        !SetCommonGeoTiffFields(
            *tiff,
            grid.raster,
            samplesPerPixel,
            8,
            SAMPLEFORMAT_UINT,
            PHOTOMETRIC_RGB,
            error))
    {
        return false;
    }
    uint16_t extraSample = EXTRASAMPLE_UNASSALPHA;
    if (TIFFSetField(tiff.get(), TIFFTAG_TILEWIDTH, grid.tileSize) != 1 ||
        TIFFSetField(tiff.get(), TIFFTAG_TILELENGTH, grid.tileSize) != 1 ||
        TIFFSetField(
            tiff.get(),
            TIFFTAG_PREDICTOR,
            PREDICTOR_HORIZONTAL) != 1 ||
        TIFFSetField(
            tiff.get(),
            TIFFTAG_EXTRASAMPLES,
            1,
            &extraSample) != 1)
    {
        error = "无法设置 DOM GeoTIFF 分块或透明通道。";
        return false;
    }

    const uint64_t tileColumns =
        static_cast<uint64_t>(
            grid.tileRange.MaxTile.X - grid.tileRange.MinTile.X) + 1;
    const uint64_t tileRows =
        static_cast<uint64_t>(
            grid.tileRange.MaxTile.Y - grid.tileRange.MinTile.Y) + 1;
    const uint64_t totalTiles = tileColumns * tileRows;
    uint64_t completedTiles = 0;
    uint64_t sourceTileCount = 0;
    for (int32_t tileY = grid.tileRange.MinTile.Y;
         tileY <= grid.tileRange.MaxTile.Y;
         ++tileY)
    {
        for (int32_t tileX = grid.tileRange.MinTile.X;
             tileX <= grid.tileRange.MaxTile.X;
             ++tileX)
        {
            if (cancellationRequested && cancellationRequested())
            {
                error = "DOM GeoTIFF 导出已取消。";
                return false;
            }

            bool hasSourceTile = false;
            cv::Mat tileImage = BuildDomTile(
                baseTileRoot,
                overlayTileRoot,
                {tileX, tileY, grid.tileRange.MinTile.Zoom},
                grid.tileSize,
                hasSourceTile);
            if (hasSourceTile)
            {
                ++sourceTileCount;
            }
            if (!tileImage.isContinuous())
            {
                tileImage = tileImage.clone();
            }

            const uint32_t pixelX =
                static_cast<uint32_t>(
                    static_cast<uint64_t>(
                        tileX - grid.tileRange.MinTile.X) *
                    grid.tileSize);
            const uint32_t pixelY =
                static_cast<uint32_t>(
                    static_cast<uint64_t>(
                        tileY - grid.tileRange.MinTile.Y) *
                    grid.tileSize);
            const ttile_t tiffTile = TIFFComputeTile(
                tiff.get(),
                pixelX,
                pixelY,
                0,
                0);
            const tmsize_t byteCount = static_cast<tmsize_t>(
                static_cast<uint64_t>(grid.tileSize) *
                grid.tileSize *
                samplesPerPixel);
            if (TIFFWriteEncodedTile(
                    tiff.get(),
                    tiffTile,
                    tileImage.data,
                    byteCount) < 0)
            {
                error = "写入 DOM GeoTIFF 瓦片失败。";
                return false;
            }
            ++completedTiles;
            if (progressCallback)
            {
                progressCallback(completedTiles, totalTiles);
            }
        }
    }
    if (sourceTileCount == 0)
    {
        error = "DOM GeoTIFF 没有找到可写入的源瓦片。";
        return false;
    }
    return true;
}

bool DownTool::WriteDemGeoTiff(
    const CopernicusDemData& elevationData,
    const std::filesystem::path& outputPath,
    const WebMercatorRasterGrid& grid,
    const Wgs84Polygons& polygonsWgs84,
    const GeoTiffCancellationCallback& cancellationRequested,
    const GeoTiffProgressCallback& progressCallback,
    std::string& error)
{
    if (!IsValidGrid(grid) ||
        grid.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        grid.height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    {
        error = "DEM GeoTIFF 网格无效。";
        return false;
    }

    const uint64_t uncompressedBytes =
        static_cast<uint64_t>(grid.width) * grid.height * sizeof(float);
    TiffPtr tiff = OpenTiff(outputPath, uncompressedBytes, error);
    if (!tiff ||
        !SetCommonGeoTiffFields(
            *tiff,
            grid,
            1,
            32,
            SAMPLEFORMAT_IEEEFP,
            PHOTOMETRIC_MINISBLACK,
            error))
    {
        return false;
    }
    if (TIFFSetField(
            tiff.get(),
            TIFFTAG_ROWSPERSTRIP,
            TIFFDefaultStripSize(tiff.get(), 0)) != 1 ||
        TIFFSetField(
            tiff.get(),
            TIFFTAG_PREDICTOR,
            PREDICTOR_FLOATINGPOINT) != 1 ||
        TIFFSetField(tiff.get(), kTiffTagGdalNoData, "nan") != 1)
    {
        error = "无法设置 DEM GeoTIFF 压缩或 NoData 标签。";
        return false;
    }

    const cv::Mat mask = BuildPolygonMask(grid, polygonsWgs84);
    const double pixelWidth = (grid.east - grid.west) / grid.width;
    const double pixelHeight = (grid.north - grid.south) / grid.height;
    const float noData = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> scanline(grid.width, noData);
    for (uint32_t row = 0; row < grid.height; ++row)
    {
        if (cancellationRequested && cancellationRequested())
        {
            error = "DEM GeoTIFF 导出已取消。";
            return false;
        }

        std::fill(scanline.begin(), scanline.end(), noData);
        const uint8_t* maskPixels =
            mask.empty() ? nullptr : mask.ptr<uint8_t>(static_cast<int>(row));
        const double mercatorY =
            grid.north - (static_cast<double>(row) + 0.5) * pixelHeight;
        for (uint32_t column = 0; column < grid.width; ++column)
        {
            if (maskPixels != nullptr && maskPixels[column] == 0)
            {
                continue;
            }
            const double mercatorX =
                grid.west + (static_cast<double>(column) + 0.5) * pixelWidth;
            const std::optional<double> elevation =
                elevationData.Sample(WebMercatorToLonLat(mercatorX, mercatorY));
            if (elevation.has_value() && std::isfinite(*elevation))
            {
                scanline[column] = static_cast<float>(*elevation);
            }
        }

        if (TIFFWriteScanline(
            tiff.get(), scanline.data(), row, 0) < 0)
        {
            error = "写入 DEM GeoTIFF 像素失败。";
            return false;
        }
        if (progressCallback)
        {
            progressCallback(row + 1, grid.height);
        }
    }
    return true;
}
