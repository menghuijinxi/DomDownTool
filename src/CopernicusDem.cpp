#include "CopernicusDem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "GeographicLib/Geodesic.hpp"
#include "curl/curl.h"
#include "opencv2/core.hpp"
#include "opencv2/imgcodecs.hpp"

namespace
{
constexpr std::string_view kCopernicusDemBaseUrl =
    "https://copernicus-dem-30m.s3.eu-central-1.amazonaws.com";
constexpr long kConnectTimeoutSeconds = 15;
constexpr long kRequestTimeoutSeconds = 300;
constexpr int kDownloadAttemptCount = 3;
constexpr size_t kLoadedTileLimit = 4;
constexpr float kMinimumValidElevation = -1000.F;
constexpr float kMaximumValidElevation = 12000.F;

struct DemTile
{
    int32_t westDegrees = 0;
    int32_t southDegrees = 0;
    std::string id;
    std::filesystem::path filePath;
    int rows = 0;
    int columns = 0;
};

struct LoadedDemTile
{
    std::string id;
    cv::Mat image;
    uint64_t lastUsed = 0;
};

class CurlGlobalState
{
  public:
    CurlGlobalState()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            throw std::runtime_error("无法初始化 Copernicus DEM 网络组件。");
        }
    }

    ~CurlGlobalState()
    {
        curl_global_cleanup();
    }
};

CurlGlobalState& GetCurlGlobalState()
{
    static CurlGlobalState state;
    return state;
}

bool IsCancellationRequested(
    const DownTool::CopernicusDemCancellationCallback& cancellationRequested)
{
    return cancellationRequested && cancellationRequested();
}

std::string CoordinateLabel(int32_t value, char positivePrefix, char negativePrefix,
                            int width)
{
    std::ostringstream output;
    output << (value >= 0 ? positivePrefix : negativePrefix)
           << std::setw(width) << std::setfill('0') << std::abs(value) << "_00";
    return output.str();
}

std::string BuildTileId(int32_t westDegrees, int32_t southDegrees)
{
    return "Copernicus_DSM_COG_10_" +
        CoordinateLabel(southDegrees, 'N', 'S', 2) + "_" +
        CoordinateLabel(westDegrees, 'E', 'W', 3) + "_DEM";
}

std::string BuildTileUrl(const std::string& tileId)
{
    return std::string(kCopernicusDemBaseUrl) + "/" + tileId + "/" + tileId +
        ".tif";
}

size_t WriteDownloadBytes(char* bytes, size_t elementSize, size_t elementCount,
                          void* userData)
{
    std::ofstream& output = *static_cast<std::ofstream*>(userData);
    const size_t byteCount = elementSize * elementCount;
    output.write(bytes, static_cast<std::streamsize>(byteCount));
    return output.good() ? byteCount : 0;
}

int ReportDownloadProgress(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto& cancellationRequested =
        *static_cast<const DownTool::CopernicusDemCancellationCallback*>(userData);
    return IsCancellationRequested(cancellationRequested) ? 1 : 0;
}

bool DownloadFileOnce(
    std::string_view url,
    const std::filesystem::path& outputPath,
    const DownTool::CopernicusDemCancellationCallback& cancellationRequested,
    std::string& error)
{
    (void)GetCurlGlobalState();
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        error = "无法创建高程缓存临时文件。";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        error = "无法创建 Copernicus DEM 下载请求。";
        return false;
    }

    const std::string urlText(url);
    curl_easy_setopt(curl, CURLOPT_URL, urlText.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDownloadBytes);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ReportDownloadProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancellationRequested);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DomDownTool CopernicusDEM/1.0");

    const CURLcode result = curl_easy_perform(curl);
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);
    output.close();

    if (result == CURLE_ABORTED_BY_CALLBACK && IsCancellationRequested(cancellationRequested))
    {
        error = "高程下载已取消。";
        return false;
    }
    if (result != CURLE_OK)
    {
        error = "Copernicus DEM 下载失败：" +
            std::string(curl_easy_strerror(result));
        return false;
    }
    if (statusCode != 200)
    {
        error = "Copernicus DEM 服务返回 HTTP " + std::to_string(statusCode) + "。";
        return false;
    }
    return true;
}

bool DownloadFileWithRetry(
    std::string_view url,
    const std::filesystem::path& outputPath,
    const DownTool::CopernicusDemCancellationCallback& cancellationRequested,
    std::string& error)
{
    for (int attempt = 1; attempt <= kDownloadAttemptCount; ++attempt)
    {
        if (IsCancellationRequested(cancellationRequested))
        {
            error = "高程下载已取消。";
            return false;
        }

        if (DownloadFileOnce(url, outputPath, cancellationRequested, error))
        {
            return true;
        }
        if (attempt == kDownloadAttemptCount ||
            IsCancellationRequested(cancellationRequested))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
    }
    return false;
}

cv::Mat ReadDemImage(const std::filesystem::path& filePath)
{
    std::ifstream input(filePath, std::ios::binary);
    if (!input.is_open())
    {
        return {};
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(std::numeric_limits<int>::max()))
    {
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input.good() && !input.eof())
    {
        return {};
    }

    cv::Mat image = cv::imdecode(bytes, cv::IMREAD_ANYDEPTH | cv::IMREAD_UNCHANGED);
    if (image.empty() || image.channels() != 1)
    {
        return {};
    }
    if (image.type() != CV_32FC1)
    {
        cv::Mat converted;
        image.convertTo(converted, CV_32FC1);
        image = std::move(converted);
    }
    return image;
}

bool IsValidElevation(float elevation)
{
    return std::isfinite(elevation) && elevation > kMinimumValidElevation &&
        elevation < kMaximumValidElevation;
}

DownTool::CopernicusDemBounds CalculateBounds(const DownTool::LonLat& center,
                                               double radiusMeters)
{
    const GeographicLib::Geodesic& geodesic = GeographicLib::Geodesic::WGS84();
    double northLatitude = 0.;
    double northLongitude = 0.;
    double eastLatitude = 0.;
    double eastLongitude = 0.;
    double southLatitude = 0.;
    double southLongitude = 0.;
    double westLatitude = 0.;
    double westLongitude = 0.;
    geodesic.Direct(center.lat, center.lon, 0., radiusMeters,
                    northLatitude, northLongitude);
    geodesic.Direct(center.lat, center.lon, 90., radiusMeters,
                    eastLatitude, eastLongitude);
    geodesic.Direct(center.lat, center.lon, 180., radiusMeters,
                    southLatitude, southLongitude);
    geodesic.Direct(center.lat, center.lon, 270., radiusMeters,
                    westLatitude, westLongitude);
    return {
        std::min(westLongitude, eastLongitude),
        std::max(westLongitude, eastLongitude),
        std::min(southLatitude, northLatitude),
        std::max(southLatitude, northLatitude)};
}

std::vector<DemTile> BuildTiles(
    const DownTool::CopernicusDemBounds& bounds,
    const DownTool::Wgs84Polygons& polygons,
    const std::filesystem::path& cachePath)
{
    const int32_t minimumLongitude = static_cast<int32_t>(std::floor(bounds.west));
    const int32_t maximumLongitude = static_cast<int32_t>(std::floor(
        std::nextafter(bounds.east, -std::numeric_limits<double>::infinity())));
    const int32_t minimumLatitude = static_cast<int32_t>(std::floor(bounds.south));
    const int32_t maximumLatitude = static_cast<int32_t>(std::floor(
        std::nextafter(bounds.north, -std::numeric_limits<double>::infinity())));

    std::vector<DemTile> tiles;
    for (int32_t latitude = minimumLatitude; latitude <= maximumLatitude; ++latitude)
    {
        for (int32_t longitude = minimumLongitude; longitude <= maximumLongitude;
             ++longitude)
        {
            const DownTool::GeographicBounds tileBounds{
                static_cast<double>(longitude),
                static_cast<double>(longitude + 1),
                static_cast<double>(latitude),
                static_cast<double>(latitude + 1)
            };
            if (!DownTool::GeographicPolygonsIntersectBounds(
                    polygons, tileBounds))
            {
                continue;
            }
            DemTile tile;
            tile.westDegrees = longitude;
            tile.southDegrees = latitude;
            tile.id = BuildTileId(longitude, latitude);
            tile.filePath = cachePath / tile.id / (tile.id + ".tif");
            tiles.push_back(std::move(tile));
        }
    }
    return tiles;
}

std::optional<double> FindMinimumElevation(
    const cv::Mat& image,
    const DemTile& tile,
    const DownTool::CopernicusDemBounds& requestBounds)
{
    const double west = static_cast<double>(tile.westDegrees);
    const double east = west + 1.;
    const double south = static_cast<double>(tile.southDegrees);
    const double north = south + 1.;
    const double intersectionWest = std::max(west, requestBounds.west);
    const double intersectionEast = std::min(east, requestBounds.east);
    const double intersectionSouth = std::max(south, requestBounds.south);
    const double intersectionNorth = std::min(north, requestBounds.north);
    if (intersectionWest >= intersectionEast || intersectionSouth >= intersectionNorth)
    {
        return std::nullopt;
    }

    const int minimumX = std::clamp(
        static_cast<int>(std::floor((intersectionWest - west) * image.cols)),
        0, image.cols - 1);
    const int maximumX = std::clamp(
        static_cast<int>(std::ceil((intersectionEast - west) * image.cols)),
        minimumX + 1, image.cols);
    const int minimumY = std::clamp(
        static_cast<int>(std::floor((north - intersectionNorth) * image.rows)),
        0, image.rows - 1);
    const int maximumY = std::clamp(
        static_cast<int>(std::ceil((north - intersectionSouth) * image.rows)),
        minimumY + 1, image.rows);

    double minimum = std::numeric_limits<double>::infinity();
    for (int y = minimumY; y < maximumY; ++y)
    {
        const float* row = image.ptr<float>(y);
        for (int x = minimumX; x < maximumX; ++x)
        {
            if (IsValidElevation(row[x]))
            {
                minimum = std::min(minimum, static_cast<double>(row[x]));
            }
        }
    }
    return std::isfinite(minimum) ? std::optional<double>(minimum) : std::nullopt;
}

std::optional<double> BilinearSample(const cv::Mat& image, double pixelX, double pixelY)
{
    const double clampedX = std::clamp(pixelX, 0., static_cast<double>(image.cols - 1));
    const double clampedY = std::clamp(pixelY, 0., static_cast<double>(image.rows - 1));
    const int x0 = static_cast<int>(std::floor(clampedX));
    const int y0 = static_cast<int>(std::floor(clampedY));
    const int x1 = std::min(x0 + 1, image.cols - 1);
    const int y1 = std::min(y0 + 1, image.rows - 1);
    const double xFraction = clampedX - x0;
    const double yFraction = clampedY - y0;
    const std::array<float, 4> values = {
        image.at<float>(y0, x0), image.at<float>(y0, x1),
        image.at<float>(y1, x0), image.at<float>(y1, x1)};
    const std::array<double, 4> weights = {
        (1. - xFraction) * (1. - yFraction),
        xFraction * (1. - yFraction),
        (1. - xFraction) * yFraction,
        xFraction * yFraction};

    double weightedElevation = 0.;
    double totalWeight = 0.;
    for (size_t index = 0; index < values.size(); ++index)
    {
        if (IsValidElevation(values[index]))
        {
            weightedElevation += values[index] * weights[index];
            totalWeight += weights[index];
        }
    }
    return totalWeight > 0. ?
        std::optional<double>(weightedElevation / totalWeight) : std::nullopt;
}
} // namespace

class DownTool::CopernicusDemData::Impl
{
  public:
    CopernicusDemBounds bounds;
    std::vector<DemTile> tiles;
    std::vector<std::filesystem::path> sourceFiles;
    double minimumElevationMeters = 0.;
    mutable std::mutex cacheMutex;
    mutable std::vector<LoadedDemTile> loadedTiles;
    mutable uint64_t useSequence = 0;

    const DemTile* FindTile(const LonLat& lonLat) const
    {
        const auto contains = [&lonLat](const DemTile& tile)
        {
            return lonLat.lon >= tile.westDegrees &&
                lonLat.lon <= tile.westDegrees + 1. &&
                lonLat.lat >= tile.southDegrees &&
                lonLat.lat <= tile.southDegrees + 1.;
        };
        const auto iterator = std::find_if(tiles.begin(), tiles.end(), contains);
        return iterator != tiles.end() ? &*iterator : nullptr;
    }

    cv::Mat LoadTile(const DemTile& tile) const
    {
        std::lock_guard lock(cacheMutex);
        ++useSequence;
        const auto cached = std::find_if(
            loadedTiles.begin(), loadedTiles.end(),
            [&tile](const LoadedDemTile& value) { return value.id == tile.id; });
        if (cached != loadedTiles.end())
        {
            cached->lastUsed = useSequence;
            return cached->image;
        }

        cv::Mat image = ReadDemImage(tile.filePath);
        if (image.empty())
        {
            return {};
        }
        if (loadedTiles.size() >= kLoadedTileLimit)
        {
            const auto leastRecentlyUsed = std::min_element(
                loadedTiles.begin(), loadedTiles.end(),
                [](const LoadedDemTile& left, const LoadedDemTile& right)
                {
                    return left.lastUsed < right.lastUsed;
                });
            loadedTiles.erase(leastRecentlyUsed);
        }
        loadedTiles.push_back(LoadedDemTile{tile.id, image, useSequence});
        return image;
    }
};

DownTool::CopernicusDemData::CopernicusDemData(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

DownTool::CopernicusDemData::~CopernicusDemData() = default;

std::shared_ptr<DownTool::CopernicusDemData> DownTool::CopernicusDemData::Prepare(
    const CopernicusDemOptions& options, std::string& error)
{
    if (!std::isfinite(options.centerWgs84.lon) ||
        !std::isfinite(options.centerWgs84.lat) ||
        options.centerWgs84.lon < -180. || options.centerWgs84.lon > 180. ||
        options.centerWgs84.lat < -90. || options.centerWgs84.lat > 90.)
    {
        error = "Copernicus DEM 下载范围无效。";
        return nullptr;
    }
    if (options.polygonsWgs84.empty())
    {
        if (!std::isfinite(options.radiusMeters) || options.radiusMeters <= 0.)
        {
            error = "Copernicus DEM 下载范围无效。";
            return nullptr;
        }
    }
    else if (!ValidateWgs84Polygons(options.polygonsWgs84, error))
    {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    if (options.polygonsWgs84.empty())
    {
        impl->bounds = CalculateBounds(options.centerWgs84, options.radiusMeters);
    }
    else
    {
        const std::optional<GeographicBounds> polygonBounds =
            CalculateGeographicBounds(options.polygonsWgs84);
        if (!polygonBounds.has_value())
        {
            error = "无法计算 Copernicus DEM 多边形范围。";
            return nullptr;
        }
        impl->bounds = {
            polygonBounds->west,
            polygonBounds->east,
            polygonBounds->south,
            polygonBounds->north
        };
    }
    impl->tiles = BuildTiles(
        impl->bounds, options.polygonsWgs84, options.cachePath);
    if (impl->tiles.empty())
    {
        error = "Copernicus DEM 下载范围没有对应分幅。";
        return nullptr;
    }

    const CopernicusDemDownloadCallback download = options.downloadCallback ?
        options.downloadCallback : DownloadFileWithRetry;
    double minimumElevation = std::numeric_limits<double>::infinity();
    for (size_t index = 0; index < impl->tiles.size(); ++index)
    {
        if (IsCancellationRequested(options.cancellationRequested))
        {
            error = "高程下载已取消。";
            return nullptr;
        }

        DemTile& tile = impl->tiles[index];
        std::error_code directoryError;
        std::filesystem::create_directories(tile.filePath.parent_path(), directoryError);
        if (directoryError)
        {
            error = "无法创建高程缓存目录：" + directoryError.message();
            return nullptr;
        }

        cv::Mat image = ReadDemImage(tile.filePath);
        if (image.empty())
        {
            const std::filesystem::path partialPath = tile.filePath.string() + ".part";
            std::error_code removeError;
            std::filesystem::remove(partialPath, removeError);
            if (options.progressCallback)
            {
                options.progressCallback(
                    {"下载 Copernicus 高程", index, impl->tiles.size()});
            }
            if (!download(BuildTileUrl(tile.id), partialPath,
                          options.cancellationRequested, error))
            {
                std::filesystem::remove(partialPath, removeError);
                return nullptr;
            }
            std::filesystem::remove(tile.filePath, removeError);
            std::error_code renameError;
            std::filesystem::rename(partialPath, tile.filePath, renameError);
            if (renameError)
            {
                std::filesystem::remove(partialPath, removeError);
                error = "无法完成高程缓存文件：" + renameError.message();
                return nullptr;
            }
            image = ReadDemImage(tile.filePath);
        }
        if (image.empty())
        {
            error = "Copernicus DEM 文件无法读取：" + tile.id;
            return nullptr;
        }

        tile.rows = image.rows;
        tile.columns = image.cols;
        const std::optional<double> tileMinimum =
            FindMinimumElevation(image, tile, impl->bounds);
        if (tileMinimum)
        {
            minimumElevation = std::min(minimumElevation, *tileMinimum);
        }
        impl->sourceFiles.push_back(tile.filePath);
        if (options.progressCallback)
        {
            options.progressCallback(
                {"准备 Copernicus 高程", index + 1, impl->tiles.size()});
        }
    }

    if (!std::isfinite(minimumElevation))
    {
        error = "Copernicus DEM 范围内没有有效高程值。";
        return nullptr;
    }
    impl->minimumElevationMeters = minimumElevation;
    return std::shared_ptr<CopernicusDemData>(
        new CopernicusDemData(std::move(impl)));
}

std::optional<double> DownTool::CopernicusDemData::Sample(const LonLat& lonLat) const
{
    const DemTile* tile = impl_->FindTile(lonLat);
    if (tile == nullptr)
    {
        return std::nullopt;
    }
    const cv::Mat image = impl_->LoadTile(*tile);
    if (image.empty())
    {
        return std::nullopt;
    }

    const double pixelX =
        (lonLat.lon - static_cast<double>(tile->westDegrees)) * image.cols - 0.5;
    const double pixelY =
        (static_cast<double>(tile->southDegrees + 1) - lonLat.lat) * image.rows - 0.5;
    return BilinearSample(image, pixelX, pixelY);
}

double DownTool::CopernicusDemData::GetMinimumElevationMeters() const
{
    return impl_->minimumElevationMeters;
}

const DownTool::CopernicusDemBounds& DownTool::CopernicusDemData::GetBounds() const
{
    return impl_->bounds;
}

const std::vector<std::filesystem::path>&
DownTool::CopernicusDemData::GetSourceFiles() const
{
    return impl_->sourceFiles;
}
