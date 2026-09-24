#include "preview/ExportTaskManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#include "AmapVectorPngDownloader.h"
#include "CopernicusDem.h"

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include "preview/ExportImageComposer.h"

namespace DomDownToolPreview
{
namespace
{
using Json = nlohmann::json;

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string SanitizeFileName(std::string name)
{
    constexpr std::string_view invalidCharacters = "<>:\"/\\|?*";
    std::replace_if(
        name.begin(), name.end(),
        [invalidCharacters](char character)
        {
            return static_cast<unsigned char>(character) < 32 ||
                invalidCharacters.find(character) != std::string_view::npos;
        },
        '_');
    return name.empty() ? "未命名" : name;
}

std::string NormalizeFormat(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value == "jpg" || value == "jpeg" ? "jpg" : "png";
}

std::string ComponentKindValue(ExportComponentKind kind)
{
    switch (kind)
    {
    case ExportComponentKind::MergedImage:
        return "merged-image";
    case ExportComponentKind::StandardXyz:
        return "standard-xyz";
    case ExportComponentKind::RawTiles:
        return "raw-tiles";
    case ExportComponentKind::BasePlate:
        return "base-plate";
    case ExportComponentKind::BuildingModel:
        return "building-model";
    case ExportComponentKind::GeoJson:
        return "geojson";
    }
    return "merged-image";
}

std::optional<ExportComponentKind> ComponentKindFrom(std::string_view value)
{
    if (value == "merged-image")
    {
        return ExportComponentKind::MergedImage;
    }
    if (value == "standard-xyz")
    {
        return ExportComponentKind::StandardXyz;
    }
    if (value == "raw-tiles")
    {
        return ExportComponentKind::RawTiles;
    }
    if (value == "base-plate")
    {
        return ExportComponentKind::BasePlate;
    }
    if (value == "building-model")
    {
        return ExportComponentKind::BuildingModel;
    }
    if (value == "geojson")
    {
        return ExportComponentKind::GeoJson;
    }
    return std::nullopt;
}

std::string ShapeValue(DownTool::FbxPlanetShape shape)
{
    switch (shape)
    {
    case DownTool::FbxPlanetShape::Sphere:
        return "sphere";
    case DownTool::FbxPlanetShape::Both:
        return "both";
    case DownTool::FbxPlanetShape::Plane:
        return "plane";
    }
    return "plane";
}

DownTool::FbxPlanetShape ShapeFrom(std::string_view value)
{
    if (value == "sphere")
    {
        return DownTool::FbxPlanetShape::Sphere;
    }
    if (value == "both")
    {
        return DownTool::FbxPlanetShape::Both;
    }
    return DownTool::FbxPlanetShape::Plane;
}

std::string ComponentDirectoryName(ExportComponentKind kind)
{
    switch (kind)
    {
    case ExportComponentKind::MergedImage:
        return "大图";
    case ExportComponentKind::StandardXyz:
        return "XYZ";
    case ExportComponentKind::RawTiles:
        return "原始瓦片";
    case ExportComponentKind::BasePlate:
        return "底板模型";
    case ExportComponentKind::BuildingModel:
        return "建筑模型";
    case ExportComponentKind::GeoJson:
        return "GeoJSON";
    }
    return "组件";
}

bool IsImagery(const ExportSourceEntry& source)
{
    return source.kind == ExportSourceKind::Imagery;
}

bool HasImagery(const ExportComponent& component)
{
    return std::any_of(
        component.sources.begin(), component.sources.end(),
        [](const ExportSourceEntry& source) { return IsImagery(source); });
}

bool HasGis(const ExportComponent& component)
{
    return std::any_of(
        component.sources.begin(), component.sources.end(),
        [](const ExportSourceEntry& source)
        {
            return source.kind == ExportSourceKind::Gis;
        });
}

bool HasBuildingSource(const ExportComponent& component)
{
    return std::any_of(
        component.sources.begin(), component.sources.end(),
        [](const ExportSourceEntry& source)
        {
            return source.kind == ExportSourceKind::Gis &&
                source.gisCategory == DownTool::AmapVectorRenderCategory::Building;
        });
}

const ExportContentGroup* FindGroup(
    const ExportConfiguration& configuration, const ExportComponent& component)
{
    for (const ExportContentGroup& group : configuration.groups)
    {
        for (const ExportComponent& item : group.components)
        {
            if (&item == &component)
            {
                return &group;
            }
        }
    }
    return nullptr;
}

const ExportComponent* FindComponent(
    const ExportConfiguration& configuration, std::string_view id)
{
    for (const ExportContentGroup& group : configuration.groups)
    {
        for (const ExportComponent& component : group.components)
        {
            if (component.id == id)
            {
                return &component;
            }
        }
    }
    return nullptr;
}

bool GroupEnabled(const ExportConfiguration& configuration, std::string_view componentId)
{
    for (const ExportContentGroup& group : configuration.groups)
    {
        for (const ExportComponent& component : group.components)
        {
            if (component.id == componentId)
            {
                return group.enabled;
            }
        }
    }
    return false;
}

std::string JsonString(const Json& object, const char* field, const std::string& fallback = {})
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto iterator = object.find(field);
    return iterator != object.end() && iterator->is_string() ? iterator->get<std::string>() :
        fallback;
}

double JsonNumber(const Json& object, const char* field, double fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto iterator = object.find(field);
    return iterator != object.end() && iterator->is_number() ? iterator->get<double>() : fallback;
}

bool JsonBool(const Json& object, const char* field, bool fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto iterator = object.find(field);
    return iterator != object.end() && iterator->is_boolean() ? iterator->get<bool>() : fallback;
}

Json SourceToJson(const ExportSourceEntry& source)
{
    Json json = {
        {"id", source.id},
        {"name", source.name},
        {"kind", source.kind == ExportSourceKind::Gis ? "gis" : "imagery"},
        {"cachePath", PathToUtf8(source.cachePath)}};
    if (source.kind == ExportSourceKind::Gis)
    {
        json["gisCategory"] = static_cast<int>(source.gisCategory);
        json["color"] = {
            {"red", source.color.red},
            {"green", source.color.green},
            {"blue", source.color.blue},
            {"alpha", source.color.alpha}};
    }
    if (!source.multidateYear.empty())
    {
        json["multidateYear"] = source.multidateYear;
    }
    if (!source.multidateKey.empty())
    {
        json["multidateKey"] = source.multidateKey;
    }
    if (!source.multidateGbCode.empty())
    {
        json["multidateGbCode"] = source.multidateGbCode;
    }
    if (!source.multidateRegionName.empty())
    {
        json["multidateRegionName"] = source.multidateRegionName;
    }
    return json;
}

std::optional<ExportSourceEntry> SourceFromJson(const Json& json)
{
    if (!json.is_object() || JsonString(json, "id").empty())
    {
        return std::nullopt;
    }
    ExportSourceEntry source;
    source.id = JsonString(json, "id");
    source.name = JsonString(json, "name");
    source.kind = JsonString(json, "kind") == "gis" ? ExportSourceKind::Gis :
        ExportSourceKind::Imagery;
    source.cachePath = std::filesystem::u8path(JsonString(json, "cachePath"));
    source.gisCategory = static_cast<DownTool::AmapVectorRenderCategory>(
        static_cast<int>(JsonNumber(json, "gisCategory", 2.)));
    const auto color = json.find("color");
    if (color != json.end() && color->is_object())
    {
        source.color.red = static_cast<uint8_t>(JsonNumber(*color, "red", 0.));
        source.color.green = static_cast<uint8_t>(JsonNumber(*color, "green", 0.));
        source.color.blue = static_cast<uint8_t>(JsonNumber(*color, "blue", 0.));
        source.color.alpha = static_cast<uint8_t>(JsonNumber(*color, "alpha", 255.));
    }
    source.multidateYear = JsonString(json, "multidateYear");
    source.multidateKey = JsonString(json, "multidateKey");
    source.multidateGbCode = JsonString(json, "multidateGbCode");
    source.multidateRegionName = JsonString(json, "multidateRegionName");
    return source;
}

std::string WallModeValue(DownTool::AmapBuildingFbxTextureMode mode)
{
    return mode == DownTool::AmapBuildingFbxTextureMode::MergedAtlas ? "merged-atlas" :
        "scattered";
}

std::string RoofModeValue(DownTool::AmapBuildingFbxRoofTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas:
        return "merged-atlas";
    case DownTool::AmapBuildingFbxRoofTextureMode::Satellite:
        return "satellite";
    case DownTool::AmapBuildingFbxRoofTextureMode::Scattered:
        return "scattered";
    }
    return "scattered";
}

Json ComponentToJson(const ExportComponent& component)
{
    Json sources = Json::array();
    for (const ExportSourceEntry& source : component.sources)
    {
        sources.push_back(SourceToJson(source));
    }
    return {
        {"id", component.id},
        {"kind", ComponentKindValue(component.kind)},
        {"sources", std::move(sources)},
        {"imageFormat", NormalizeFormat(component.imageFormat)},
        {"maxEdge", component.maxEdge},
        {"xyzTileSize", component.xyzTileSize},
        {"textureSize", component.textureSize},
        {"textureFormat", NormalizeFormat(component.textureFormat)},
        {"shape", ShapeValue(component.shape)},
        {"exportElevation", component.exportElevation},
        {"elevationSampleMeters", component.elevationSampleMeters},
        {"meshSubdivision", component.meshSubdivision},
        {"wallTexturePath", PathToUtf8(component.wallTexturePath)},
        {"roofTexturePath", PathToUtf8(component.roofTexturePath)},
        {"wallTextureMode", WallModeValue(component.wallTextureMode)},
        {"roofTextureMode", RoofModeValue(component.roofTextureMode)},
        {"atlasImageFormat", component.atlasImageFormat ==
            DownTool::AmapBuildingFbxAtlasImageFormat::Jpg ? "jpg" : "png"},
        {"layerHeightCentimeters", component.layerHeightCentimeters},
        {"maxWallMaterialsPerBucket", component.maxWallMaterialsPerBucket},
        {"textureRandomSeed", component.textureRandomSeed},
        {"followBaseElevation", component.followBaseElevation},
        {"basePlateComponentId", component.basePlateComponentId},
        {"includeBaseLayers", component.includeBaseLayers},
        {"includeLabels", component.includeLabels}};
}

std::optional<ExportComponent> ComponentFromJson(const Json& json)
{
    const std::optional<ExportComponentKind> kind = ComponentKindFrom(JsonString(json, "kind"));
    if (!json.is_object() || JsonString(json, "id").empty() || !kind.has_value())
    {
        return std::nullopt;
    }
    ExportComponent component;
    component.id = JsonString(json, "id");
    component.kind = *kind;
    const auto sources = json.find("sources");
    if (sources != json.end())
    {
        if (!sources->is_array())
        {
            return std::nullopt;
        }
        for (const Json& sourceJson : *sources)
        {
            const std::optional<ExportSourceEntry> source = SourceFromJson(sourceJson);
            if (!source.has_value())
            {
                return std::nullopt;
            }
            component.sources.push_back(*source);
        }
    }
    component.imageFormat = NormalizeFormat(JsonString(json, "imageFormat", "png"));
    component.maxEdge = static_cast<uint32_t>(std::max(0., JsonNumber(json, "maxEdge", 16384.)));
    component.xyzTileSize = static_cast<uint32_t>(
        std::max(0., JsonNumber(json, "xyzTileSize", 256.)));
    component.textureSize = static_cast<uint32_t>(
        std::max(0., JsonNumber(json, "textureSize", 4096.)));
    component.textureFormat = NormalizeFormat(JsonString(json, "textureFormat", "png"));
    component.shape = ShapeFrom(JsonString(json, "shape", "sphere"));
    component.exportElevation = JsonBool(json, "exportElevation", false);
    component.elevationSampleMeters = JsonNumber(json, "elevationSampleMeters", 30.);
    component.meshSubdivision = static_cast<uint32_t>(
        std::max(0., JsonNumber(json, "meshSubdivision", 64.)));
    const std::string wallPath = JsonString(json, "wallTexturePath");
    if (!wallPath.empty())
    {
        component.wallTexturePath = std::filesystem::u8path(wallPath);
    }
    const std::string roofPath = JsonString(json, "roofTexturePath");
    if (!roofPath.empty())
    {
        component.roofTexturePath = std::filesystem::u8path(roofPath);
    }
    const std::string wallMode = JsonString(json, "wallTextureMode");
    if (wallMode == "scattered")
    {
        component.wallTextureMode = DownTool::AmapBuildingFbxTextureMode::Scattered;
    }
    else if (wallMode == "merged-atlas")
    {
        component.wallTextureMode = DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
    }
    const std::string roofMode = JsonString(json, "roofTextureMode");
    if (roofMode == "scattered")
    {
        component.roofTextureMode =
            DownTool::AmapBuildingFbxRoofTextureMode::Scattered;
    }
    else if (roofMode == "merged-atlas")
    {
        component.roofTextureMode =
            DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
    }
    else if (roofMode == "satellite")
    {
        component.roofTextureMode =
            DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
    }
    const std::string atlasImageFormat = JsonString(json, "atlasImageFormat");
    if (atlasImageFormat == "png")
    {
        component.atlasImageFormat = DownTool::AmapBuildingFbxAtlasImageFormat::Png;
    }
    else if (atlasImageFormat == "jpg")
    {
        component.atlasImageFormat = DownTool::AmapBuildingFbxAtlasImageFormat::Jpg;
    }
    component.layerHeightCentimeters = JsonNumber(json, "layerHeightCentimeters", 45.);
    component.maxWallMaterialsPerBucket = static_cast<uint32_t>(
        std::max(1., JsonNumber(json, "maxWallMaterialsPerBucket", 1.)));
    component.textureRandomSeed = static_cast<uint32_t>(
        std::max(0., JsonNumber(json, "textureRandomSeed", 0.)));
    component.followBaseElevation = JsonBool(json, "followBaseElevation", false);
    component.basePlateComponentId = JsonString(json, "basePlateComponentId");
    component.includeBaseLayers = JsonBool(json, "includeBaseLayers", true);
    component.includeLabels = JsonBool(json, "includeLabels", false);
    return component;
}

bool CreateDirectory(const std::filesystem::path& path, std::string& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(path, directoryError);
    if (directoryError)
    {
        error = "无法创建输出目录：" + directoryError.message();
        return false;
    }
    return true;
}

bool MovePathReplacing(
    const std::filesystem::path& from, const std::filesystem::path& to, std::string& error)
{
    std::error_code existsError;
    if (!std::filesystem::exists(from, existsError))
    {
        error = "缺少导出文件：" + PathToUtf8(from);
        return false;
    }
    if (!CreateDirectory(to.parent_path(), error))
    {
        return false;
    }
    std::error_code removeError;
    if (std::filesystem::exists(to))
    {
        if (std::filesystem::is_directory(to))
        {
            std::filesystem::remove_all(to, removeError);
        }
        else
        {
            std::filesystem::remove(to, removeError);
        }
        if (removeError)
        {
            error = "无法覆盖导出文件：" + removeError.message();
            return false;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(from, to, renameError);
    if (renameError)
    {
        error = "无法移动导出文件：" + renameError.message();
        return false;
    }
    return true;
}

SatelliteExportSource ToSatelliteSource(const ExportSourceEntry& source)
{
    SatelliteExportSource satellite;
    satellite.id = source.id;
    satellite.name = source.name;
    satellite.cachePath = source.cachePath;
    satellite.multidateYear = source.multidateYear;
    satellite.multidateKey = source.multidateKey;
    satellite.multidateGbCode = source.multidateGbCode;
    satellite.multidateRegionName = source.multidateRegionName;
    satellite.multidateCookie = source.multidateCookie;
    return satellite;
}

std::set<DownTool::AmapVectorRenderCategory> GisCategories(const ExportComponent& component)
{
    std::set<DownTool::AmapVectorRenderCategory> categories;
    for (const ExportSourceEntry& source : component.sources)
    {
        if (source.kind == ExportSourceKind::Gis)
        {
            categories.insert(source.gisCategory);
        }
    }
    return categories;
}

std::map<DownTool::AmapVectorRenderCategory, DownTool::AmapVectorColor> GisColors(
    const ExportComponent& component)
{
    std::map<DownTool::AmapVectorRenderCategory, DownTool::AmapVectorColor> colors;
    for (const ExportSourceEntry& source : component.sources)
    {
        if (source.kind == ExportSourceKind::Gis)
        {
            colors[source.gisCategory] = source.color;
        }
    }
    return colors;
}

std::string JoinedGisName(const ExportComponent& component)
{
    std::string name;
    for (const ExportSourceEntry& source : component.sources)
    {
        if (source.kind != ExportSourceKind::Gis)
        {
            continue;
        }
        if (!name.empty())
        {
            name += "+";
        }
        name += source.name.empty() ? "GIS" : source.name;
    }
    return SanitizeFileName(name);
}
} // namespace

nlohmann::json ExportGroupsToJson(const std::vector<ExportContentGroup>& groups)
{
    Json json = Json::array();
    for (const ExportContentGroup& group : groups)
    {
        Json components = Json::array();
        for (const ExportComponent& component : group.components)
        {
            components.push_back(ComponentToJson(component));
        }
        json.push_back({
            {"id", group.id},
            {"name", group.name},
            {"enabled", group.enabled},
            {"components", std::move(components)}});
    }
    return json;
}

bool ReadExportGroups(const nlohmann::json& value, std::vector<ExportContentGroup>& groups)
{
    groups.clear();
    if (!value.is_object() || !value.contains("groups"))
    {
        return true;
    }
    const Json& jsonGroups = value.at("groups");
    if (!jsonGroups.is_array())
    {
        return false;
    }
    for (const Json& groupJson : jsonGroups)
    {
        if (!groupJson.is_object())
        {
            return false;
        }
        ExportContentGroup group;
        group.id = JsonString(groupJson, "id");
        group.name = JsonString(groupJson, "name");
        group.enabled = JsonBool(groupJson, "enabled", true);
        const auto components = groupJson.find("components");
        if (components != groupJson.end())
        {
            if (!components->is_array())
            {
                return false;
            }
            for (const Json& componentJson : *components)
            {
                const std::optional<ExportComponent> component = ComponentFromJson(componentJson);
                if (!component.has_value())
                {
                    return false;
                }
                group.components.push_back(*component);
            }
        }
        groups.push_back(std::move(group));
    }
    return true;
}

std::string GroupedExportError(const ExportConfiguration& configuration)
{
    if (configuration.groups.empty())
    {
        return {};
    }
    if (!std::isfinite(configuration.centerWgs84.lon) ||
        !std::isfinite(configuration.centerWgs84.lat) ||
        configuration.centerWgs84.lat < -90. || configuration.centerWgs84.lat > 90. ||
        configuration.centerWgs84.lon < -180. || configuration.centerWgs84.lon > 180.)
    {
        return "导出中心经纬度无效。";
    }
    if (!(configuration.radiusMeters > 0.))
    {
        return "导出半径必须大于 0 米。";
    }
    if (configuration.zoom < 1 || configuration.zoom > 30)
    {
        return "导出层级必须在 1 到 30 之间。";
    }

    std::set<std::string> componentIds;
    bool hasEnabledComponent = false;
    for (const ExportContentGroup& group : configuration.groups)
    {
        if (!group.enabled)
        {
            continue;
        }
        for (const ExportComponent& component : group.components)
        {
            hasEnabledComponent = true;
            if (component.id.empty() || !componentIds.insert(component.id).second)
            {
                return "组件标识无效。";
            }
            for (const ExportSourceEntry& source : component.sources)
            {
                if (source.id.empty())
                {
                    return "图源缺少标识。";
                }
                if (source.id == "tianditu.multidate" && !source.multidateKey.empty() &&
                    (source.multidateYear.empty() || source.multidateCookie.empty() ||
                     (source.multidateGbCode.empty() && source.multidateRegionName.empty())))
                {
                    return "天地图历史时相需要已刷新的时间线、区域和当前登录态。";
                }
            }
            const bool imagery = HasImagery(component);
            const bool gis = HasGis(component);
            switch (component.kind)
            {
            case ExportComponentKind::MergedImage:
            case ExportComponentKind::StandardXyz:
                if (component.sources.empty())
                {
                    return "大图和标准 XYZ 至少需要一个图源。";
                }
                if (component.kind == ExportComponentKind::StandardXyz &&
                    component.xyzTileSize != 256 && component.xyzTileSize != 512 &&
                    component.xyzTileSize != 1024)
                {
                    return "标准 XYZ 分辨率只支持 256、512 或 1024。";
                }
                if (component.kind == ExportComponentKind::MergedImage && component.maxEdge == 0)
                {
                    return "大图最大边无效。";
                }
                break;
            case ExportComponentKind::RawTiles:
                if (!imagery || gis)
                {
                    return "原始瓦片只接受影像图源。";
                }
                break;
            case ExportComponentKind::BasePlate:
                if (component.sources.empty())
                {
                    return "底板模型至少需要一个图源。";
                }
                if (component.textureSize == 0)
                {
                    return "底板贴图尺寸无效。";
                }
                if (!component.exportElevation && component.meshSubdivision == 0)
                {
                    return "没开高程时，底板网格细分必须大于 0。";
                }
                if (component.exportElevation && !(component.elevationSampleMeters > 0.))
                {
                    return "高程采样间距必须大于 0 米。";
                }
                break;
            case ExportComponentKind::BuildingModel:
                if (!HasBuildingSource(component))
                {
                    return "建筑模型的图源必须包含建筑。";
                }
                if (component.layerHeightCentimeters <= 0. ||
                    component.maxWallMaterialsPerBucket == 0)
                {
                    return "建筑模型的楼层高度或墙体材质上限无效。";
                }
                if (component.roofTextureMode ==
                        DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
                {
                    const ExportComponent* basePlate = FindComponent(
                        configuration, component.basePlateComponentId);
                    if (basePlate == nullptr ||
                        basePlate->kind != ExportComponentKind::BasePlate ||
                        !GroupEnabled(configuration, basePlate->id))
                    {
                        return "屋顶卫星图必须引用一个已勾选的底板模型。";
                    }
                    if (!HasImagery(*basePlate))
                    {
                        return "被引用的底板模型必须带卫星贴图。";
                    }
                }
                break;
            case ExportComponentKind::GeoJson:
                if (!gis || imagery)
                {
                    return "GeoJSON 只接受 GIS 图源。";
                }
                break;
            }
        }
    }
    if (!hasEnabledComponent)
    {
        return "至少需要一个已勾选内容组，并且其中有一个组件。";
    }
    return {};
}
bool ExportGroupedRunner::Run(
    ExportTaskManager& manager, const ExportTaskManager::QueuedExport& queuedExport,
    std::string& error)
{
    const std::string validation = GroupedExportError(queuedExport.configuration);
    if (!validation.empty())
    {
        error = validation;
        return false;
    }

    struct Job
    {
        const ExportContentGroup* group = nullptr;
        const ExportComponent* component = nullptr;
    };
    std::set<std::string> referencedBasePlates;
    for (const ExportContentGroup& group : queuedExport.configuration.groups)
    {
        if (!group.enabled)
        {
            continue;
        }
        for (const ExportComponent& component : group.components)
        {
            if (component.kind == ExportComponentKind::BuildingModel &&
                component.roofTextureMode ==
                    DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
            {
                referencedBasePlates.insert(component.basePlateComponentId);
            }
        }
    }

    std::vector<Job> jobs;
    std::set<std::string> pulled;
    const auto appendJobs = [&](bool onlyReferencedBasePlates)
    {
        for (const ExportContentGroup& group : queuedExport.configuration.groups)
        {
            if (!group.enabled)
            {
                continue;
            }
            for (const ExportComponent& component : group.components)
            {
                const bool referenced = component.kind == ExportComponentKind::BasePlate &&
                    referencedBasePlates.contains(component.id);
                if (onlyReferencedBasePlates != referenced || pulled.contains(component.id))
                {
                    continue;
                }
                jobs.push_back({&group, &component});
                pulled.insert(component.id);
            }
        }
    };
    appendJobs(true);
    appendJobs(false);

    bool needsElevation = false;
    for (const Job& job : jobs)
    {
        if ((job.component->kind == ExportComponentKind::BasePlate &&
             job.component->exportElevation) ||
            (job.component->kind == ExportComponentKind::BuildingModel &&
             job.component->followBaseElevation))
        {
            needsElevation = true;
        }
    }

    ExportConfiguration cleanConfiguration = queuedExport.configuration;
    cleanConfiguration.executionRootOverride.clear();
    cleanConfiguration.flatSourceOutput = false;
    const std::filesystem::path taskRoot = ExportTaskOutputRoot(
        cleanConfiguration, queuedExport.taskId);
    const size_t stepCount = std::max<size_t>(1, jobs.size() + (needsElevation ? 1u : 0u));
    size_t stepIndex = 0;
    std::shared_ptr<DownTool::CopernicusDemData> elevationData;
    if (needsElevation)
    {
        DownTool::CopernicusDemOptions elevationOptions;
        elevationOptions.centerWgs84 = queuedExport.configuration.centerWgs84;
        elevationOptions.radiusMeters = queuedExport.configuration.radiusMeters;
        elevationOptions.cancellationRequested = [&manager]
        {
            return manager.IsCancellationRequested();
        };
        elevationOptions.progressCallback =
            [&manager, taskId = queuedExport.taskId, stepIndex, stepCount](
                const DownTool::CopernicusDemProgress& progress)
            {
                const double ratio = progress.total == 0 ? 0. :
                    std::clamp(
                        static_cast<double>(progress.completed) /
                            static_cast<double>(progress.total),
                        0., 1.);
                manager.UpdateProgress(
                    taskId, progress.stage,
                    (static_cast<double>(stepIndex) + ratio) / static_cast<double>(stepCount),
                    progress.completed, progress.total);
            };
        elevationData = DownTool::CopernicusDemData::Prepare(elevationOptions, error);
        if (!elevationData)
        {
            return false;
        }
        if (!CreateDirectory(taskRoot / std::filesystem::u8path("高度"), error))
        {
            return false;
        }
        Json sourceFiles = Json::array();
        for (const std::filesystem::path& sourceFile : elevationData->GetSourceFiles())
        {
            sourceFiles.push_back(PathToUtf8(sourceFile));
        }
        const DownTool::CopernicusDemBounds& bounds = elevationData->GetBounds();
        const Json metadata = {
            {"provider", "Copernicus DEM GLO-30"},
            {"resolutionMeters", 30},
            {"verticalDatum", "EGM2008"},
            {"referenceMode", "minimum"},
            {"referenceElevationMeters", elevationData->GetMinimumElevationMeters()},
            {"bounds", {{"west", bounds.west}, {"east", bounds.east},
                        {"south", bounds.south}, {"north", bounds.north}}},
            {"sourceFiles", std::move(sourceFiles)},
            {"version", 1}};
        std::ofstream metadataOutput(
            taskRoot / std::filesystem::u8path("高度") / "meta.json",
            std::ios::binary | std::ios::trunc);
        if (!metadataOutput.is_open())
        {
            error = "无法创建高度元数据文件。";
            return false;
        }
        metadataOutput << metadata.dump(2);
        ++stepIndex;
    }

    std::map<const ExportContentGroup*, std::filesystem::path> groupDirectories;
    std::set<std::string> usedGroupNames;
    for (const Job& job : jobs)
    {
        if (groupDirectories.contains(job.group))
        {
            continue;
        }
        std::string name = SanitizeFileName(job.group->name);
        std::string unique = name;
        for (int suffix = 2; usedGroupNames.contains(unique); ++suffix)
        {
            unique = fmt::format("{} ({})", name, suffix);
        }
        usedGroupNames.insert(unique);
        groupDirectories.emplace(
            job.group, taskRoot / std::filesystem::u8path(unique));
    }

    const auto failed = [&error](bool succeeded)
    {
        if (!succeeded && error.empty())
        {
            error = "导出任务已取消。";
        }
        return !succeeded;
    };

    for (const Job& job : jobs)
    {
        if (manager.IsCancellationRequested())
        {
            error = "导出任务已取消。";
            return false;
        }
        const ExportComponent& component = *job.component;
        const std::filesystem::path componentDirectory =
            groupDirectories.at(job.group) /
            std::filesystem::u8path(ComponentDirectoryName(component.kind));
        if (!CreateDirectory(componentDirectory, error))
        {
            return false;
        }
        const bool wantsElevation =
            (component.kind == ExportComponentKind::BasePlate && component.exportElevation) ||
            (component.kind == ExportComponentKind::BuildingModel &&
             component.followBaseElevation);
        const std::shared_ptr<DownTool::CopernicusDemData> componentElevation =
            wantsElevation ? elevationData : nullptr;
        const std::string imageFormat = NormalizeFormat(component.imageFormat);
        std::set<std::string> usedSourceNames;

        const auto uniqueSourceName = [&usedSourceNames](std::string name)
        {
            name = SanitizeFileName(std::move(name));
            std::string unique = name;
            for (int suffix = 2; usedSourceNames.contains(unique); ++suffix)
            {
                unique = fmt::format("{} ({})", name, suffix);
            }
            usedSourceNames.insert(unique);
            return unique;
        };
        const auto makeStep = [&](bool tiles, bool merged, bool fbx)
        {
            ExportTaskManager::QueuedExport step = queuedExport;
            step.configuration.executionRootOverride = componentDirectory;
            step.configuration.flatSourceOutput = true;
            step.configuration.exportTiles = tiles;
            step.configuration.exportMergedImage = merged;
            step.configuration.exportFbx = fbx;
            step.configuration.mergedImageFormat = imageFormat;
            step.configuration.mergedImageResolution = component.maxEdge;
            step.configuration.fbxTextureSize = component.textureSize;
            step.configuration.fbxTextureFormat = NormalizeFormat(component.textureFormat);
            step.configuration.fbxTerrainSampleRateMeters = component.elevationSampleMeters;
            step.configuration.executionTileSize = tiles ? component.xyzTileSize : 0;
            step.configuration.executionFbxShape = fbx ?
                std::optional<DownTool::FbxPlanetShape>(component.shape) : std::nullopt;
            step.configuration.executionFbxSubdivision =
                fbx && !component.exportElevation ? component.meshSubdivision : 0;
            return step;
        };
        const auto runSatellite = [&](const ExportSourceEntry& source,
                                      std::string outputName, bool tiles, bool merged,
                                      bool fbx)
        {
            if (source.cachePath.empty())
            {
                error = "图源缺少缓存目录：" + source.name;
                return false;
            }
            ExportSourceEntry named = source;
            named.name = std::move(outputName);
            ExportTaskManager::QueuedExport step = makeStep(tiles, merged, fbx);
            return !failed(manager.RunSatelliteExport(
                step, ToSatelliteSource(named), componentElevation, stepIndex, stepCount,
                error));
        };

        bool succeeded = true;
        if (component.kind == ExportComponentKind::RawTiles)
        {
            std::ofstream manifest(
                componentDirectory / std::filesystem::u8path("来源.txt"),
                std::ios::binary | std::ios::trunc);
            if (!manifest.is_open())
            {
                error = "无法创建原始瓦片说明。";
                return false;
            }
            for (const ExportSourceEntry& source : component.sources)
            {
                const std::string outputName = uniqueSourceName(source.name);
                if (!IsImagery(source) ||
                    !runSatellite(source, outputName, false, false, false))
                {
                    succeeded = false;
                    break;
                }
                const std::filesystem::path sourceDirectory =
                    componentDirectory / std::filesystem::u8path(outputName);
                std::error_code removeError;
                std::filesystem::remove_all(sourceDirectory, removeError);
                manifest << source.name << "\t" << PathToUtf8(source.cachePath) << "\n";
            }
        }
        else if (component.kind == ExportComponentKind::MergedImage ||
                 component.kind == ExportComponentKind::StandardXyz ||
                 component.kind == ExportComponentKind::BasePlate)
        {
            const bool tiles = component.kind == ExportComponentKind::StandardXyz;
            const bool merged = component.kind == ExportComponentKind::MergedImage;
            const bool fbx = component.kind == ExportComponentKind::BasePlate;
            std::vector<std::filesystem::path> imageryOutputs;
            for (const ExportSourceEntry& source : component.sources)
            {
                if (!IsImagery(source))
                {
                    continue;
                }
                const std::string outputName = uniqueSourceName(source.name);
                if (!runSatellite(source, outputName, tiles, merged, fbx))
                {
                    succeeded = false;
                    break;
                }
                const std::filesystem::path sourceDirectory =
                    componentDirectory / std::filesystem::u8path(outputName);
                if (merged)
                {
                    const std::filesystem::path finalImage =
                        componentDirectory / std::filesystem::u8path(outputName + "." + imageFormat);
                    if (!MovePathReplacing(
                            sourceDirectory / ("merged." + imageFormat), finalImage, error))
                    {
                        succeeded = false;
                        break;
                    }
                    std::error_code removeError;
                    std::filesystem::remove_all(sourceDirectory, removeError);
                    imageryOutputs.push_back(finalImage);
                }
                else if (tiles)
                {
                    const std::filesystem::path xyzDirectory = sourceDirectory / "XYZ";
                    std::vector<std::filesystem::path> children;
                    std::error_code iteratorError;
                    for (std::filesystem::directory_iterator iterator(xyzDirectory, iteratorError),
                         end;
                         !iteratorError && iterator != end; iterator.increment(iteratorError))
                    {
                        children.push_back(iterator->path());
                    }
                    for (const std::filesystem::path& child : children)
                    {
                        if (!MovePathReplacing(child, sourceDirectory / child.filename(), error))
                        {
                            succeeded = false;
                            break;
                        }
                    }
                    std::error_code removeError;
                    std::filesystem::remove_all(xyzDirectory, removeError);
                    if (succeeded &&
                        !std::filesystem::exists(sourceDirectory / "meta.json"))
                    {
                        error = "卫星 XYZ 瓦片未生成完整元数据。";
                        succeeded = false;
                    }
                    imageryOutputs.push_back(sourceDirectory);
                }
                else
                {
                    imageryOutputs.push_back(sourceDirectory / "FBX");
                }
                if (!succeeded)
                {
                    break;
                }
            }
            if (succeeded && HasGis(component))
            {
                const std::filesystem::path temporaryRoot =
                    taskRoot / std::filesystem::u8path("临时") /
                    std::filesystem::u8path(SanitizeFileName(component.id));
                const std::filesystem::path gisXyz = temporaryRoot / "XYZ";
                const std::filesystem::path gisImage =
                    temporaryRoot / ("gis." + (merged ? imageFormat : "png"));
                const std::filesystem::path gisFbx = temporaryRoot / "FBX";
                const std::string gisName = uniqueSourceName(JoinedGisName(component));
                const std::filesystem::path directXyz =
                    componentDirectory / std::filesystem::u8path(gisName);
                const std::filesystem::path directImage =
                    componentDirectory / std::filesystem::u8path(gisName + "." + imageFormat);
                const std::filesystem::path directFbx =
                    componentDirectory / std::filesystem::u8path(gisName) / "FBX";
                DownTool::AmapVectorPngDownloadOptions options;
                options.vectorDownload.centerWgs84 = queuedExport.configuration.centerWgs84;
                options.vectorDownload.radiusMeters = queuedExport.configuration.radiusMeters;
                options.vectorDownload.zoom = queuedExport.configuration.zoom;
                options.vectorDownload.includeBaseLayers = true;
                options.vectorDownload.includeLabels = GisCategories(component).contains(
                    DownTool::AmapVectorRenderCategory::Point);
                options.vectorDownload.cancellationRequested = [&manager]
                {
                    return manager.IsCancellationRequested();
                };
                options.render.categories = GisCategories(component);
                options.render.colors = GisColors(component);
                options.render.pngTileCachePath = temporaryRoot / "png";
                options.render.cancellationRequested = options.vectorDownload.cancellationRequested;
                DownTool::AmapVectorPngDownloader downloader;
                downloader.SetCancellationRequestedCallback(
                    [&manager] { return manager.IsCancellationRequested(); });
                downloader.SetPostProcessOutputPath(PathToUtf8(imageryOutputs.empty() && tiles ?
                    directXyz : gisXyz));
                downloader.SetPostProcessEnabled(tiles);
                downloader.SetPostProcessMergedImageEnabled(merged);
                downloader.SetPostProcessMergedImageOutputPath(PathToUtf8(
                    imageryOutputs.empty() && merged ? directImage : gisImage));
                downloader.SetPostProcessMergedImageResolution(component.maxEdge);
                downloader.SetPostProcessFbxEnabled(fbx);
                downloader.SetFbxOutputPath(PathToUtf8(
                    imageryOutputs.empty() && fbx ? directFbx : gisFbx));
                downloader.SetFbxTextureSize(component.textureSize);
                downloader.SetFbxTextureFormat(NormalizeFormat(component.textureFormat));
                downloader.SetFbxTerrainSampleRateMeters(component.elevationSampleMeters);
                if (tiles)
                {
                    downloader.SetTileSize(component.xyzTileSize);
                }
                if (fbx)
                {
                    downloader.SetFbxShape(component.shape);
                    if (!component.exportElevation)
                    {
                        downloader.SetFbxMeshSubdivision(component.meshSubdivision);
                    }
                }
                if (componentElevation)
                {
                    downloader.SetFbxTerrainHeightEnabled(true);
                    downloader.SetFbxElevationReferenceMeters(
                        componentElevation->GetMinimumElevationMeters());
                    downloader.SetFbxElevationSampler(
                        [componentElevation](const DownTool::LonLat& lonLat)
                        {
                            return componentElevation->Sample(lonLat);
                        });
                }
                downloader.SetMaxDownQueue(std::max(1u, std::thread::hardware_concurrency()));
                manager.SetActiveDownloader(&downloader);
                DownTool::AmapVectorPngDownloadResult gisResult;
                try
                {
                    gisResult = downloader.DownloadPngTiles(options);
                }
                catch (...)
                {
                    manager.ClearActiveDownloader();
                    throw;
                }
                manager.ClearActiveDownloader();
                if (manager.IsCancellationRequested())
                {
                    error = "导出任务已取消。";
                    succeeded = false;
                }
                else if (!gisResult.success)
                {
                    error = gisResult.error.empty() ? "GIS 导出失败。" : gisResult.error;
                    succeeded = false;
                }
                else if (!imageryOutputs.empty())
                {
                    const auto cancelled = [&manager] { return manager.IsCancellationRequested(); };
                    for (const std::filesystem::path& imageryOutput : imageryOutputs)
                    {
                        if (merged)
                        {
                            const std::filesystem::path composite =
                                imageryOutput.parent_path() /
                                (imageryOutput.stem().string() + ".overlay" +
                                 imageryOutput.extension().string());
                            if (!ExportImageComposer::CompositeImage(
                                    imageryOutput, gisImage, composite, error, cancelled) ||
                                !MovePathReplacing(composite, imageryOutput, error))
                            {
                                succeeded = false;
                                break;
                            }
                        }
                        else if (tiles)
                        {
                            const std::filesystem::path composite = imageryOutput.string() + ".overlay";
                            if (!ExportImageComposer::CompositeXyzTileTree(
                                    imageryOutput, gisXyz, composite, error, cancelled))
                            {
                                succeeded = false;
                                break;
                            }
                            std::error_code removeError;
                            std::filesystem::remove_all(imageryOutput, removeError);
                            if (!MovePathReplacing(composite, imageryOutput, error))
                            {
                                succeeded = false;
                                break;
                            }
                        }
                        else if (!ExportImageComposer::CompositeTileTree(
                                     imageryOutput, gisFbx, imageryOutput, error, cancelled))
                        {
                            succeeded = false;
                            break;
                        }
                    }
                }
            }
        }
        else if (component.kind == ExportComponentKind::BuildingModel)
        {
            ExportTaskManager::QueuedExport step = queuedExport;
            step.configuration.exportBuildingFbx = true;
            step.configuration.exportFbx = true;
            step.configuration.gisCategories = {
                DownTool::AmapVectorRenderCategory::Building};
            step.configuration.buildingWallTexturePath = component.wallTexturePath;
            step.configuration.buildingRoofTexturePath = component.roofTexturePath;
            step.configuration.buildingFbxWallTextureMode = component.wallTextureMode;
            step.configuration.buildingFbxRoofTextureMode = component.roofTextureMode;
            step.configuration.buildingFbxAtlasImageFormat = component.atlasImageFormat;
            step.configuration.buildingLayerHeightCentimeters = component.layerHeightCentimeters;
            step.configuration.buildingMaxWallMaterialsPerBucket =
                component.maxWallMaterialsPerBucket;
            step.configuration.buildingTextureRandomSeed = component.textureRandomSeed;
            step.configuration.fbxTerrainSampleRateMeters = component.elevationSampleMeters;
            step.configuration.exportTerrainHeight = component.followBaseElevation;
            step.configuration.executionFbxShape = component.shape;
            Json reference;
            bool writeReference = false;
            if (component.roofTextureMode ==
                DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
            {
                const ExportComponent* basePlate = FindComponent(
                    queuedExport.configuration, component.basePlateComponentId);
                const ExportContentGroup* baseGroup = nullptr;
                for (const ExportContentGroup& group : queuedExport.configuration.groups)
                {
                    for (const ExportComponent& item : group.components)
                    {
                        if (basePlate != nullptr && item.id == basePlate->id)
                        {
                            baseGroup = &group;
                        }
                    }
                }
                step.configuration.satelliteSources.clear();
                Json referenceOutputs = Json::array();
                if (basePlate != nullptr)
                {
                    for (const ExportSourceEntry& source : basePlate->sources)
                    {
                        if (IsImagery(source))
                        {
                            SatelliteExportSource satellite = ToSatelliteSource(source);
                            satellite.name = uniqueSourceName(source.name);
                            step.configuration.satelliteSources.push_back(satellite);
                        }
                    }
                }
                step.configuration.executionRootOverride =
                    groupDirectories.at(baseGroup) /
                    std::filesystem::u8path(ComponentDirectoryName(ExportComponentKind::BasePlate));
                step.configuration.flatSourceOutput = true;
                for (const SatelliteExportSource& source :
                     step.configuration.satelliteSources)
                {
                    referenceOutputs.push_back(PathToUtf8(
                        step.configuration.executionRootOverride /
                        std::filesystem::u8path(source.name) /
                        "FBX" / "buildings.fbx"));
                }
                reference = {
                    {"basePlateComponentId", component.basePlateComponentId},
                    {"outputs", std::move(referenceOutputs)}};
                writeReference = true;
            }
            else
            {
                step.configuration.buildingFbxOutputOverride = componentDirectory;
            }
            succeeded = !failed(manager.RunBuildingFbxExport(
                step, componentElevation, stepIndex, stepCount, error));
            if (succeeded && writeReference)
            {
                std::ofstream referenceOutput(
                    componentDirectory / std::filesystem::u8path("引用.json"),
                    std::ios::binary | std::ios::trunc);
                if (!referenceOutput.is_open())
                {
                    error = "无法创建建筑模型引用清单。";
                    return false;
                }
                referenceOutput << reference.dump(2);
            }
        }
        else if (component.kind == ExportComponentKind::GeoJson)
        {
            ExportTaskManager::QueuedExport step = queuedExport;
            step.configuration.gisCategories = GisCategories(component);
            step.configuration.includeBaseLayersOverride = component.includeBaseLayers;
            step.configuration.includeLabelsOverride = component.includeLabels;
            step.configuration.geoJsonOutputFileOverride =
                componentDirectory / std::filesystem::u8path("数据.geojson");
            succeeded = !failed(manager.RunGeoJsonExport(step, stepIndex, stepCount, error));
        }

        if (!succeeded)
        {
            return false;
        }
        if (component.kind == ExportComponentKind::BasePlate && component.exportElevation)
        {
            const std::filesystem::path metadata =
                taskRoot / std::filesystem::u8path("高度") / "meta.json";
            std::error_code copyError;
            std::filesystem::create_directories(
                componentDirectory / std::filesystem::u8path("高度"), copyError);
            std::filesystem::copy_file(
                metadata,
                componentDirectory / std::filesystem::u8path("高度") / "meta.json",
                std::filesystem::copy_options::overwrite_existing, copyError);
        }
        manager.UpdateProgress(
            queuedExport.taskId, "组件导出完成",
            static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount), 1, 1);
        ++stepIndex;
    }
    return true;
}

} // namespace DomDownToolPreview
