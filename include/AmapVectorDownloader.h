#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "AmapVectorDecoder.h"
#include "AmapVectorTileGrid.h"
#include "DataType.h"
#include "GeographicPolygon.h"

namespace DownTool
{
    class AmapVectorPngDownloader;

    struct AmapVectorDownloadProgress
    {
        size_t completedTaskCount = 0;
        size_t totalTaskCount = 0;
        size_t cacheHitCount = 0;
        size_t downloadedTaskCount = 0;
        size_t failedTaskCount = 0;
    };

    using AmapVectorDownloadProgressCallback =
        std::function<void(const AmapVectorDownloadProgress&)>;

    struct AmapVectorDownloadOptions
    {
        LonLat centerWgs84;
        double radiusMeters = 0.;
        Wgs84Polygons polygonsWgs84;
        uint32_t zoom = 14;
        std::string webApiKey = "d1f15ad3fdf0c704434eaf23c7c18698";
        // 高德 Web JSAPI 版本；可按服务端变更覆盖，默认值来自已验证的 Web 请求。
        std::string webApiVersion = "26_07_27_00";
        uint32_t containRange = 2;
        bool includeBaseLayers = true;
        bool includeLabels = true;
        // 原始矢量信封与 PNG 切片缓存分离，避免高德专用格网与 Web Mercator XYZ 混用。
        std::filesystem::path rawTileCachePath =
            std::filesystem::u8path("cache/高德地图/gis数据/pbf缓存");
        bool useRawTileCache = true;
        // 0 表示按硬件线程数自动选择；Download 返回前会等待所有工作线程结束。
        uint32_t maxConcurrency = std::thread::hardware_concurrency();
        // 回调由工作线程串行调用；Download 返回后不会再调用。
        AmapVectorDownloadProgressCallback progressCallback;
        // 返回 true 时尽快停止领取新任务。
        std::function<bool()> cancellationRequested;
        std::filesystem::path geoJsonOutputPath;
    };

    struct AmapVectorDownloadResult
    {
        bool success = false;
        std::string error;
        std::vector<AmapVectorTile> selectedTiles;
        uint32_t downloadedPacketCount = 0;
        uint32_t cachedPacketCount = 0;
        uint32_t downloadedTaskCount = 0;
        uint32_t cachedTaskCount = 0;
        uint32_t featureCount = 0;
        AmapVectorDecodedData decodedData;
    };

    class AmapVectorDownloader
    {
        friend class AmapVectorPngDownloader;

        AmapVectorDownloadResult DownloadTiles(
            const AmapVectorDownloadOptions& options,
            std::vector<AmapVectorTile> selectedTiles) const;

    public:
        // auth_key 由此方法按每个请求向高德服务申请，调用方无需保存或生成它。
        AmapVectorDownloadResult Download(const AmapVectorDownloadOptions& options) const;

        // 保持原有 GeoJSON 下载入口兼容；会额外将解码后的数据保存在结果中。
        AmapVectorDownloadResult DownloadGeoJson(const AmapVectorDownloadOptions& options) const;
    };
}
