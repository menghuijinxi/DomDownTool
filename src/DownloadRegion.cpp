#include "DownloadRegion.h"

#include <set>
#include <utility>

#include "nlohmann/json.hpp"

const char* DownTool::DownloadRegionRoleName(DownloadRegionRole role)
{
    switch (role)
    {
    case DownloadRegionRole::Include:
        return "下载区域";
    case DownloadRegionRole::ExcludeGis:
        return "GIS 排除区域";
    case DownloadRegionRole::ExcludeTerrain:
        return "底板排除区域";
    }
    return "未知区域";
}

DownTool::Wgs84Polygons DownTool::GetRegionPolygons(const DownloadRegions& regions,
                                                    DownloadRegionRole role)
{
    Wgs84Polygons polygons;
    for (const DownloadRegion& region : regions)
    {
        if (region.GetEnabled() && region.GetRole() == role)
        {
            polygons.push_back(region.GetVertices());
        }
    }
    return polygons;
}

bool DownTool::ValidateDownloadRegions(const DownloadRegions& regions, std::string& error)
{
    std::set<std::string> ids;
    for (const DownloadRegion& region : regions)
    {
        if (region.GetId().empty() || !ids.insert(region.GetId()).second)
        {
            error = "区域 ID 为空或重复。";
            return false;
        }
        if (!ValidateWgs84Polygons({region.GetVertices()}, error) ||
            !IsSimpleWgs84Ring(region.GetVertices()))
        {
            error = region.GetName() + "：区域需要至少三个不共线的顶点，且边界不能自交。";
            return false;
        }
    }
    return true;
}

nlohmann::json DownTool::DownloadRegionsToJson(const DownloadRegions& regions)
{
    nlohmann::json result = nlohmann::json::array();
    for (const DownloadRegion& region : regions)
    {
        nlohmann::json vertices = nlohmann::json::array();
        for (const LonLat& point : region.GetVertices())
        {
            vertices.push_back({point.lon, point.lat});
        }
        result.push_back(
            {{"id", region.GetId()},
             {"name", region.GetName()},
             {"source", region.GetSource()},
             {"role", region.GetRole() == DownloadRegionRole::Include ? "include" :
                          region.GetRole() == DownloadRegionRole::ExcludeGis ? "exclude-gis" :
                                                                              "exclude-terrain"},
             {"enabled", region.GetEnabled()},
             {"verticesWgs84", std::move(vertices)}});
    }
    return result;
}

bool DownTool::ReadDownloadRegions(const nlohmann::json& value, DownloadRegions& regions,
                                   std::string& error)
{
    if (!value.is_array())
    {
        error = "任务区域列表格式无效。";
        return false;
    }
    DownloadRegions parsed;
    for (const auto& entry : value)
    {
        if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
            !entry.contains("name") || !entry["name"].is_string() || !entry.contains("role") ||
            !entry["role"].is_string() || !entry.contains("enabled") ||
            !entry["enabled"].is_boolean() || !entry.contains("verticesWgs84") ||
            !entry["verticesWgs84"].is_array() ||
            (entry.contains("source") && !entry["source"].is_string()))
        {
            error = "任务区域字段无效。";
            return false;
        }
        const std::string role = entry["role"].get<std::string>();
        if (role != "include" && role != "exclude-gis" && role != "exclude-terrain")
        {
            error = "任务区域类型无效。";
            return false;
        }
        DownloadRegion region;
        region.SetId(entry["id"].get<std::string>());
        region.SetName(entry["name"].get<std::string>());
        region.SetSource(entry.value("source", ""));
        region.SetRole(role == "include" ? DownloadRegionRole::Include :
                       role == "exclude-gis" ? DownloadRegionRole::ExcludeGis :
                                              DownloadRegionRole::ExcludeTerrain);
        region.SetEnabled(entry["enabled"].get<bool>());
        Wgs84Ring vertices;
        for (const auto& vertex : entry["verticesWgs84"])
        {
            if (!vertex.is_array() || vertex.size() != 2 || !vertex[0].is_number() ||
                !vertex[1].is_number())
            {
                error = "任务区域坐标无效。";
                return false;
            }
            vertices.emplace_back(vertex[0].get<double>(), vertex[1].get<double>());
        }
        region.SetVertices(vertices);
        parsed.push_back(std::move(region));
    }
    if (!ValidateDownloadRegions(parsed, error))
    {
        return false;
    }
    regions = std::move(parsed);
    return true;
}

DownTool::DownloadRegions DownTool::MakeDownloadRegions(const Wgs84Polygons& polygons,
                                                        const std::string& source)
{
    DownloadRegions regions;
    for (const Wgs84Ring& polygon : polygons)
    {
        DownloadRegion region;
        region.SetId("region-" + std::to_string(regions.size() + 1));
        region.SetName("下载区 " + std::to_string(regions.size() + 1));
        region.SetSource(source);
        region.SetVertices(polygon);
        regions.push_back(std::move(region));
    }
    return regions;
}
