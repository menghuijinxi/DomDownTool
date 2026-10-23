#include "TencentVectorDownloader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include "curl/curl.h"
#include "fmt/format.h"

#include "GCJ02GoogleDownBase.h"
#include "TencentVectorDecoder.h"

namespace
{
    using namespace DownTool;
    constexpr size_t kMaxResponseBytes = 32 * 1024 * 1024;

    bool Cancelled(const AmapVectorDownloadOptions& options)
    {
        return options.cancellationRequested && options.cancellationRequested();
    }

    size_t Receive(char* bytes, size_t size, size_t count, void* context)
    {
        auto& response = *static_cast<std::string*>(context);
        if (size != 0 && count > (kMaxResponseBytes - response.size()) / size)
        {
            return 0;
        }
        try
        {
            response.append(bytes, size * count);
            return size * count;
        }
        catch (...)
        {
            return 0;
        }
    }

    int TransferProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        try
        {
            return Cancelled(*static_cast<const AmapVectorDownloadOptions*>(context)) ? 1 : 0;
        }
        catch (...)
        {
            return 1;
        }
    }

    std::string RequestTile(const AmapVectorDownloadOptions& options, const AmapVectorTile& tile)
    {
        using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
        CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
        if (!handle)
        {
            throw std::runtime_error("无法创建腾讯 GIS HTTP 请求。");
        }
        std::string response;
        const std::string url = TencentVectorDownloader::TileUrl(tile);
        curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_REFERER, "https://map.qq.com/");
        curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "Mozilla/5.0");
        curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, Receive);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, TransferProgress);
        curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &options);
        const CURLcode status = curl_easy_perform(handle.get());
        if (status != CURLE_OK)
        {
            throw std::runtime_error("腾讯 GIS 请求失败：" +
                                     std::string(curl_easy_strerror(status)));
        }
        return response;
    }

    std::filesystem::path CacheFile(const AmapVectorDownloadOptions& options,
                                    const AmapVectorTile& tile)
    {
        return options.GetTencentCachePath() / "v405-style100-compress1" /
               std::to_string(tile.zoom) / std::to_string(tile.x) /
               (std::to_string(tile.y) + ".json");
    }

    void WriteCache(const std::filesystem::path& path, const std::string& response)
    {
        std::filesystem::create_directories(path.parent_path());
        // 仅用于并发缓存写入的临时文件去重，不保存请求或用户状态。
        static std::atomic_uint64_t sequence = 0;
        auto temporary = path;
        temporary +=
            fmt::format(".{}.{}.tmp", std::chrono::steady_clock::now().time_since_epoch().count(),
                        sequence.fetch_add(1));
        try
        {
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(response.data(), static_cast<std::streamsize>(response.size()));
                output.close();
                if (!output)
                {
                    throw std::runtime_error("腾讯原始瓦片缓存写入失败。");
                }
            }
            std::error_code error;
            // Windows rename 不覆盖旧文件；只在新响应已完整写入后替换损坏缓存。
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
            if (error)
            {
                throw std::runtime_error("腾讯原始瓦片缓存替换失败：" + error.message());
            }
        }
        catch (...)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
    }

    std::pair<AmapVectorDecodedData, bool> LoadTile(const AmapVectorDownloadOptions& options,
                                                    const AmapVectorTile& tile)
    {
        AmapVectorDecodedData decoded;
        std::string error;
        const auto cache = CacheFile(options, tile);
        if (options.useRawTileCache && std::filesystem::is_regular_file(cache) &&
            std::filesystem::file_size(cache) <= kMaxResponseBytes)
        {
            std::ifstream input(cache, std::ios::binary);
            const std::string envelope{std::istreambuf_iterator<char>(input), {}};
            if (TencentVectorDecoder::DecodeEnvelope(envelope, tile, decoded, error))
            {
                return {std::move(decoded), true};
            }
        }
        const std::string response = RequestTile(options, tile);
        if (!TencentVectorDecoder::DecodeEnvelope(response, tile, decoded, error))
        {
            throw std::runtime_error(error);
        }
        if (Cancelled(options))
        {
            throw std::runtime_error("腾讯 GIS 任务已取消。");
        }
        if (options.useRawTileCache)
        {
            WriteCache(cache, response);
        }
        return {std::move(decoded), false};
    }

    std::string FeatureSignature(const AmapVectorFeature& feature)
    {
        std::string signature = std::to_string(feature.mainKey);
        for (const auto& part : feature.geometryParts)
        {
            signature += "|";
            for (const auto& point : part)
            {
                signature += fmt::format("{:.9f},{:.9f};", point.longitude, point.latitude);
            }
        }
        signature +=
            feature.GetHeightMeters() ? fmt::format("h={}", *feature.GetHeightMeters()) : "";
        for (const auto& name : feature.names)
        {
            signature += name.text;
        }
        return signature;
    }
} // namespace

std::string DownTool::TencentVectorDownloader::TileUrl(const AmapVectorTile& tile)
{
    if (tile.zoom < 3 || tile.zoom > 18 || tile.x >= (1u << tile.zoom) ||
        tile.y >= (1u << tile.zoom))
    {
        throw std::invalid_argument("腾讯 GIS 瓦片坐标无效。");
    }
    return fmt::format("https://rt{}.map.gtimg.com/tile?styleid=0&z={}&x={}&y={}&version=405"
                       "&building=1&style=100&compress=1&mapType=hybrid",
                       (tile.x + tile.y) % 4, tile.zoom, tile.x, (1u << tile.zoom) - 1 - tile.y);
}

std::vector<DownTool::AmapVectorTile> DownTool::TencentVectorDownloader::SelectTiles(
    const AmapVectorDownloadOptions& options)
{
    const uint32_t zoom = options.GetTencentDataZoom();
    if (zoom < 3 || zoom > 18)
    {
        throw std::invalid_argument("腾讯 GIS 数据层级暂支持 z3 至 z18。");
    }
    std::string error;
    if (!options.polygonsWgs84.empty() && !ValidateWgs84Polygons(options.polygonsWgs84, error))
    {
        throw std::invalid_argument(error);
    }
    const auto bounds = options.polygonsWgs84.empty()
                            ? CalculateGeographicBounds(options.centerWgs84, options.radiusMeters)
                            : CalculateGeographicBounds(options.polygonsWgs84);
    if (!bounds)
    {
        throw std::invalid_argument("腾讯 GIS 下载范围无效。");
    }
    const double scale = std::ldexp(1., static_cast<int>(zoom));
    double minX = scale;
    double minY = scale;
    double maxX = 0.;
    double maxY = 0.;
    // 沿边采样覆盖 GCJ-02 偏移的非线性，外扩一块保留边界缓冲数据。
    for (unsigned index = 0; index <= 16; ++index)
    {
        const double ratio = static_cast<double>(index) / 16.;
        const double lon = bounds->west + (bounds->east - bounds->west) * ratio;
        const double lat = bounds->south + (bounds->north - bounds->south) * ratio;
        for (const LonLat point : {LonLat{lon, bounds->south}, LonLat{lon, bounds->north},
                                   LonLat{bounds->west, lat}, LonLat{bounds->east, lat}})
        {
            const auto gcj = GCJ02GoogleDownBase::WGS84ToGCJ02(point);
            const double clampedLat = std::clamp(gcj.lat, -85.05112878, 85.05112878);
            const double x = (gcj.lon + 180.) / 360. * scale;
            const double y = (1. - std::asinh(std::tan(clampedLat * std::numbers::pi / 180.)) /
                                       std::numbers::pi) *
                             0.5 * scale;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }
    const auto clampIndex = [scale](double value) {
        return static_cast<uint32_t>(std::clamp(std::floor(value), 0., scale - 1.));
    };
    const uint32_t left = clampIndex(minX - 1.);
    const uint32_t right = clampIndex(maxX + 1.);
    const uint32_t top = clampIndex(minY - 1.);
    const uint32_t bottom = clampIndex(maxY + 1.);
    const uint64_t count = static_cast<uint64_t>(right - left + 1) * (bottom - top + 1);
    if (count > 100000)
    {
        throw std::invalid_argument("腾讯 GIS 下载范围超过 100000 块，请缩小范围后分批导出。");
    }
    std::vector<AmapVectorTile> tiles;
    tiles.reserve(static_cast<size_t>(count));
    for (uint32_t y = top; y <= bottom; ++y)
    {
        for (uint32_t x = left; x <= right; ++x)
        {
            tiles.push_back({x, y, zoom});
        }
    }
    return tiles;
}

DownTool::AmapVectorDownloadResult DownTool::TencentVectorDownloader::Download(
    const AmapVectorDownloadOptions& options) const
{
    AmapVectorDownloadResult result;
    try
    {
        if (!options.includeBaseLayers && !options.includeLabels)
        {
            throw std::invalid_argument("至少需要选择一类腾讯 GIS 图层。");
        }
        result.selectedTiles = SelectTiles(options);
        if (Cancelled(options))
        {
            throw std::runtime_error("腾讯 GIS 任务已取消。");
        }
        std::vector<std::pair<AmapVectorDecodedData, bool>> responses(result.selectedTiles.size());
        std::atomic_size_t next = 0;
        std::atomic_bool failed = false;
        std::mutex progressMutex;
        std::string firstError;
        AmapVectorDownloadProgress progress;
        progress.totalTaskCount = responses.size();
        if (options.progressCallback)
        {
            options.progressCallback(progress);
        }
        const size_t concurrency =
            std::min<size_t>(responses.size(), std::clamp(options.maxConcurrency == 0
                                                              ? std::thread::hardware_concurrency()
                                                              : options.maxConcurrency,
                                                          1u, 16u));
        {
            // jthread 在异常和正常路径均等待退出；返回后不再触发回调。
            std::vector<std::jthread> workers;
            for (size_t worker = 0; worker < concurrency; ++worker)
            {
                workers.emplace_back([&] {
                    try
                    {
                        while (!failed.load() && !Cancelled(options))
                        {
                            const size_t index = next.fetch_add(1);
                            if (index >= responses.size())
                            {
                                return;
                            }
                            responses[index] = LoadTile(options, result.selectedTiles[index]);
                            const std::lock_guard lock(progressMutex);
                            ++progress.completedTaskCount;
                            progress.cacheHitCount += responses[index].second ? 1 : 0;
                            progress.downloadedTaskCount += responses[index].second ? 0 : 1;
                            if (options.progressCallback)
                            {
                                options.progressCallback(progress);
                            }
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        failed.store(true);
                        const std::lock_guard lock(progressMutex);
                        if (firstError.empty())
                        {
                            firstError = exception.what();
                        }
                    }
                    catch (...)
                    {
                        failed.store(true);
                        const std::lock_guard lock(progressMutex);
                        firstError = "腾讯 GIS 下载回调或工作线程发生未知异常。";
                    }
                });
            }
        }
        if (failed.load())
        {
            throw std::runtime_error(firstError);
        }
        std::unordered_set<std::string> signatures;
        for (auto& response : responses)
        {
            if (Cancelled(options))
            {
                throw std::runtime_error("腾讯 GIS 任务已取消。");
            }
            result.cachedTaskCount += response.second ? 1 : 0;
            result.downloadedTaskCount += response.second ? 0 : 1;
            for (auto& feature : response.first.features)
            {
                const bool label = (feature.mainKey & 0xf0000) == 262144;
                if ((label ? !options.includeLabels : !options.includeBaseLayers) ||
                    (options.GetFeatureFilter() && !options.GetFeatureFilter()(feature)))
                {
                    continue;
                }
                if (feature.geometryParts.empty() ||
                    signatures.insert(FeatureSignature(feature)).second)
                {
                    result.decodedData.features.push_back(std::move(feature));
                }
            }
        }
        auto range = options.GetGisSpatialRange();
        if (range.GetIncludedPolygons().empty())
        {
            const auto bounds =
                options.polygonsWgs84.empty()
                    ? CalculateGeographicBounds(options.centerWgs84, options.radiusMeters)
                    : CalculateGeographicBounds(options.polygonsWgs84);
            range.SetIncludedPolygons(options.polygonsWgs84.empty()
                                          ? Wgs84Polygons{{{bounds->west, bounds->south},
                                                           {bounds->east, bounds->south},
                                                           {bounds->east, bounds->north},
                                                           {bounds->west, bounds->north}}}
                                          : options.polygonsWgs84);
        }
        if (!ApplyGisSpatialFilter(result.decodedData, range, result.error,
                                   options.cancellationRequested,
                                   options.GetProcessingProgressCallback()))
        {
            return result;
        }
        result.decodedData.status = true;
        result.decodedData.version = "Tencent 405";
        result.decodedData.packetCount = static_cast<uint32_t>(responses.size());
        result.downloadedPacketCount = result.decodedData.packetCount;
        result.cachedPacketCount = result.cachedTaskCount;
        result.featureCount = static_cast<uint32_t>(result.decodedData.features.size());
        result.success = true;
    }
    catch (const std::exception& exception)
    {
        result.error = "腾讯 GIS 下载失败：" + std::string(exception.what());
    }
    catch (...)
    {
        result.error = "腾讯 GIS 下载或回调发生未知异常。";
    }
    return result;
}
