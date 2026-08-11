#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "curl/curl.h"

#if defined(_WIN32)
#include "windows.h"
#endif

#include "AmapVectorPngDownloader.h"

namespace
{
    struct ApplicationOptions
    {
        DownTool::AmapVectorPngDownloadOptions download;
        std::filesystem::path outputRoot =
            std::filesystem::u8path("cache/高德地图/gis数据/输出");
        std::filesystem::path geoJsonOutputPath;
        uint32_t mergedImageResolution = 16384;
        std::string mergedImageFormat = "png";
        bool writeGeoJson = true;
        bool geoJsonPathOverridden = false;
        bool writePyramid = true;
        bool showHelp = false;
        bool strictWaterCodes = false;
    };

    class CurlGlobalContext
    {
    public:
        CurlGlobalContext()
            : result_(curl_global_init(CURL_GLOBAL_ALL))
        {
        }

        ~CurlGlobalContext()
        {
            if (result_ == CURLE_OK)
            {
                curl_global_cleanup();
            }
        }

        bool IsReady() const
        {
            return result_ == CURLE_OK;
        }

        std::string GetError() const
        {
            return curl_easy_strerror(result_);
        }

    private:
        CURLcode result_ = CURLE_FAILED_INIT;
    };

    std::string ToLowerAscii(std::string text)
    {
        std::transform(
            text.begin(),
            text.end(),
            text.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return text;
    }

    std::string PathToUtf8String(const std::filesystem::path& path)
    {
        const std::u8string utf8Path = path.u8string();
        std::string result;
        result.reserve(utf8Path.size());
        for (const char8_t character : utf8Path)
        {
            result.push_back(static_cast<char>(character));
        }
        return result;
    }

    bool ParseDouble(const std::string& text, double& value)
    {
        try
        {
            size_t parsedLength = 0;
            value = std::stod(text, &parsedLength);
            return parsedLength == text.size();
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool ParseUint32(const std::string& text, uint32_t& value)
    {
        try
        {
            size_t parsedLength = 0;
            const unsigned long long parsedValue = std::stoull(text, &parsedLength);
            if (parsedLength != text.size() || parsedValue > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
            value = static_cast<uint32_t>(parsedValue);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    bool ParseHexByte(const std::string& text, size_t offset, uint8_t& value)
    {
        const auto hexValue = [](char character) -> int
        {
            if (character >= '0' && character <= '9')
            {
                return character - '0';
            }
            if (character >= 'a' && character <= 'f')
            {
                return character - 'a' + 10;
            }
            if (character >= 'A' && character <= 'F')
            {
                return character - 'A' + 10;
            }
            return -1;
        };

        const int high = hexValue(text[offset]);
        const int low = hexValue(text[offset + 1]);
        if (high < 0 || low < 0)
        {
            return false;
        }
        value = static_cast<uint8_t>((high << 4) | low);
        return true;
    }

    bool ParseColor(const std::string& text, DownTool::AmapVectorColor& color)
    {
        const std::string hex = !text.empty() && text.front() == '#' ? text.substr(1) : text;
        if (hex.size() != 6 && hex.size() != 8)
        {
            return false;
        }
        if (!ParseHexByte(hex, 0, color.red) ||
            !ParseHexByte(hex, 2, color.green) ||
            !ParseHexByte(hex, 4, color.blue))
        {
            return false;
        }
        color.alpha = 255;
        return hex.size() == 6 || ParseHexByte(hex, 6, color.alpha);
    }

    bool ParseCategory(
        const std::string& text,
        DownTool::AmapVectorRenderCategory& category)
    {
        static const std::map<std::string, DownTool::AmapVectorRenderCategory> categories = {
            { "all", DownTool::AmapVectorRenderCategory::All },
            { "point", DownTool::AmapVectorRenderCategory::Point },
            { "road", DownTool::AmapVectorRenderCategory::Road },
            { "green", DownTool::AmapVectorRenderCategory::Green },
            { "highway", DownTool::AmapVectorRenderCategory::Highway },
            { "subway", DownTool::AmapVectorRenderCategory::Subway },
            { "transit", DownTool::AmapVectorRenderCategory::Subway },
            { "building", DownTool::AmapVectorRenderCategory::Building },
            { "water", DownTool::AmapVectorRenderCategory::Water },
            { "boundary", DownTool::AmapVectorRenderCategory::Boundary },
            { "line", DownTool::AmapVectorRenderCategory::Boundary },
            { "other", DownTool::AmapVectorRenderCategory::Other }
        };
        const auto iterator = categories.find(ToLowerAscii(text));
        if (iterator == categories.end())
        {
            return false;
        }
        category = iterator->second;
        return true;
    }

    bool ParseCategories(
        const std::string& text,
        std::set<DownTool::AmapVectorRenderCategory>& categories)
    {
        categories.clear();
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t separator = text.find(',', start);
            const std::string item = text.substr(
                start,
                separator == std::string::npos ? std::string::npos : separator - start);
            DownTool::AmapVectorRenderCategory category;
            if (item.empty() || !ParseCategory(item, category))
            {
                return false;
            }
            categories.insert(category);
            if (separator == std::string::npos)
            {
                break;
            }
            start = separator + 1;
        }
        return !categories.empty();
    }

    bool ParseColorOverride(
        const std::string& text,
        DownTool::AmapVectorRenderCategory& category,
        DownTool::AmapVectorColor& color)
    {
        const size_t separator = text.find('=');
        return separator != std::string::npos &&
            ParseCategory(text.substr(0, separator), category) &&
            category != DownTool::AmapVectorRenderCategory::All &&
            ParseColor(text.substr(separator + 1), color);
    }

    bool ParseFeatureCode(const std::string& text, std::pair<uint32_t, uint32_t>& code)
    {
        const size_t separator = text.find(':');
        return separator != std::string::npos &&
            ParseUint32(text.substr(0, separator), code.first) &&
            ParseUint32(text.substr(separator + 1), code.second);
    }

    void PrintDownloadProgress(const DownTool::AmapVectorDownloadProgress& progress)
    {
        std::cout <<
            "\rPBF 下载：" << progress.completedTaskCount << '/' << progress.totalTaskCount <<
            "，缓存 " << progress.cacheHitCount <<
            "，下载 " << progress.downloadedTaskCount <<
            "，失败 " << progress.failedTaskCount << std::flush;
        if (progress.completedTaskCount == progress.totalTaskCount ||
            progress.failedTaskCount != 0)
        {
            std::cout << '\n';
        }
    }

    void PrintRenderProgress(const DownTool::AmapVectorRenderProgress& progress)
    {
        std::cout <<
            "\rPNG 渲染：" << progress.completedTileCount << '/' << progress.totalTileCount <<
            "，失败 " << progress.failedTileCount << std::flush;
        if (progress.completedTileCount == progress.totalTileCount ||
            progress.failedTileCount != 0)
        {
            std::cout << '\n';
        }
    }

    ApplicationOptions MakeDefaultOptions()
    {
        ApplicationOptions options;
        options.download.vectorDownload.centerWgs84 = { 116.41, 39.92 };
        options.download.vectorDownload.radiusMeters = 2000.;
        options.download.vectorDownload.zoom = 16;
        options.download.vectorDownload.includeBaseLayers = true;
        options.download.vectorDownload.includeLabels = false;
        options.download.vectorDownload.progressCallback = PrintDownloadProgress;
        options.download.render.categories = { DownTool::AmapVectorRenderCategory::Water };
        options.download.render.progressCallback = PrintRenderProgress;
        options.download.render.colors[DownTool::AmapVectorRenderCategory::Water] =
            DownTool::GetDefaultAmapVectorRenderColor(
                DownTool::AmapVectorRenderCategory::Water);
        return options;
    }

    bool RequireValue(
        int argumentCount,
        char* arguments[],
        int& index,
        std::string& value,
        std::string& error)
    {
        if (index + 1 >= argumentCount)
        {
            error = std::string("参数缺少值：") + arguments[index];
            return false;
        }
        value = arguments[++index];
        return true;
    }

    bool ParseCommandLine(
        int argumentCount,
        char* arguments[],
        ApplicationOptions& options,
        std::string& error)
    {
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::string argument = arguments[index];
            if (argument == "--help" || argument == "-h")
            {
                options.showHelp = true;
                continue;
            }
            if (argument == "--labels")
            {
                options.download.vectorDownload.includeLabels = true;
                continue;
            }
            if (argument == "--no-geojson")
            {
                options.writeGeoJson = false;
                continue;
            }
            if (argument == "--merged-only")
            {
                options.writePyramid = false;
                continue;
            }
            if (argument == "--render-all-zooms")
            {
                options.download.render.ignoreFeatureZoomRange = true;
                continue;
            }
            if (argument == "--building-height-colors")
            {
                options.download.render.colorBuildingsByHeight = true;
                continue;
            }

            std::string value;
            if (!RequireValue(argumentCount, arguments, index, value, error))
            {
                return false;
            }
            if (argument == "--lon")
            {
                if (!ParseDouble(value, options.download.vectorDownload.centerWgs84.lon))
                {
                    error = "--lon 必须是有效数字。";
                    return false;
                }
            }
            else if (argument == "--lat")
            {
                if (!ParseDouble(value, options.download.vectorDownload.centerWgs84.lat))
                {
                    error = "--lat 必须是有效数字。";
                    return false;
                }
            }
            else if (argument == "--radius")
            {
                if (!ParseDouble(value, options.download.vectorDownload.radiusMeters))
                {
                    error = "--radius 必须是有效数字。";
                    return false;
                }
            }
            else if (argument == "--zoom")
            {
                if (!ParseUint32(value, options.download.vectorDownload.zoom))
                {
                    error = "--zoom 必须是非负整数。";
                    return false;
                }
            }
            else if (argument == "--key")
            {
                options.download.vectorDownload.webApiKey = value;
            }
            else if (argument == "--api-version")
            {
                options.download.vectorDownload.webApiVersion = value;
            }
            else if (argument == "--types")
            {
                if (!ParseCategories(value, options.download.render.categories))
                {
                    error = "--types 包含未知类型。";
                    return false;
                }
            }
            else if (argument == "--color")
            {
                DownTool::AmapVectorRenderCategory category;
                DownTool::AmapVectorColor color;
                if (!ParseColorOverride(value, category, color))
                {
                    error = "--color 格式应为 water=#2DA050 或 water=#2DA050FF。";
                    return false;
                }
                options.download.render.colors[category] = color;
            }
            else if (argument == "--water-code")
            {
                std::pair<uint32_t, uint32_t> code;
                if (!ParseFeatureCode(value, code))
                {
                    error = "--water-code 格式应为 mainKey:subKey。";
                    return false;
                }
                options.strictWaterCodes = true;
                options.download.render.featureCodeCategories[code] =
                    DownTool::AmapVectorRenderCategory::Water;
            }
            else if (argument == "--output")
            {
                options.outputRoot = std::filesystem::u8path(value);
            }
            else if (argument == "--geojson")
            {
                options.writeGeoJson = true;
                options.geoJsonPathOverridden = true;
                options.geoJsonOutputPath = std::filesystem::u8path(value);
            }
            else if (argument == "--raw-cache")
            {
                options.download.vectorDownload.rawTileCachePath =
                    std::filesystem::u8path(value);
            }
            else if (argument == "--png-cache")
            {
                options.download.render.pngTileCachePath = std::filesystem::u8path(value);
            }
            else if (argument == "--concurrency")
            {
                uint32_t concurrency = 0;
                if (!ParseUint32(value, concurrency))
                {
                    error = "--concurrency 必须是非负整数。";
                    return false;
                }
                options.download.vectorDownload.maxConcurrency = concurrency;
                options.download.render.maxConcurrency = concurrency;
            }
            else if (argument == "--merged-resolution")
            {
                if (!ParseUint32(value, options.mergedImageResolution))
                {
                    error = "--merged-resolution 必须是非负整数。";
                    return false;
                }
            }
            else if (argument == "--merged-format")
            {
                options.mergedImageFormat = ToLowerAscii(value);
                if (options.mergedImageFormat == "jpeg")
                {
                    options.mergedImageFormat = "jpg";
                }
                if (options.mergedImageFormat != "png" &&
                    options.mergedImageFormat != "jpg")
                {
                    error = "--merged-format 只支持 png 或 jpg。";
                    return false;
                }
            }
            else
            {
                error = "未知参数：" + argument;
                return false;
            }
        }
        return true;
    }

    void FinalizeOptions(ApplicationOptions& options)
    {
        if (options.strictWaterCodes)
        {
            options.download.render.layerCategories["region"] =
                DownTool::AmapVectorRenderCategory::Other;
        }
        if (options.writeGeoJson)
        {
            if (!options.geoJsonPathOverridden)
            {
                options.geoJsonOutputPath =
                    options.outputRoot / std::filesystem::u8path("数据.geojson");
            }
            options.download.vectorDownload.geoJsonOutputPath = options.geoJsonOutputPath;
        }
    }

    void PrintUsage()
    {
        std::cout <<
            "AmapGisDownload - 下载高德 GIS 矢量数据、渲染 PNG 并合并大图\n\n"
            "默认：北京中心、半径 2000 米、z16、只渲染水体。\n\n"
            "参数：\n"
            "  --lon <经度>                  WGS84 中心经度\n"
            "  --lat <纬度>                  WGS84 中心纬度\n"
            "  --radius <米>                 下载半径\n"
            "  --zoom <层级>                 PNG 显示层级；PBF 按高德规则映射\n"
            "  --types <类型列表>            water,building,road,point,line,transit,other,all\n"
            "  --render-all-zooms           忽略要素 minZoom/maxZoom，强制渲染全部\n"
            "  --building-height-colors     建筑按高度稳定使用不同颜色\n"
            "  --color <类型=#RRGGBB[AA]>    自定义类别颜色，可重复\n"
            "  --water-code <main:sub>       仅将指定分类码视为水体，可重复\n"
            "  --key <Web API Key>           高德 Web API Key\n"
            "  --api-version <版本>          高德 Web JSAPI 版本\n"
            "  --labels                      同时下载 POI 等标注图层\n"
            "  --output <目录>               输出根目录\n"
            "  --geojson <文件>              GeoJSON 输出文件\n"
            "  --no-geojson                  不输出 GeoJSON\n"
            "  --merged-only                只输出合并大图，不生成金字塔目录\n"
            "  --raw-cache <目录>            原始 PBF 缓存目录\n"
            "  --png-cache <目录>            临时 PNG XYZ 目录（每次重写）\n"
            "  --concurrency <数量>          PBF/PNG 并发数，默认按硬件线程数，0 表示自动\n"
            "  --merged-resolution <像素>    合并大图最大边，默认 16384\n"
            "  --merged-format <png|jpg>     合并大图格式，默认 png\n"
            "  -h, --help                    显示帮助\n\n"
            "示例：\n"
            "  AmapGisDownload.exe --types water --zoom 16 --radius 2000\n"
            "  AmapGisDownload.exe --types road,building --color \"road=#F0C641\"\n"
            "  AmapGisDownload.exe --types building --building-height-colors\n"
            "  AmapGisDownload.exe --types all --zoom 14 --render-all-zooms\n"
            "  AmapGisDownload.exe --types water --merged-only --merged-format jpg\n"
            "  AmapGisDownload.exe --types water --water-code 123:4\n";
    }
}

int main(int argumentCount, char* arguments[])
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    ApplicationOptions options = MakeDefaultOptions();
    std::string error;
    if (!ParseCommandLine(argumentCount, arguments, options, error))
    {
        std::cerr << error << "\n\n";
        PrintUsage();
        return 2;
    }
    if (options.showHelp)
    {
        PrintUsage();
        return 0;
    }
    FinalizeOptions(options);

    CurlGlobalContext curlContext;
    if (!curlContext.IsReady())
    {
        std::cerr << "无法初始化 CURL：" << curlContext.GetError() << '\n';
        return 1;
    }

    DownTool::AmapVectorPngDownloader downloader;
    const std::filesystem::path tileOutputRoot = options.writePyramid ?
        options.outputRoot / std::filesystem::u8path("瓦片") :
        options.download.render.pngTileCachePath / std::filesystem::u8path("大图采样");
    std::filesystem::path mergedImagePath =
        options.outputRoot / std::filesystem::u8path("合并大图");
    mergedImagePath.replace_extension(options.mergedImageFormat);
    downloader.SetPostProcessEnabled(options.writePyramid);
    downloader.SetPostProcessOutputPath(PathToUtf8String(tileOutputRoot));
    downloader.SetPostProcessMergedImageEnabled(true);
    downloader.SetPostProcessMergedImageResolution(options.mergedImageResolution);
    downloader.SetPostProcessMergedImageOutputPath(PathToUtf8String(mergedImagePath));
    downloader.SetMaxDownQueue(options.download.render.maxConcurrency);

    const DownTool::AmapVectorPngDownloadResult result =
        downloader.DownloadPngTiles(options.download);
    if (!result.success)
    {
        std::cerr << "高德 GIS 下载或渲染失败：" << result.error << '\n';
        return 1;
    }

    const uint32_t dataZoom = result.vectorDownload.selectedTiles.empty() ?
        DownTool::AmapVectorTileGrid::ResolveDataZoom(options.download.vectorDownload.zoom) :
        result.vectorDownload.selectedTiles.front().zoom;
    std::cout <<
        "高德 GIS 处理完成。\n"
        "显示层级：" << options.download.vectorDownload.zoom << '\n' <<
        "PBF 数据层级：" << dataZoom << '\n' <<
        "要素数量：" << result.vectorDownload.featureCount << '\n' <<
        "PBF 缓存命中：" << result.vectorDownload.cachedTaskCount << '\n' <<
        "PBF 实际下载：" << result.vectorDownload.downloadedTaskCount << '\n' <<
        "新渲染 PNG：" << result.render.renderedTileCount << '\n' <<
        "临时 PNG 目录：" << PathToUtf8String(result.render.pngTileCachePath) << '\n' <<
        "大图输出：" << PathToUtf8String(mergedImagePath) << '\n';
    return 0;
}
