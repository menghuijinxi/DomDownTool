#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "curl/curl.h"

#if defined(_WIN32)
#include "windows.h"
#endif

#include "ArcGISDown.h"
#include "BaiduDown.h"
#include "BingDown.h"
#include "DownToolBase.h"
#include "GaoDeDown.h"
#include "GoogleDown.h"
#include "TencentDown.h"
#include "TianDiTuDown.h"

namespace
{
    enum class MapProvider
    {
        GaoDe,
        Baidu,
        Tencent,
        TianDiTu,
        Google,
        Bing,
        ArcGis
    };

    enum class OutputMode
    {
        Raw,
        Merged,
        Pyramid,
        Both,
        Fbx
    };

    struct ApplicationOptions
    {
        MapProvider provider = MapProvider::GaoDe;
        DownTool::GaoDeMapType mapType = DownTool::GaoDeMapType::Satellite;
        DownTool::LonLat centerWgs84 = { 116.41, 39.92 };
        double radiusMeters = 2000.;
        uint32_t zoom = 16;
        uint32_t concurrency = 0;
        std::optional<std::filesystem::path> cachePath;
        std::optional<std::filesystem::path> outputPath;
        std::optional<std::string> proxy;
        OutputMode outputMode = OutputMode::Merged;
        uint32_t mergedImageResolution = 16384;
        std::string mergedImageFormat = "png";
        uint32_t fbxTextureSize = 4096;
        std::string fbxTextureFormat = "png";
        DownTool::FbxPlanetShape fbxShape = DownTool::FbxPlanetShape::Plane;
        uint32_t fbxSubdivision = 64;
        double fbxTerrainSampleRateMeters = 30.;
        uint32_t fbxTerrainMaximumTriangles = 0;
        bool showHelp = false;
        bool dryRun = false;
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

    bool IsPowerOfTwo(uint32_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
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
            if (parsedLength != text.size() ||
                parsedValue > std::numeric_limits<uint32_t>::max())
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

    bool ParseProvider(const std::string& text, MapProvider& provider)
    {
        const std::string normalized = ToLowerAscii(text);
        if (normalized == "gaode" || normalized == "amap")
        {
            provider = MapProvider::GaoDe;
        }
        else if (normalized == "baidu")
        {
            provider = MapProvider::Baidu;
        }
        else if (normalized == "tencent")
        {
            provider = MapProvider::Tencent;
        }
        else if (normalized == "tianditu" || normalized == "tdt")
        {
            provider = MapProvider::TianDiTu;
        }
        else if (normalized == "google")
        {
            provider = MapProvider::Google;
        }
        else if (normalized == "bing")
        {
            provider = MapProvider::Bing;
        }
        else if (normalized == "arcgis")
        {
            provider = MapProvider::ArcGis;
        }
        else
        {
            return false;
        }

        return true;
    }

    bool ParseMapType(const std::string& text, DownTool::GaoDeMapType& mapType)
    {
        const std::string normalized = ToLowerAscii(text);
        if (normalized == "satellite")
        {
            mapType = DownTool::GaoDeMapType::Satellite;
        }
        else if (normalized == "electronic")
        {
            mapType = DownTool::GaoDeMapType::Electronic;
        }
        else if (normalized == "road-network" || normalized == "road")
        {
            mapType = DownTool::GaoDeMapType::RoadNetworkOverlay;
        }
        else
        {
            return false;
        }

        return true;
    }

    bool ParseOutputMode(const std::string& text, OutputMode& outputMode)
    {
        const std::string normalized = ToLowerAscii(text);
        if (normalized == "raw")
        {
            outputMode = OutputMode::Raw;
        }
        else if (normalized == "merged")
        {
            outputMode = OutputMode::Merged;
        }
        else if (normalized == "pyramid")
        {
            outputMode = OutputMode::Pyramid;
        }
        else if (normalized == "both")
        {
            outputMode = OutputMode::Both;
        }
        else if (normalized == "fbx")
        {
            outputMode = OutputMode::Fbx;
        }
        else
        {
            return false;
        }

        return true;
    }

    bool ParseFbxShape(const std::string& text, DownTool::FbxPlanetShape& shape)
    {
        const std::string normalized = ToLowerAscii(text);
        if (normalized == "plane")
        {
            shape = DownTool::FbxPlanetShape::Plane;
        }
        else if (normalized == "sphere")
        {
            shape = DownTool::FbxPlanetShape::Sphere;
        }
        else if (normalized == "both")
        {
            shape = DownTool::FbxPlanetShape::Both;
        }
        else
        {
            return false;
        }

        return true;
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

    bool ValidateOptions(const ApplicationOptions& options, std::string& error)
    {
        if (!std::isfinite(options.centerWgs84.lon) ||
            options.centerWgs84.lon < -180. || options.centerWgs84.lon > 180.)
        {
            error = "中心经度必须位于 -180 到 180 之间。";
        }
        else if (!std::isfinite(options.centerWgs84.lat) ||
            options.centerWgs84.lat < -85. || options.centerWgs84.lat > 85.)
        {
            error = "中心纬度必须位于 -85 到 85 之间。";
        }
        else if (!std::isfinite(options.radiusMeters) || options.radiusMeters <= 0.)
        {
            error = "下载半径必须大于 0。";
        }
        else if (options.zoom > 30)
        {
            error = "瓦片层级不能高于 30。";
        }
        else if (options.provider != MapProvider::GaoDe &&
            options.mapType != DownTool::GaoDeMapType::Satellite)
        {
            error = "--map-type 目前只适用于高德地图。";
        }
        else if ((options.outputMode == OutputMode::Merged ||
            options.outputMode == OutputMode::Both) &&
            options.mergedImageResolution == 0)
        {
            error = "合并大图最大边必须大于 0。";
        }
        else if (options.outputMode == OutputMode::Fbx &&
            (!IsPowerOfTwo(options.fbxTextureSize) ||
                options.fbxTextureSize < 256 || options.fbxTextureSize > 16384))
        {
            error = "FBX 贴图尺寸必须是 256 到 16384 之间的 2 的次幂。";
        }
        else if (options.outputMode == OutputMode::Fbx &&
            options.fbxTextureFormat != "png" && options.fbxTextureFormat != "jpg")
        {
            error = "FBX 贴图格式只支持 png 或 jpg。";
        }
        else if (options.outputMode == OutputMode::Fbx && options.fbxSubdivision == 0)
        {
            error = "FBX 球面细分数必须大于 0。";
        }
        else
        {
            return true;
        }

        return false;
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
            if (argument == "--dry-run")
            {
                options.dryRun = true;
                continue;
            }
            if (argument == "--no-proxy")
            {
                options.proxy = "";
                continue;
            }
            if (argument == "--merged-only")
            {
                options.outputMode = OutputMode::Merged;
                continue;
            }
            if (argument == "--raw-only")
            {
                options.outputMode = OutputMode::Raw;
                continue;
            }

            std::string value;
            if (!RequireValue(argumentCount, arguments, index, value, error))
            {
                return false;
            }

            if (argument == "--provider")
            {
                if (!ParseProvider(value, options.provider))
                {
                    error = "--provider 包含未知地图源。";
                    return false;
                }
            }
            else if (argument == "--map-type")
            {
                if (!ParseMapType(value, options.mapType))
                {
                    error = "--map-type 只支持 satellite、electronic 或 road-network。";
                    return false;
                }
            }
            else if (argument == "--lon")
            {
                if (!ParseDouble(value, options.centerWgs84.lon))
                {
                    error = "--lon 必须是有效数字。";
                    return false;
                }
            }
            else if (argument == "--lat")
            {
                if (!ParseDouble(value, options.centerWgs84.lat))
                {
                    error = "--lat 必须是有效数字。";
                    return false;
                }
            }
            else if (argument == "--radius")
            {
                if (!ParseDouble(value, options.radiusMeters))
                {
                    error = "--radius 必须是有效数字。";
                    return false;
                }
            }
            else if (argument == "--zoom")
            {
                if (!ParseUint32(value, options.zoom))
                {
                    error = "--zoom 必须是非负整数。";
                    return false;
                }
            }
            else if (argument == "--cache")
            {
                options.cachePath = std::filesystem::u8path(value);
            }
            else if (argument == "--output")
            {
                options.outputPath = std::filesystem::u8path(value);
            }
            else if (argument == "--proxy")
            {
                options.proxy = value;
            }
            else if (argument == "--concurrency")
            {
                if (!ParseUint32(value, options.concurrency))
                {
                    error = "--concurrency 必须是非负整数。";
                    return false;
                }
            }
            else if (argument == "--output-mode")
            {
                if (!ParseOutputMode(value, options.outputMode))
                {
                    error = "--output-mode 只支持 raw、merged、pyramid、both 或 fbx。";
                    return false;
                }
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
            else if (argument == "--fbx-texture-size")
            {
                if (!ParseUint32(value, options.fbxTextureSize))
                {
                    error = "--fbx-texture-size 必须是非负整数。";
                    return false;
                }
            }
            else if (argument == "--fbx-texture-format")
            {
                options.fbxTextureFormat = ToLowerAscii(value);
                if (options.fbxTextureFormat == "jpeg")
                {
                    options.fbxTextureFormat = "jpg";
                }
                if (options.fbxTextureFormat != "png" &&
                    options.fbxTextureFormat != "jpg")
                {
                    error = "--fbx-texture-format 只支持 png 或 jpg。";
                    return false;
                }
            }
            else if (argument == "--fbx-shape")
            {
                if (!ParseFbxShape(value, options.fbxShape))
                {
                    error = "--fbx-shape 只支持 plane、sphere 或 both。";
                    return false;
                }
            }
            else if (argument == "--fbx-subdivision")
            {
                if (!ParseUint32(value, options.fbxSubdivision))
                {
                    error = "--fbx-subdivision 必须是非负整数。";
                    return false;
                }
            }
            else if (argument == "--fbx-terrain-max-triangles")
            {
                if (!ParseUint32(value, options.fbxTerrainMaximumTriangles) ||
                    options.fbxTerrainMaximumTriangles == 1)
                {
                    error = "--fbx-terrain-max-triangles 必须是 0（不限）或至少 2。";
                    return false;
                }
            }
            else if (argument == "--fbx-terrain-sample-rate")
            {
                if (!ParseDouble(value, options.fbxTerrainSampleRateMeters) ||
                    options.fbxTerrainSampleRateMeters <= 0.)
                {
                    error = "--fbx-terrain-sample-rate 必须是大于 0 的米数。";
                    return false;
                }
            }
            else
            {
                error = "未知参数：" + argument;
                return false;
            }
        }

        return ValidateOptions(options, error);
    }

    std::unique_ptr<DownTool::DownToolBase> CreateDownloader(
        const ApplicationOptions& options)
    {
        switch (options.provider)
        {
        case MapProvider::GaoDe:
            return std::make_unique<DownTool::GaoDeDown>(options.mapType);
        case MapProvider::Baidu:
            return std::make_unique<DownTool::BaiduDown>();
        case MapProvider::Tencent:
            return std::make_unique<DownTool::TencentDown>();
        case MapProvider::TianDiTu:
            return std::make_unique<DownTool::TianDiTuDown>();
        case MapProvider::Google:
            return std::make_unique<DownTool::GoogleDown>();
        case MapProvider::Bing:
            return std::make_unique<DownTool::BingDown>();
        case MapProvider::ArcGis:
            return std::make_unique<DownTool::ArcGISDown>();
        }

        return nullptr;
    }

    const char* GetProviderName(MapProvider provider)
    {
        switch (provider)
        {
        case MapProvider::GaoDe:
            return "gaode";
        case MapProvider::Baidu:
            return "baidu";
        case MapProvider::Tencent:
            return "tencent";
        case MapProvider::TianDiTu:
            return "tianditu";
        case MapProvider::Google:
            return "google";
        case MapProvider::Bing:
            return "bing";
        case MapProvider::ArcGis:
            return "arcgis";
        }

        return "unknown";
    }

    const char* GetMapTypeName(DownTool::GaoDeMapType mapType)
    {
        switch (mapType)
        {
        case DownTool::GaoDeMapType::Satellite:
            return "satellite";
        case DownTool::GaoDeMapType::Electronic:
            return "electronic";
        case DownTool::GaoDeMapType::RoadNetworkOverlay:
            return "road-network";
        }

        return "unknown";
    }

    const char* GetOutputModeName(OutputMode outputMode)
    {
        switch (outputMode)
        {
        case OutputMode::Raw:
            return "raw";
        case OutputMode::Merged:
            return "merged";
        case OutputMode::Pyramid:
            return "pyramid";
        case OutputMode::Both:
            return "both";
        case OutputMode::Fbx:
            return "fbx";
        }

        return "unknown";
    }

    std::filesystem::path ConfigureDownloader(
        const ApplicationOptions& options,
        DownTool::DownToolBase& downloader)
    {
        downloader.SetCenterLonLat(options.centerWgs84);
        downloader.SetRadius(options.radiusMeters);
        downloader.SetZoom(options.zoom);

        const uint32_t automaticConcurrency =
            std::max(1u, std::thread::hardware_concurrency());
        downloader.SetMaxDownQueue(
            options.concurrency == 0 ? automaticConcurrency : options.concurrency);

        if (options.cachePath.has_value())
        {
            downloader.SetCachePath(PathToUtf8String(options.cachePath.value()));
        }
        if (options.proxy.has_value())
        {
            downloader.SetProxy(options.proxy.value());
        }

        const std::filesystem::path outputRoot = options.outputPath.value_or(
            std::filesystem::u8path(downloader.GetCachePath()) /
            std::filesystem::u8path("Google_墨卡托"));
        downloader.SetPostProcessOutputPath(PathToUtf8String(outputRoot / "XYZ"));
        downloader.SetPostProcessEnabled(
            options.outputMode == OutputMode::Pyramid ||
            options.outputMode == OutputMode::Both);
        downloader.SetPostProcessMergedImageEnabled(
            options.outputMode == OutputMode::Merged ||
            options.outputMode == OutputMode::Both);
        downloader.SetPostProcessFbxEnabled(options.outputMode == OutputMode::Fbx);

        std::filesystem::path mergedImagePath = outputRoot / "merged";
        mergedImagePath.replace_extension(options.mergedImageFormat);
        downloader.SetPostProcessMergedImageResolution(options.mergedImageResolution);
        downloader.SetPostProcessMergedImageOutputPath(
            PathToUtf8String(mergedImagePath));

        downloader.SetFbxOutputPath(PathToUtf8String(outputRoot / "FBX"));
        downloader.SetFbxTextureSize(options.fbxTextureSize);
        downloader.SetFbxTextureFormat(options.fbxTextureFormat);
        downloader.SetFbxShape(options.fbxShape);
        downloader.SetFbxMeshSubdivision(options.fbxSubdivision);
        downloader.SetFbxTerrainSampleRateMeters(options.fbxTerrainSampleRateMeters);
        downloader.SetFbxTerrainMaximumTriangles(options.fbxTerrainMaximumTriangles);
        return outputRoot;
    }

    void PrintConfiguration(
        const ApplicationOptions& options,
        DownTool::DownToolBase& downloader,
        const std::filesystem::path& outputRoot)
    {
        std::cout <<
            "地图下载配置：\n"
            "  地图源：" << GetProviderName(options.provider) << '\n';
        if (options.provider == MapProvider::GaoDe)
        {
            std::cout << "  高德类型：" << GetMapTypeName(options.mapType) << '\n';
        }
        std::cout <<
            "  WGS84 中心：" << options.centerWgs84.lon << ", " <<
                options.centerWgs84.lat << '\n' <<
            "  半径：" << options.radiusMeters << " 米\n" <<
            "  层级：z" << options.zoom << '\n' <<
            "  并发：" << downloader.GetMaxDownQueue() << '\n' <<
            "  原始瓦片缓存：" << downloader.GetCachePath() << '\n' <<
            "  输出模式：" << GetOutputModeName(options.outputMode) << '\n';
        if (options.outputMode != OutputMode::Raw)
        {
            std::cout << "  后处理输出：" << PathToUtf8String(outputRoot) << '\n';
        }
    }

    void PrintUsage()
    {
        std::cout <<
            "DomDownTool - 下载卫星图、电子地图或透明路网并执行后处理\n\n"
            "默认：高德卫星、北京中心、半径 2000 米、z16、只输出合并 PNG 大图。\n\n"
            "常用参数：\n"
            "  --provider <地图源>           gaode,baidu,tencent,tianditu,google,bing,arcgis\n"
            "  --map-type <类型>             高德：satellite,electronic,road-network\n"
            "  --lon <经度>                  WGS84 中心经度\n"
            "  --lat <纬度>                  WGS84 中心纬度\n"
            "  --radius <米>                 下载半径\n"
            "  --zoom <层级>                 下载及输出层级\n"
            "  --output-mode <模式>          raw,merged,pyramid,both,fbx\n"
            "  --merged-only                等同于 --output-mode merged\n"
            "  --raw-only                   等同于 --output-mode raw\n"
            "  --cache <目录>                覆盖原始瓦片缓存目录\n"
            "  --output <目录>               覆盖后处理输出目录\n"
            "  --concurrency <数量>          下载和后处理并发数；0 表示自动\n"
            "  --proxy <地址>                覆盖地图源代理\n"
            "  --no-proxy                   强制直连\n"
            "  --merged-resolution <像素>    合并大图最大边，默认 16384\n"
            "  --merged-format <png|jpg>     合并大图格式，默认 png\n"
            "  --fbx-texture-size <像素>     FBX 贴图边长，默认 4096\n"
            "  --fbx-texture-format <png|jpg> FBX 贴图格式，默认 png\n"
            "  --fbx-shape <形态>            plane,sphere,both\n"
            "  --fbx-subdivision <数量>      球面网格细分数，默认 64\n"
            "  --fbx-terrain-sample-rate <米> 高度采样间距，默认 30\n"
            "  --fbx-terrain-max-triangles <数量> 每个贴图地块的三角面上限，0 不限\n"
            "  --dry-run                    只检查并打印配置，不下载\n"
            "  -h, --help                    显示帮助\n\n"
            "示例：\n"
            "  DomDownTool.exe --provider gaode --lon 116.41 --lat 39.92 "
                "--radius 2000 --zoom 16\n"
            "  DomDownTool.exe --provider baidu --radius 20000 --zoom 16 "
                "--merged-format jpg\n"
            "  DomDownTool.exe --provider gaode --map-type electronic "
                "--output-mode pyramid\n";
    }
}

int main(int argumentCount, char* arguments[])
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    ApplicationOptions options;
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

    std::unique_ptr<DownTool::DownToolBase> downloader = CreateDownloader(options);
    if (!downloader)
    {
        std::cerr << "无法创建地图下载器。\n";
        return 1;
    }

    const std::filesystem::path outputRoot =
        ConfigureDownloader(options, *downloader);
    PrintConfiguration(options, *downloader, outputRoot);
    if (options.dryRun)
    {
        std::cout << "配置检查完成，未执行下载。\n";
        return 0;
    }

    CurlGlobalContext curlContext;
    if (!curlContext.IsReady())
    {
        std::cerr << "无法初始化 CURL：" << curlContext.GetError() << '\n';
        return 1;
    }

    downloader->StartDown();
    return 0;
}
