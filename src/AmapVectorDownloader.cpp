#include "AmapVectorDownloader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "cpl_json.h"
#include "curl/curl.h"

#include "AmapVectorDecoder.h"

namespace
{
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

        CPLJSONDocument document;
        if (!document.LoadMemory(response.body))
        {
            error = "高德矢量授权服务返回了无法解析的数据。";
            return false;
        }
        const CPLJSONObject root = document.GetRoot();
        if (root.GetString("status") != "1")
        {
            const std::string information = root.GetString("info", root.GetString("infocode"));
            error = information.empty() ?
                "高德矢量授权请求被拒绝。" : "高德矢量授权请求被拒绝：" + information;
            return false;
        }

        const CPLJSONArray urls = root.GetArray("tile_urls");
        if (urls.Size() == 0 || urls[0].GetType() != CPLJSONObject::Type::String)
        {
            error = "高德矢量授权响应没有包含瓦片 URL。";
            return false;
        }
        signedUrl = urls[0].ToString();
        if (signedUrl.empty())
        {
            error = "高德矢量授权响应包含空瓦片 URL。";
            return false;
        }
        return true;
    }

    bool DownloadTileEnvelope(const std::string& signedUrl, std::string& envelope, std::string& error)
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
}

DownTool::AmapVectorDownloadResult DownTool::AmapVectorDownloader::DownloadGeoJson(
    const AmapVectorDownloadOptions& options) const
{
    AmapVectorDownloadResult result;
    if (options.webApiKey.empty())
    {
        result.error = "下载高德矢量数据需要 Web API key。";
        return result;
    }
    if (options.webApiVersion.empty())
    {
        result.error = "下载高德矢量数据需要 Web JSAPI 版本。";
        return result;
    }
    if (options.geoJsonOutputPath.empty())
    {
        result.error = "下载高德矢量数据需要 GeoJSON 输出路径。";
        return result;
    }
    if (!options.includeBaseLayers && !options.includeLabels)
    {
        result.error = "至少需要选择一类高德矢量图层。";
        return result;
    }
    if (options.containRange == 0 || options.containRange > 16)
    {
        result.error = "高德矢量 contain_range 必须介于 1 和 16 之间。";
        return result;
    }

    try
    {
        result.selectedTiles = AmapVectorTileGrid::SelectTilesForWgs84Radius(
            options.centerWgs84,
            options.radiusMeters,
            options.zoom);
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
        return result;
    }

    std::vector<uint32_t> tileTypes;
    if (options.includeBaseLayers)
    {
        tileTypes.push_back(1);
    }
    if (options.includeLabels)
    {
        tileTypes.push_back(2);
    }

    AmapVectorDecodedData decodedData;
    for (const AmapVectorTile& tile : result.selectedTiles)
    {
        for (const uint32_t type : tileTypes)
        {
            std::string signedUrl;
            if (!RequestSignedTileUrl(options, tile, type, signedUrl, result.error))
            {
                return result;
            }

            std::string envelope;
            if (!DownloadTileEnvelope(signedUrl, envelope, result.error))
            {
                return result;
            }

            AmapVectorDecodedData tileData;
            if (!AmapVectorDecoder::DecodeEnvelope(envelope, tileData, result.error))
            {
                return result;
            }
            MergeDecodedData(tileData, decodedData);
        }
    }

    if (!AmapVectorGeoJsonWriter::WriteFile(
        decodedData, options.geoJsonOutputPath, result.error))
    {
        return result;
    }

    result.downloadedPacketCount = decodedData.packetCount;
    result.featureCount = static_cast<uint32_t>(decodedData.features.size());
    result.success = true;
    return result;
}
