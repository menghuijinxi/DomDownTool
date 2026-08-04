#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "AmapVectorTileGrid.h"
#include "DataType.h"

namespace DownTool
{
    struct AmapVectorDownloadOptions
    {
        LonLat centerWgs84;
        double radiusMeters = 0.;
        uint32_t zoom = 14;
        std::string webApiKey = "d1f15ad3fdf0c704434eaf23c7c18698";
        // 高德 Web JSAPI 版本；可按服务端变更覆盖，默认值来自已验证的 Web 请求。
        std::string webApiVersion = "26_07_27_00";
        uint32_t containRange = 2;
        bool includeBaseLayers = true;
        bool includeLabels = true;
        std::filesystem::path geoJsonOutputPath;
    };

    struct AmapVectorDownloadResult
    {
        bool success = false;
        std::string error;
        std::vector<AmapVectorTile> selectedTiles;
        uint32_t downloadedPacketCount = 0;
        uint32_t featureCount = 0;
    };

    class AmapVectorDownloader
    {
    public:
        // auth_key 由此方法按每个请求向高德服务申请，调用方无需保存或生成它。
        AmapVectorDownloadResult DownloadGeoJson(const AmapVectorDownloadOptions& options) const;
    };
}
