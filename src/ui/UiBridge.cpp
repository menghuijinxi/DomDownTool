#include "UiBridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cpl_json.h"
#include "fmt/format.h"

#include "BaiduDown.h"
#include "DownToolBase.h"
#include "GaoDeDown.h"
#include "TencentDown.h"
#include "TianDiTuDown.h"
#include "TianDiTuMultidateDown.h"

namespace
{
    constexpr uint32_t kDefaultMergedImageResolution = 16384;
    constexpr uint32_t kDefaultFbxTextureSize = 4096;
    constexpr uint32_t kMaximumFbxTextureSize = 16384;

    struct DownloadParameters
    {
        std::string project;
        std::string providerId;
        double longitude = 0.;
        double latitude = 0.;
        double radius = 0.;
        uint32_t zoom = 18;
        uint32_t tileSize = 256;
        std::string outputMode;
        uint32_t mergedImageResolution = kDefaultMergedImageResolution;
        uint32_t fbxTextureSize = kDefaultFbxTextureSize;
    };

    bool isPowerOfTwo(uint32_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    std::string FormatJson(const CPLJSONObject& json)
    {
        return json.Format(CPLJSONObject::PrettyFormat::Plain);
    }

    std::string BuildErrorJson(const std::string& message)
    {
        CPLJSONObject root;
        root.Add("ok", false);
        root.Add("error", message);
        return FormatJson(root);
    }

    std::optional<CPLJSONObject> ParseRequestPayload(const std::string& requestJson,
                                                     std::string& error)
    {
        CPLJSONDocument requestDocument;
        if (!requestDocument.LoadMemory(requestJson))
        {
            error = "无法解析 WebView 请求。";
            return std::nullopt;
        }

        const CPLJSONObject requestRoot = requestDocument.GetRoot();
        if (requestRoot.GetType() != CPLJSONObject::Type::Array)
        {
            error = "WebView 请求参数格式错误。";
            return std::nullopt;
        }

        const CPLJSONArray arguments = requestRoot.ToArray();
        if (arguments.Size() == 0)
        {
            error = "缺少下载参数。";
            return std::nullopt;
        }

        const CPLJSONObject firstArgument = arguments[0];
        if (firstArgument.GetType() == CPLJSONObject::Type::Object)
        {
            return firstArgument;
        }

        if (firstArgument.GetType() != CPLJSONObject::Type::String)
        {
            error = "下载参数必须是 JSON 对象。";
            return std::nullopt;
        }

        CPLJSONDocument payloadDocument;
        if (!payloadDocument.LoadMemory(firstArgument.ToString()))
        {
            error = "无法解析下载参数。";
            return std::nullopt;
        }

        const CPLJSONObject payload = payloadDocument.GetRoot();
        if (payload.GetType() != CPLJSONObject::Type::Object)
        {
            error = "下载参数必须是 JSON 对象。";
            return std::nullopt;
        }
        return payload;
    }

    std::optional<DownloadParameters> ParseDownloadParameters(const std::string& requestJson,
                                                              std::string& error)
    {
        const std::optional<CPLJSONObject> payload = ParseRequestPayload(requestJson, error);
        if (!payload.has_value())
        {
            return std::nullopt;
        }

        DownloadParameters parameters;
        parameters.project = payload->GetString("project", "未命名项目");
        parameters.providerId = payload->GetString("providerId");
        parameters.longitude = payload->GetDouble("longitude");
        parameters.latitude = payload->GetDouble("latitude");
        parameters.radius = payload->GetDouble("radius");
        parameters.zoom = static_cast<uint32_t>(std::max(0, payload->GetInteger("zoom", 18)));
        parameters.tileSize =
            static_cast<uint32_t>(std::max(0, payload->GetInteger("tileSize", 256)));
        parameters.outputMode = payload->GetString("outputMode", "pyramid");
        parameters.mergedImageResolution = static_cast<uint32_t>(
            std::max(0, payload->GetInteger("mergedImageResolution",
                                            static_cast<int>(kDefaultMergedImageResolution))));
        parameters.fbxTextureSize = static_cast<uint32_t>(std::max(
            0, payload->GetInteger("fbxTextureSize", static_cast<int>(kDefaultFbxTextureSize))));

        if (parameters.providerId.empty())
        {
            error = "请选择地图源。";
        }
        else if (!std::isfinite(parameters.longitude) || parameters.longitude < -180. ||
                 parameters.longitude > 180.)
        {
            error = "中心经度必须位于 -180 到 180 之间。";
        }
        else if (!std::isfinite(parameters.latitude) || parameters.latitude < -85. ||
                 parameters.latitude > 85.)
        {
            error = "中心纬度必须位于 -85 到 85 之间。";
        }
        else if (!std::isfinite(parameters.radius) || parameters.radius <= 0.)
        {
            error = "下载半径必须大于 0。";
        }
        else if (parameters.zoom > 30)
        {
            error = "瓦片层级不能高于 30。";
        }
        else if (parameters.tileSize == 0)
        {
            error = "瓦片大小必须大于 0。";
        }
        else if (parameters.outputMode != "pyramid" && parameters.outputMode != "merged" &&
                 parameters.outputMode != "fbx")
        {
            error = "不支持的输出方式。";
        }
        else if (parameters.outputMode == "merged" && parameters.mergedImageResolution == 0)
        {
            error = "大图输出分辨率必须大于 0。";
        }
        else if (parameters.outputMode == "fbx" &&
                 (!isPowerOfTwo(parameters.fbxTextureSize) ||
                  parameters.fbxTextureSize > kMaximumFbxTextureSize ||
                  parameters.fbxTextureSize < parameters.tileSize ||
                  parameters.fbxTextureSize % parameters.tileSize != 0))
        {
            error = "FBX 贴图尺寸必须是瓦片尺寸的整数倍、2 的次幂且不能超过 16384。";
        }
        else
        {
            return parameters;
        }

        return std::nullopt;
    }

    std::unique_ptr<DownTool::DownToolBase> CreateDownloader(const std::string& backend)
    {
        if (backend == "GaoDeDown")
        {
            return std::make_unique<DownTool::GaoDeDown>();
        }
        if (backend == "TencentDown")
        {
            return std::make_unique<DownTool::TencentDown>();
        }
        if (backend == "TianDiTuDown")
        {
            return std::make_unique<DownTool::TianDiTuDown>();
        }
        if (backend == "TianDiTuMultidateDown")
        {
            return std::make_unique<DownTool::TianDiTuMultidateDown>();
        }
        if (backend == "BaiduDown")
        {
            return std::make_unique<DownTool::BaiduDown>();
        }
        return nullptr;
    }

    std::string GetOutputModeName(const std::string& outputMode)
    {
        if (outputMode == "merged")
        {
            return "大图";
        }
        if (outputMode == "fbx")
        {
            return "FBX";
        }
        return "金字塔瓦片";
    }

    std::string getOutputDescription(const std::string& outputMode, uint32_t mergedImageResolution,
                                     uint32_t fbxTextureSize)
    {
        if (outputMode == "merged")
        {
            return fmt::format("大图 / 最大边 {} px", mergedImageResolution);
        }
        if (outputMode == "fbx")
        {
            return fmt::format("FBX / 贴图 {} px", fbxTextureSize);
        }
        return "金字塔瓦片";
    }
} // namespace

DomDownToolUi::UiBridge::UiBridge()
    : mapSources_{
          {"gaode.satellite", "高德卫星", "高德", "cache/高德地图", "GCJ02", "去除 GCJ02 偏移",
           "GaoDeDown",
           "https://uri.amap.com/marker?position={longitude},{latitude}&name=DomDownTool&"
           "src=DomDownTool&coordinate=gps&callnative=0"},
          {"tencent.satellite", "腾讯卫星", "腾讯", "cache/腾讯地图", "GCJ02", "去除 GCJ02 偏移",
           "TencentDown",
           "https://apis.map.qq.com/uri/v1/marker?marker=coord:{latitude},{longitude};"
           "title:DomDownTool&referer=DomDownTool"},
          {"tianditu.satellite", "天地图影像", "天地图", "cache/天地图", "WGS84", "无需坐标纠偏",
           "TianDiTuDown",
           "./map-preview.html?provider=tianditu&longitude={longitude}&latitude={latitude}&"
           "zoom={zoom}"},
          {"tianditu.multidate", "天地图多时相", "天地图", "cache/天地图", "CGCS2000",
           "按子类规则转换到 WGS84", "TianDiTuMultidateDown",
           "./map-preview.html?provider=tianditu&longitude={longitude}&latitude={latitude}&"
           "zoom={zoom}"},
          {"baidu.satellite", "百度卫星", "百度", "cache/百度地图", "BD09", "BD09 转换到 WGS84",
           "BaiduDown",
           "https://api.map.baidu.com/marker?location={latitude},{longitude}&title=DomDownTool&"
           "content=DomDownTool&coord_type=wgs84&output=html&"
           "src=webapp.domdowntool.preview"}}
{
}

std::string DomDownToolUi::UiBridge::GetMapSourcesJson() const
{
    CPLJSONArray groups;
    std::vector<std::string> groupNames;
    for (const MapSource& source : mapSources_)
    {
        if (std::find(groupNames.begin(), groupNames.end(), source.group) == groupNames.end())
        {
            groupNames.push_back(source.group);
        }
    }

    for (const std::string& groupName : groupNames)
    {
        CPLJSONObject groupJson;
        CPLJSONArray sources;
        std::string cacheRoot;
        for (const MapSource& source : mapSources_)
        {
            if (source.group != groupName)
            {
                continue;
            }

            CPLJSONObject sourceJson;
            sourceJson.Add("id", source.id);
            sourceJson.Add("name", source.name);
            sourceJson.Add("coordinate", source.coordinate);
            sourceJson.Add("offsetStrategy", source.offsetStrategy);
            sourceJson.Add("previewUrlTemplate", source.previewUrlTemplate);
            sources.Add(sourceJson);
            cacheRoot = source.cacheRoot;
        }

        groupJson.Add("name", groupName);
        groupJson.Add("cacheRoot", cacheRoot);
        groupJson.Add("sources", sources);
        groups.Add(groupJson);
    }

    CPLJSONObject root;
    root.Add("ok", true);
    root.Add("groups", groups);
    return FormatJson(root);
}

std::string DomDownToolUi::UiBridge::GetQueueSnapshotJson() const
{
    CPLJSONArray jobs;
    for (const DownloadJob& job : jobs_)
    {
        const MapSource* source = FindMapSource(job.providerId);
        if (source == nullptr)
        {
            continue;
        }

        CPLJSONObject jobJson;
        jobJson.Add("id", static_cast<uint64_t>(job.id));
        jobJson.Add("project", job.project);
        jobJson.Add("providerId", job.providerId);
        jobJson.Add("providerName", source->name);
        jobJson.Add("cacheRoot", source->cacheRoot);
        jobJson.Add("coordinate", source->coordinate);
        jobJson.Add("offsetStrategy", source->offsetStrategy);
        jobJson.Add("longitude", job.longitude);
        jobJson.Add("latitude", job.latitude);
        jobJson.Add("radius", job.radius);
        jobJson.Add("zoom", static_cast<int>(job.zoom));
        jobJson.Add("tileSize", static_cast<int>(job.tileSize));
        jobJson.Add("outputMode", job.outputMode);
        jobJson.Add("outputName", GetOutputModeName(job.outputMode));
        jobJson.Add(
            "outputDescription",
            getOutputDescription(job.outputMode, job.mergedImageResolution, job.fbxTextureSize));
        jobJson.Add("mergedImageResolution", static_cast<int>(job.mergedImageResolution));
        jobJson.Add("fbxTextureSize", static_cast<int>(job.fbxTextureSize));
        jobJson.Add("status", job.status);
        jobJson.Add("progress", static_cast<int>(job.progress));
        jobs.Add(jobJson);
    }

    CPLJSONObject root;
    root.Add("ok", true);
    root.Add("jobs", jobs);
    return FormatJson(root);
}

std::string DomDownToolUi::UiBridge::AddDownloadJobJson(const std::string& requestJson)
{
    std::string error;
    const std::optional<DownloadParameters> parameters =
        ParseDownloadParameters(requestJson, error);
    if (!parameters.has_value())
    {
        return BuildErrorJson(error);
    }

    if (FindMapSource(parameters->providerId) == nullptr)
    {
        return BuildErrorJson("找不到指定的地图源。");
    }

    DownloadJob job;
    job.id = nextJobId_++;
    job.project = parameters->project;
    job.providerId = parameters->providerId;
    job.longitude = parameters->longitude;
    job.latitude = parameters->latitude;
    job.radius = parameters->radius;
    job.zoom = parameters->zoom;
    job.tileSize = parameters->tileSize;
    job.outputMode = parameters->outputMode;
    job.mergedImageResolution = parameters->mergedImageResolution;
    job.fbxTextureSize = parameters->fbxTextureSize;
    job.status = "等待中";
    jobs_.insert(jobs_.begin(), job);
    return GetQueueSnapshotJson();
}

std::string DomDownToolUi::UiBridge::EstimatePreviewJson(const std::string& requestJson) const
{
    std::string error;
    const std::optional<DownloadParameters> parameters =
        ParseDownloadParameters(requestJson, error);
    if (!parameters.has_value())
    {
        return BuildErrorJson(error);
    }

    const MapSource* source = FindMapSource(parameters->providerId);
    if (source == nullptr)
    {
        return BuildErrorJson("找不到指定的地图源。");
    }

    std::unique_ptr<DownTool::DownToolBase> downloader = CreateDownloader(source->backend);
    if (downloader == nullptr)
    {
        return BuildErrorJson("地图下载器尚未接入 UI。");
    }

    downloader->SetCenterLonLat(DownTool::LonLat(parameters->longitude, parameters->latitude));
    downloader->SetRadius(parameters->radius);
    downloader->SetZoom(parameters->zoom);
    downloader->SetTileSize(parameters->tileSize);
    downloader->SetCachePath(source->cacheRoot);
    downloader->CalculateDownRange();

    const DownTool::Tile& minTile = downloader->GetMinTile();
    const DownTool::Tile& maxTile = downloader->GetMaxTile();
    const int64_t tileWidth = std::max<int64_t>(0, static_cast<int64_t>(maxTile.X) - minTile.X + 1);
    const int64_t tileHeight =
        std::max<int64_t>(0, static_cast<int64_t>(maxTile.Y) - minTile.Y + 1);
    const uint64_t tileCount = static_cast<uint64_t>(tileWidth) * static_cast<uint64_t>(tileHeight);

    CPLJSONObject minimumTile;
    minimumTile.Add("x", minTile.X);
    minimumTile.Add("y", minTile.Y);
    minimumTile.Add("zoom", static_cast<int>(minTile.Zoom));

    CPLJSONObject maximumTile;
    maximumTile.Add("x", maxTile.X);
    maximumTile.Add("y", maxTile.Y);
    maximumTile.Add("zoom", static_cast<int>(maxTile.Zoom));

    CPLJSONObject root;
    root.Add("ok", true);
    root.Add("providerName", source->name);
    root.Add("cacheRoot", source->cacheRoot);
    root.Add("coordinate", source->coordinate);
    root.Add("offsetStrategy", source->offsetStrategy);
    root.Add("minimumTile", minimumTile);
    root.Add("maximumTile", maximumTile);
    root.Add("tileCount", tileCount);
    root.Add("tileRangeText", fmt::format("Z{} / X {}-{} / Y {}-{}", parameters->zoom, minTile.X,
                                          maxTile.X, minTile.Y, maxTile.Y));
    return FormatJson(root);
}

const DomDownToolUi::UiBridge::MapSource* DomDownToolUi::UiBridge::FindMapSource(
    const std::string& providerId) const
{
    const auto source = std::find_if(mapSources_.begin(), mapSources_.end(),
                                     [&providerId](const MapSource& candidate)
                                     { return candidate.id == providerId; });
    return source == mapSources_.end() ? nullptr : &*source;
}
