#include "BaiduVectorDownloader.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include "curl/curl.h"
#include "fmt/format.h"
#include "openssl/evp.h"

#include "BaiduDown.h"
#include "BaiduVectorClassification.h"
#include "BaiduVectorDecoder.h"

namespace
{
using namespace DownTool;
constexpr size_t kMaxResponseBytes = 32 * 1024 * 1024;
constexpr const char* kCacheVersion = "v088-20261021";

void CheckCancellation(const AmapVectorDownloadOptions& options)
{
    if (options.cancellationRequested && options.cancellationRequested())
    {
        throw std::runtime_error("百度 GIS 任务已取消。");
    }
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
        CheckCancellation(*static_cast<const AmapVectorDownloadOptions*>(context));
        return 0;
    }
    catch (...)
    {
        return 1;
    }
}

std::string Request(const AmapVectorDownloadOptions& options, const std::string& url)
{
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    CurlHandle handle(curl_easy_init(), &curl_easy_cleanup);
    if (!handle)
    {
        throw std::runtime_error("无法创建百度 GIS HTTP 请求。");
    }
    CheckCancellation(options);
    std::string response;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_REFERER, "https://map.baidu.com/");
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
    const auto status = curl_easy_perform(handle.get());
    CheckCancellation(options);
    if (status != CURLE_OK)
    {
        throw std::runtime_error("百度 GIS 请求失败：" + std::string(curl_easy_strerror(status)));
    }
    return response;
}

std::string ReadCache(const std::filesystem::path& path)
{
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > kMaxResponseBytes)
    {
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

void WriteCache(const std::filesystem::path& path, const std::string& response)
{
    std::filesystem::create_directories(path.parent_path());
    // 仅用于并发写入的临时文件去重；不持有下载任务状态。
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
                throw std::runtime_error("百度 GIS 缓存写入失败。");
            }
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            throw std::runtime_error("百度 GIS 缓存替换失败：" + error.message());
        }
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::string EncodeParameters(const std::string& text)
{
    // 百度请求专用的 8→5 位编码，输入只含已验证的 ASCII 参数。
    const unsigned padding = 5 - static_cast<unsigned>(text.size() * 8 % 5);
    uint32_t bits = 0;
    unsigned length = padding;
    std::string encoded;
    for (const auto character : text)
    {
        bits = (bits << 8) | (static_cast<unsigned char>(character) << 1);
        length += 8;
        while (length >= 5)
        {
            length -= 5;
            encoded.push_back(static_cast<char>(((bits >> length) & 31u) + 50));
        }
    }
    encoded += std::to_string(padding);
    using CurlString = std::unique_ptr<char, decltype(&curl_free)>;
    CurlString escaped(curl_easy_escape(nullptr, encoded.c_str(), static_cast<int>(encoded.size())),
                       &curl_free);
    if (!escaped)
    {
        throw std::runtime_error("百度 GIS 请求参数编码失败。");
    }
    return escaped.get();
}

std::pair<AmapVectorDecodedData, bool> LoadTile(const AmapVectorDownloadOptions& options,
                                                const BaiduVectorTile& tile)
{
    CheckCancellation(options);
    const auto cache = options.GetBaiduCachePath() / kCacheVersion / "vector" /
                       std::to_string(tile.GetZoom()) / std::to_string(tile.GetX()) /
                       (std::to_string(tile.GetY()) + ".bin");
    BaiduVectorDecodeOptions decodeOptions(tile);
    decodeOptions.SetCancellation(options.cancellationRequested);
    AmapVectorDecodedData decoded;
    std::string error;
    if (options.useRawTileCache &&
        BaiduVectorDecoder::DecodeTile(ReadCache(cache), decodeOptions, decoded, error))
    {
        return {std::move(decoded), true};
    }
    CheckCancellation(options);
    const auto response = Request(options, BaiduVectorDownloader::TileUrl(tile));
    if (!BaiduVectorDecoder::DecodeTile(response, decodeOptions, decoded, error))
    {
        throw std::runtime_error(error);
    }
    CheckCancellation(options);
    if (options.useRawTileCache)
    {
        WriteCache(cache, response);
    }
    return {std::move(decoded), false};
}

std::string ClassificationDigest(const std::string& ids)
{
    const auto key = "types/" + ids;
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned length = 0;
    if (EVP_Digest(key.data(), key.size(), bytes.data(), &length, EVP_sha256(), nullptr) != 1)
    {
        throw std::runtime_error("百度分类缓存摘要计算失败。");
    }
    std::string digest;
    for (unsigned index = 0; index < length; ++index)
    {
        digest += fmt::format("{:02x}", bytes[index]);
    }
    return digest;
}

BaiduFeatureTypes LoadTypes(const AmapVectorDownloadOptions& options,
                            const AmapVectorDecodedData& decoded)
{
    std::set<uint32_t> unique;
    for (const auto& feature : decoded.features)
    {
        unique.insert(feature.mainKey);
    }
    const std::vector<uint32_t> allIds(unique.begin(), unique.end());
    BaiduFeatureTypes result;
    for (size_t start = 0; start < allIds.size(); start += 64)
    {
        CheckCancellation(options);
        const std::vector<uint32_t> ids(allIds.begin() + start,
                                        allIds.begin() + std::min(start + 64, allIds.size()));
        std::string query;
        for (const auto id : ids)
        {
            query += (query.empty() ? "" : ",") + std::to_string(id);
        }
        const auto cache = options.GetBaiduCachePath() / kCacheVersion / "feature-types" /
                           (ClassificationDigest(query) + ".txt");
        BaiduFeatureTypes batch;
        std::string error;
        if (!options.useRawTileCache ||
            !BaiduVectorClassification::DecodeResponse(ReadCache(cache), ids, batch, error))
        {
            const auto response = Request(
                options,
                "https://api.map.baidu.com/custom/v2/getfeaturetype?callback=baiduFeatureTypes"
                "&feature_ids=" +
                    query);
            if (!BaiduVectorClassification::DecodeResponse(response, ids, batch, error))
            {
                throw std::runtime_error(error);
            }
            CheckCancellation(options);
            if (options.useRawTileCache)
            {
                WriteCache(cache, response);
            }
        }
        result.insert(batch.begin(), batch.end());
    }
    return result;
}

std::vector<std::pair<AmapVectorDecodedData, bool>> DownloadTiles(
    const AmapVectorDownloadOptions& options, const std::vector<BaiduVectorTile>& tiles)
{
    std::vector<std::pair<AmapVectorDecodedData, bool>> responses(tiles.size());
    std::atomic_size_t next = 0;
    std::atomic_bool failed = false;
    std::mutex progressMutex;
    std::string firstError;
    AmapVectorDownloadProgress progress;
    progress.totalTaskCount = tiles.size();
    if (options.progressCallback)
    {
        options.progressCallback(progress);
    }
    const size_t concurrency = std::min<size_t>(
        tiles.size(), std::clamp(options.maxConcurrency == 0 ? std::thread::hardware_concurrency()
                                                             : options.maxConcurrency,
                                 1u, 16u));
    {
        // 所有线程在函数返回前退出；回调串行，与现有 GIS 下载器保持一致。
        std::vector<std::jthread> workers;
        for (size_t worker = 0; worker < concurrency; ++worker)
        {
            workers.emplace_back([&] {
                try
                {
                    while (!failed.load())
                    {
                        CheckCancellation(options);
                        const size_t index = next.fetch_add(1);
                        if (index >= responses.size())
                        {
                            return;
                        }
                        responses[index] = LoadTile(options, tiles[index]);
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
                    firstError = "百度 GIS 下载回调或工作线程发生未知异常。";
                }
            });
        }
    }
    if (failed.load())
    {
        throw std::runtime_error(firstError);
    }
    CheckCancellation(options);
    return responses;
}

GisSpatialRange SpatialRange(const AmapVectorDownloadOptions& options)
{
    auto range = options.GetGisSpatialRange();
    if (range.GetIncludedPolygons().empty())
    {
        if (!options.polygonsWgs84.empty())
        {
            range.SetIncludedPolygons(options.polygonsWgs84);
        }
        else
        {
            const auto bounds =
                CalculateGeographicBounds(options.centerWgs84, options.radiusMeters);
            range.SetIncludedPolygons({{{bounds->west, bounds->south},
                                        {bounds->east, bounds->south},
                                        {bounds->east, bounds->north},
                                        {bounds->west, bounds->north}}});
        }
    }
    return range;
}

std::string FeatureSignature(const AmapVectorFeature& feature)
{
    std::string signature = fmt::format("{}:{}:", feature.mainKey, feature.subKey);
    for (const auto& part : feature.geometryParts)
    {
        signature += "|";
        for (const auto& point : part)
        {
            signature += fmt::format("{:.9f},{:.9f};", point.longitude, point.latitude);
        }
    }
    const auto& properties = feature.GetSourceProperties();
    signature += properties.value("guid", "");
    signature += std::to_string(properties.value("rawValue", uint32_t{0}));
    return signature;
}
} // namespace

std::string DownTool::BaiduVectorDownloader::TileUrl(const BaiduVectorTile& tile)
{
    BaiduVectorTileGrid::Validate(tile);
    const auto parameters =
        fmt::format("x={}&y={}&z={}&styles=pl&textimg=0&v={}&udt={}&json=0", tile.GetX(),
                    tile.GetY(), tile.GetZoom(), BaiduVectorClassification::kVersion,
                    BaiduVectorClassification::kUpdateDate);
    return "https://maponline0.bdimg.com/pvd/?qt=vtile&param=" + EncodeParameters(parameters);
}

std::vector<DownTool::BaiduVectorTile> DownTool::BaiduVectorDownloader::SelectTiles(
    const AmapVectorDownloadOptions& options)
{
    const uint32_t zoom = options.GetBaiduDataZoom();
    (void)BaiduVectorTileGrid::LocalSize(zoom);
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
        throw std::invalid_argument("百度 GIS 下载范围无效。");
    }
    int32_t minX = std::numeric_limits<int32_t>::max();
    int32_t minY = minX;
    int32_t maxX = std::numeric_limits<int32_t>::lowest();
    int32_t maxY = maxX;
    // 百度原生格网与 Web XYZ 不同；沿边采样转换，外扩一块保留边界缓冲。
    for (unsigned index = 0; index <= 16; ++index)
    {
        const double ratio = index / 16.;
        const double lon = bounds->west + (bounds->east - bounds->west) * ratio;
        const double lat = bounds->south + (bounds->north - bounds->south) * ratio;
        for (const LonLat point : {LonLat{lon, bounds->south}, LonLat{lon, bounds->north},
                                   LonLat{bounds->west, lat}, LonLat{bounds->east, lat}})
        {
            const auto tile =
                BaiduVectorTileGrid::LonLatToTile(BaiduDown::WGS84ToBD09(point), zoom);
            minX = std::min(minX, tile.GetX());
            minY = std::min(minY, tile.GetY());
            maxX = std::max(maxX, tile.GetX());
            maxY = std::max(maxY, tile.GetY());
        }
    }
    --minX;
    --minY;
    ++maxX;
    ++maxY;
    const uint64_t count = uint64_t(int64_t(maxX) - minX + 1) * (int64_t(maxY) - minY + 1);
    if (count > 100000)
    {
        throw std::invalid_argument("百度 GIS 下载范围超过 100000 块，请缩小范围后分批导出。");
    }
    std::vector<BaiduVectorTile> tiles;
    tiles.reserve(static_cast<size_t>(count));
    for (int32_t y = minY; y <= maxY; ++y)
    {
        for (int32_t x = minX; x <= maxX; ++x)
        {
            tiles.emplace_back(x, y, zoom);
        }
    }
    return tiles;
}

DownTool::AmapVectorDownloadResult DownTool::BaiduVectorDownloader::Download(
    const AmapVectorDownloadOptions& options) const
{
    AmapVectorDownloadResult result;
    try
    {
        if (!options.includeBaseLayers && !options.includeLabels)
        {
            throw std::invalid_argument("至少需要选择一类百度 GIS 图层。");
        }
        const auto tiles = SelectTiles(options);
        auto responses = DownloadTiles(options, tiles);
        std::unordered_set<std::string> signatures;
        for (auto& [decoded, cached] : responses)
        {
            result.cachedTaskCount += cached ? 1 : 0;
            result.downloadedTaskCount += cached ? 0 : 1;
            for (auto& feature : decoded.features)
            {
                CheckCancellation(options);
                if (feature.geometryParts.empty() ||
                    signatures.insert(FeatureSignature(feature)).second)
                {
                    result.decodedData.features.push_back(std::move(feature));
                }
            }
        }
        const auto types = LoadTypes(options, result.decodedData);
        if (!BaiduVectorClassification::Apply(result.decodedData, types, result.error))
        {
            return result;
        }
        std::erase_if(result.decodedData.features, [&](const AmapVectorFeature& feature) {
            CheckCancellation(options);
            const bool label = feature.geometryType == AmapVectorGeometryType::Point;
            return (label ? !options.includeLabels : !options.includeBaseLayers) ||
                   (options.GetFeatureFilter() && !options.GetFeatureFilter()(feature));
        });
        if (!ApplyGisSpatialFilter(result.decodedData, SpatialRange(options), result.error,
                                   options.cancellationRequested,
                                   options.GetProcessingProgressCallback()))
        {
            return result;
        }
        result.decodedData.status = true;
        result.decodedData.version = std::string("Baidu ") + BaiduVectorClassification::kVersion;
        result.decodedData.packetCount = static_cast<uint32_t>(tiles.size());
        result.downloadedPacketCount = result.decodedData.packetCount;
        result.cachedPacketCount = result.cachedTaskCount;
        result.featureCount = static_cast<uint32_t>(result.decodedData.features.size());
        result.success = true;
    }
    catch (const std::exception& exception)
    {
        result.error = "百度 GIS 下载失败：" + std::string(exception.what());
    }
    catch (...)
    {
        result.error = "百度 GIS 下载或回调发生未知异常。";
    }
    return result;
}
