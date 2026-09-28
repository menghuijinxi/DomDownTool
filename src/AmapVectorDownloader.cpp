#include "AmapVectorDownloader.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "curl/curl.h"
#include "nlohmann/json.hpp"

#include "AmapVectorDecoder.h"

namespace
{
    using Json = nlohmann::json;

    constexpr const char* kAmapTileAuthorizationEndpoint =
        "https://jsapi.amap.com/web_map/get_tile";
    constexpr const char* kAmapWebOrigin = "https://www.amap.com";
    constexpr const char* kAmapWebReferer = "https://www.amap.com/";

    struct HttpResponse
    {
        long statusCode = 0;
        std::string body;
        std::string contentType;
    };

    size_t WriteResponseBody(void* content, size_t size, size_t count, void* userData)
    {
        std::string* body = static_cast<std::string*>(userData);
        const size_t byteCount = size * count;
        body->append(static_cast<const char*>(content), byteCount);
        return byteCount;
    }

    std::string UrlEncode(const std::string& text)
    {
        constexpr char kHexDigits[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(text.size());
        for (const unsigned char character : text)
        {
            const bool isUnreserved =
                (character >= 'A' && character <= 'Z') ||
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') ||
                character == '-' || character == '_' || character == '.' || character == '~';
            if (isUnreserved)
            {
                encoded.push_back(static_cast<char>(character));
                continue;
            }

            encoded.push_back('%');
            encoded.push_back(kHexDigits[character >> 4]);
            encoded.push_back(kHexDigits[character & 0x0F]);
        }
        return encoded;
    }

    std::string ToLowerAscii(std::string text)
    {
        for (char& character : text)
        {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        return text;
    }

    bool StartsWith(const std::string& value, const std::string& prefix)
    {
        return value.size() >= prefix.size() &&
            value.compare(0, prefix.size(), prefix) == 0;
    }

    bool PerformHttpRequest(
        const std::string& url,
        const std::string* postBody,
        const std::vector<std::string>& headers,
        HttpResponse& response,
        std::string& error)
    {
        CURL* curl = curl_easy_init();
        if (curl == nullptr)
        {
            error = "无法创建高德矢量 HTTP 请求。";
            return false;
        }

        curl_slist* requestHeaders = nullptr;
        for (const std::string& header : headers)
        {
            curl_slist* nextHeaders = curl_slist_append(requestHeaders, header.c_str());
            if (nextHeaders == nullptr)
            {
                curl_slist_free_all(requestHeaders);
                curl_easy_cleanup(curl);
                error = "无法设置高德矢量 HTTP 请求头。";
                return false;
            }
            requestHeaders = nextHeaders;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponseBody);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, requestHeaders);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if (postBody != nullptr)
        {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                static_cast<curl_off_t>(postBody->size()));
        }

        const CURLcode curlResult = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
        char* contentType = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
        if (contentType != nullptr)
        {
            response.contentType = contentType;
        }

        curl_slist_free_all(requestHeaders);
        curl_easy_cleanup(curl);
        if (curlResult != CURLE_OK)
        {
            error = "高德矢量 HTTP 请求失败：" +
                std::string(curl_easy_strerror(curlResult));
            return false;
        }
        return true;
    }

    std::string MakeTileId(const DownTool::AmapVectorTile& tile)
    {
        return std::to_string(tile.x) + "_" + std::to_string(tile.y) + "_" +
            std::to_string(tile.zoom);
    }

    std::filesystem::path GetRawTileCacheFilePath(
        const DownTool::AmapVectorDownloadOptions& options,
        const DownTool::AmapVectorTile& tile,
        uint32_t type)
    {
        const std::filesystem::path layerDirectory = type == 1 ?
            std::filesystem::u8path("基础图层") :
            std::filesystem::u8path("标注图层");
        return options.rawTileCachePath /
            layerDirectory /
            std::to_string(tile.zoom) /
            std::to_string(tile.x) /
            (std::to_string(tile.y) + ".pbf");
    }

    bool ReadRawTileCache(
        const std::filesystem::path& cachePath,
        std::string& envelope,
        std::string& error)
    {
        std::error_code existsError;
        if (!std::filesystem::exists(cachePath, existsError))
        {
            if (existsError)
            {
                error = "无法检查高德矢量缓存：" + existsError.message();
            }
            return false;
        }

        std::ifstream input(cachePath, std::ios::binary);
        if (!input)
        {
            error = "无法读取高德矢量缓存。";
            return false;
        }
        envelope.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        if (input.bad() || envelope.empty())
        {
            error = "高德矢量缓存为空或读取失败。";
            return false;
        }
        return true;
    }

    bool WriteRawTileCache(
        const std::filesystem::path& cachePath,
        const std::string& envelope,
        std::string& error)
    {
        std::error_code createError;
        std::filesystem::create_directories(cachePath.parent_path(), createError);
        if (createError)
        {
            error = "无法创建高德矢量缓存目录：" + createError.message();
            return false;
        }

        static std::atomic_uint64_t temporaryFileSequence = 0;
        const uint64_t temporaryFileId =
            temporaryFileSequence.fetch_add(1, std::memory_order_relaxed);
        std::filesystem::path temporaryPath = cachePath;
        temporaryPath += ".tmp." + std::to_string(temporaryFileId);
        {
            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output)
            {
                error = "无法写入高德矢量缓存。";
                return false;
            }
            output.write(envelope.data(), static_cast<std::streamsize>(envelope.size()));
            if (!output)
            {
                error = "高德矢量缓存写入不完整。";
                return false;
            }
        }

        std::error_code replaceError;
        std::filesystem::rename(temporaryPath, cachePath, replaceError);
        if (replaceError)
        {
            std::filesystem::remove(cachePath, replaceError);
            replaceError.clear();
            std::filesystem::rename(temporaryPath, cachePath, replaceError);
        }
        if (replaceError)
        {
            std::filesystem::remove(temporaryPath, replaceError);
            error = "无法提交高德矢量缓存：" + replaceError.message();
            return false;
        }
        return true;
    }

    bool RequestSignedTileUrl(
        const DownTool::AmapVectorDownloadOptions& options,
        const DownTool::AmapVectorTile& tile,
        uint32_t type,
        std::string& signedUrl,
        std::string& error)
    {
        const std::string tileJson = "[{\"id\":\"" + MakeTileId(tile) +
            "\",\"type\":" + std::to_string(type) +
            ",\"contain_range\":" + std::to_string(options.containRange) + "}]";
        const std::string postBody =
            "version=" + UrlEncode(options.webApiVersion) +
            "&pbf_version=v2&access_oversea=0&data_source=1&multi_lang=0&tiles=" +
            UrlEncode(tileJson);
        const std::string url = std::string(kAmapTileAuthorizationEndpoint) +
            "?key=" + UrlEncode(options.webApiKey);
        const std::vector<std::string> headers = {
            "Content-Type: application/x-www-form-urlencoded;charset=UTF-8",
            std::string("Origin: ") + kAmapWebOrigin,
            std::string("Referer: ") + kAmapWebReferer
        };

        HttpResponse response;
        if (!PerformHttpRequest(url, &postBody, headers, response, error))
        {
            return false;
        }
        if (response.statusCode != 200)
        {
            error = "高德矢量授权服务返回 HTTP " + std::to_string(response.statusCode) + "。";
            return false;
        }

        const Json root = Json::parse(response.body, nullptr, false);
        if (root.is_discarded() || !root.is_object())
        {
            error = "高德矢量授权服务返回了无法解析的数据。";
            return false;
        }

        const auto readString = [&root](const char* fieldName)
        {
            const auto iterator = root.find(fieldName);
            return iterator != root.end() && iterator->is_string()
                ? iterator->get<std::string>()
                : std::string{};
        };
        if (readString("status") != "1")
        {
            std::string information = readString("info");
            if (information.empty())
            {
                information = readString("infocode");
            }
            error = information.empty() ?
                "高德矢量授权请求被拒绝。" : "高德矢量授权请求被拒绝：" + information;
            return false;
        }

        const auto urls = root.find("tile_urls");
        if (urls == root.end() || !urls->is_array() || urls->empty() ||
            !urls->front().is_string())
        {
            error = "高德矢量授权响应没有包含瓦片 URL。";
            return false;
        }
        signedUrl = urls->front().get<std::string>();
        if (signedUrl.empty())
        {
            error = "高德矢量授权响应包含空瓦片 URL。";
            return false;
        }
        return true;
    }

    bool DownloadTileEnvelope(
        const std::string& signedUrl,
        std::string& envelope,
        std::string& error)
    {
        const std::vector<std::string> headers = {
            std::string("Referer: ") + kAmapWebReferer
        };
        HttpResponse response;
        if (!PerformHttpRequest(signedUrl, nullptr, headers, response, error))
        {
            return false;
        }
        if (response.statusCode != 200)
        {
            error = "高德矢量瓦片下载返回 HTTP " + std::to_string(response.statusCode) + "。";
            return false;
        }
        if (!StartsWith(ToLowerAscii(response.contentType), "application/x-protobuf"))
        {
            error = "高德矢量瓦片响应不是 Protobuf 数据。";
            return false;
        }

        envelope = std::move(response.body);
        return true;
    }

    void MergeDecodedData(
        const DownTool::AmapVectorDecodedData& source,
        DownTool::AmapVectorDecodedData& target)
    {
        if (target.version.empty())
        {
            target.version = source.version;
        }
        target.status = target.status || source.status;
        target.packetCount += source.packetCount;
        target.features.insert(
            target.features.end(),
            source.features.begin(),
            source.features.end());
    }

    struct AmapVectorDownloadTask
    {
        DownTool::AmapVectorTile tile;
        uint32_t type = 0;
    };

    struct AmapVectorDownloadTaskResult
    {
        bool success = false;
        bool cacheHit = false;
        std::string error;
        DownTool::AmapVectorDecodedData decodedData;
    };

    size_t GetDownloadWorkerCount(uint32_t configuredCount, size_t taskCount)
    {
        if (taskCount == 0)
        {
            return 0;
        }
        const uint32_t hardwareCount = std::max(1u, std::thread::hardware_concurrency());
        const uint32_t requestedCount = configuredCount == 0 ? hardwareCount :
            std::min(configuredCount, hardwareCount);
        return std::min<size_t>(taskCount, std::max(1u, requestedCount));
    }

    bool DownloadAmapVectorTask(
        const DownTool::AmapVectorDownloadOptions& options,
        const AmapVectorDownloadTask& task,
        AmapVectorDownloadTaskResult& result)
    {
        std::string envelope;
        const std::filesystem::path cachePath =
            GetRawTileCacheFilePath(options, task.tile, task.type);
        bool cacheHit = false;
        std::string cacheError;
        if (options.useRawTileCache && ReadRawTileCache(cachePath, envelope, cacheError))
        {
            cacheHit = true;
        }

        DownTool::AmapVectorDecodedData tileData;
        if (cacheHit && !DownTool::AmapVectorDecoder::DecodeEnvelope(
            envelope, tileData, cacheError))
        {
            cacheHit = false;
            envelope.clear();
        }

        if (!cacheHit)
        {
            std::string signedUrl;
            if (!RequestSignedTileUrl(options, task.tile, task.type, signedUrl, result.error))
            {
                return false;
            }
            if (!DownloadTileEnvelope(signedUrl, envelope, result.error))
            {
                return false;
            }
            if (!DownTool::AmapVectorDecoder::DecodeEnvelope(
                envelope, tileData, result.error))
            {
                return false;
            }
            if (options.useRawTileCache &&
                !WriteRawTileCache(cachePath, envelope, result.error))
            {
                return false;
            }
        }

        result.cacheHit = cacheHit;
        result.decodedData = std::move(tileData);
        result.success = true;
        return true;
    }

    bool ValidateDownloadOptions(
        const DownTool::AmapVectorDownloadOptions& options,
        std::string& error)
    {
        if (options.webApiKey.empty())
        {
            error = "下载高德矢量数据需要 Web API key。";
            return false;
        }
        if (options.webApiVersion.empty())
        {
            error = "下载高德矢量数据需要 Web JSAPI 版本。";
            return false;
        }
        if (!options.includeBaseLayers && !options.includeLabels)
        {
            error = "至少需要选择一类高德矢量图层。";
            return false;
        }
        if (options.containRange == 0 || options.containRange > 16)
        {
            error = "高德矢量 contain_range 必须介于 1 和 16 之间。";
            return false;
        }
        return true;
    }
}

DownTool::AmapVectorDownloadResult DownTool::AmapVectorDownloader::Download(
    const AmapVectorDownloadOptions& options) const
{
    AmapVectorDownloadResult result;
    if (!ValidateDownloadOptions(options, result.error))
    {
        return result;
    }

    std::vector<AmapVectorTile> selectedTiles;
    try
    {
        const uint32_t dataZoom = AmapVectorTileGrid::ResolveDataZoom(options.zoom);
        selectedTiles = options.polygonsWgs84.empty() ?
            AmapVectorTileGrid::SelectTilesForWgs84Radius(
                options.centerWgs84,
                options.radiusMeters,
                dataZoom) :
            AmapVectorTileGrid::SelectTilesForWgs84Polygons(
                options.polygonsWgs84,
                dataZoom);
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
        return result;
    }

    return DownloadTiles(options, std::move(selectedTiles));
}

DownTool::AmapVectorDownloadResult DownTool::AmapVectorDownloader::DownloadTiles(
    const AmapVectorDownloadOptions& options,
    std::vector<AmapVectorTile> selectedTiles) const
{
    AmapVectorDownloadResult result;
    if (!ValidateDownloadOptions(options, result.error))
    {
        return result;
    }
    if (selectedTiles.empty())
    {
        result.error = "高德 GIS 下载范围没有包含矢量瓦片。";
        return result;
    }
    result.selectedTiles = std::move(selectedTiles);

    std::vector<uint32_t> tileTypes;
    if (options.includeBaseLayers)
    {
        tileTypes.push_back(1);
    }
    if (options.includeLabels)
    {
        tileTypes.push_back(2);
    }

    std::vector<AmapVectorDownloadTask> tasks;
    tasks.reserve(result.selectedTiles.size() * tileTypes.size());
    for (const AmapVectorTile& tile : result.selectedTiles)
    {
        if (options.cancellationRequested && options.cancellationRequested())
        {
            result.error = "高德 GIS 矢量任务已取消。";
            return result;
        }
        for (const uint32_t type : tileTypes)
        {
            tasks.push_back({ tile, type });
        }
    }

    std::vector<AmapVectorDownloadTaskResult> taskResults(tasks.size());
    std::atomic_size_t nextTaskIndex = 0;
    std::atomic_bool cancellationRequested = false;
    std::mutex progressMutex;
    std::mutex errorMutex;
    size_t completedTaskCount = 0;
    size_t cacheHitCount = 0;
    size_t downloadedTaskCount = 0;
    size_t failedTaskCount = 0;
    std::string firstError;

    const auto setFirstError = [&](std::string error)
    {
        cancellationRequested.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> errorLock(errorMutex);
        if (firstError.empty())
        {
            firstError = std::move(error);
        }
    };

    const auto reportProgress = [&](bool cacheHit, bool downloaded, bool failed)
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        ++completedTaskCount;
        cacheHitCount += cacheHit ? 1 : 0;
        downloadedTaskCount += downloaded ? 1 : 0;
        failedTaskCount += failed ? 1 : 0;
        if (options.progressCallback)
        {
            try
            {
                options.progressCallback({
                    completedTaskCount,
                    tasks.size(),
                    cacheHitCount,
                    downloadedTaskCount,
                    failedTaskCount });
            }
            catch (const std::exception& exception)
            {
                setFirstError("高德 GIS 下载进度回调异常：" + std::string(exception.what()));
            }
            catch (...)
            {
                setFirstError("高德 GIS 下载进度回调发生未知异常。");
            }
        }
    };

    if (options.progressCallback)
    {
        try
        {
            options.progressCallback({ 0, tasks.size(), 0, 0, 0 });
        }
        catch (const std::exception& exception)
        {
            setFirstError("高德 GIS 下载进度回调异常：" + std::string(exception.what()));
        }
        catch (...)
        {
            setFirstError("高德 GIS 下载进度回调发生未知异常。");
        }
    }

    if (cancellationRequested.load(std::memory_order_relaxed))
    {
        result.error = firstError;
        return result;
    }

    const size_t workerCount = GetDownloadWorkerCount(options.maxConcurrency, tasks.size());
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    try
    {
        for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
        {
            workers.emplace_back(
                [&]()
                {
                    while (!cancellationRequested.load(std::memory_order_relaxed) &&
                           (!options.cancellationRequested ||
                            !options.cancellationRequested()))
                    {
                        const size_t taskIndex =
                            nextTaskIndex.fetch_add(1, std::memory_order_relaxed);
                        if (taskIndex >= tasks.size())
                        {
                            return;
                        }

                        AmapVectorDownloadTaskResult& taskResult = taskResults[taskIndex];
                        const bool success = DownloadAmapVectorTask(
                            options, tasks[taskIndex], taskResult);
                        if (!success)
                        {
                            setFirstError(taskResult.error);
                        }
                        reportProgress(
                            taskResult.cacheHit,
                            success && !taskResult.cacheHit,
                            !success);
                    }
                });
        }
    }
    catch (const std::exception& exception)
    {
        setFirstError("无法创建高德 GIS 下载线程：" + std::string(exception.what()));
    }
    catch (...)
    {
        setFirstError("无法创建高德 GIS 下载线程。");
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    if (options.cancellationRequested && options.cancellationRequested())
    {
        result.error = "高德 GIS 矢量任务已取消。";
        return result;
    }

    if (cancellationRequested.load(std::memory_order_relaxed))
    {
        result.error = firstError.empty() ? "高德 GIS 矢量任务失败。" : firstError;
        return result;
    }

    AmapVectorDecodedData decodedData;
    for (const AmapVectorDownloadTaskResult& taskResult : taskResults)
    {
        if (options.cancellationRequested && options.cancellationRequested())
        {
            result.error = "高德 GIS 矢量任务已取消。";
            return result;
        }
        if (!taskResult.success)
        {
            result.error = taskResult.error.empty() ? "高德 GIS 矢量任务失败。" :
                taskResult.error;
            return result;
        }
        if (taskResult.cacheHit)
        {
            result.cachedPacketCount += taskResult.decodedData.packetCount;
            ++result.cachedTaskCount;
        }
        else
        {
            ++result.downloadedTaskCount;
        }
        MergeDecodedData(taskResult.decodedData, decodedData);
    }

    result.downloadedPacketCount = decodedData.packetCount;
    result.featureCount = static_cast<uint32_t>(decodedData.features.size());
    result.decodedData = std::move(decodedData);
    result.success = true;
    return result;
}

DownTool::AmapVectorDownloadResult DownTool::AmapVectorDownloader::DownloadGeoJson(
    const AmapVectorDownloadOptions& options) const
{
    AmapVectorDownloadResult result;
    if (options.geoJsonOutputPath.empty())
    {
        result.error = "下载高德矢量数据需要 GeoJSON 输出路径。";
        return result;
    }

    result = Download(options);
    if (!result.success)
    {
        return result;
    }
    if (!AmapVectorGeoJsonWriter::WriteFile(
        result.decodedData, options.geoJsonOutputPath, result.error,
        options.cancellationRequested))
    {
        result.success = false;
    }
    return result;
}
