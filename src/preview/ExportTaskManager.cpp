#include "preview/ExportTaskManager.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "ArcGISDown.h"
#include "AmapBuildingFbxExporter.h"
#include "AmapVectorDownloader.h"
#include "AmapVectorShapefileWriter.h"
#include "BaiduDown.h"
#include "BingDown.h"
#include "CopernicusDem.h"
#include "DownToolBase.h"
#include "GaoDeDown.h"
#include "GoogleDown.h"
#include "TencentDown.h"
#include "TianDiTuDown.h"
#include "TianDiTuMultidateDown.h"

#include "fmt/format.h"
#include "nlohmann/json.hpp"

#include "preview/ExportImageComposer.h"

namespace DomDownToolPreview
{
nlohmann::json ExportGroupsToJson(const std::vector<ExportContentGroup>& groups);
bool ReadExportGroups(const nlohmann::json& value, std::vector<ExportContentGroup>& groups);
} // namespace DomDownToolPreview

namespace
{
using Json = nlohmann::json;
using DomDownToolPreview::ExportConfiguration;
using DomDownToolPreview::ExportTaskRecord;
using DomDownToolPreview::ExportTaskState;
using DomDownToolPreview::GisExportMode;
using DomDownToolPreview::SatelliteExportSource;
using DomDownToolPreview::ExportComponent;
using DomDownToolPreview::ExportComponentKind;
using DomDownToolPreview::ExportContentGroup;
using DomDownToolPreview::ExportRangeMode;
using DomDownToolPreview::ExportSourceEntry;
using DomDownToolPreview::ExportSourceKind;

constexpr auto kProgressNotificationInterval = std::chrono::milliseconds(100);

std::string NormalizeImageFormat(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    if (value == "jpeg")
    {
        return "jpg";
    }
    return value == "jpg" ? "jpg" : "png";
}

std::string BuildingFbxTextureModeValue(DownTool::AmapBuildingFbxTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxTextureMode::Scattered:
        return "scattered";
    case DownTool::AmapBuildingFbxTextureMode::MergedAtlas:
        return "merged-atlas";
    }
    return "scattered";
}

std::string BuildingFbxTextureModeName(DownTool::AmapBuildingFbxTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxTextureMode::Scattered:
        return "散图";
    case DownTool::AmapBuildingFbxTextureMode::MergedAtlas:
        return "合并贴图";
    }
    return "散图";
}

std::string BuildingFbxRoofTextureModeValue(DownTool::AmapBuildingFbxRoofTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxRoofTextureMode::Scattered:
        return "scattered";
    case DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas:
        return "merged-atlas";
    case DownTool::AmapBuildingFbxRoofTextureMode::Satellite:
        return "satellite";
    }
    return "scattered";
}

std::string BuildingFbxRoofTextureModeName(DownTool::AmapBuildingFbxRoofTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxRoofTextureMode::Scattered:
        return "散图";
    case DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas:
        return "合并贴图";
    case DownTool::AmapBuildingFbxRoofTextureMode::Satellite:
        return "卫星图";
    }
    return "散图";
}

DownTool::AmapBuildingFbxTextureMode BuildingFbxTextureModeFromValue(
    std::string_view value)
{
    if (value == "merged-atlas" || value == "合并贴图")
    {
        return DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
    }
    return DownTool::AmapBuildingFbxTextureMode::Scattered;
}

DownTool::AmapBuildingFbxRoofTextureMode BuildingFbxRoofTextureModeFromValue(
    std::string_view value)
{
    if (value == "merged-atlas" || value == "合并贴图")
    {
        return DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
    }
    if (value == "satellite" || value == "卫星图")
    {
        return DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
    }
    return DownTool::AmapBuildingFbxRoofTextureMode::Scattered;
}

std::string BuildingFbxModeName(const ExportConfiguration& configuration)
{
    return "建筑 FBX（墙体" +
        BuildingFbxTextureModeName(configuration.buildingFbxWallTextureMode) +
        " + 屋顶" +
        BuildingFbxRoofTextureModeName(configuration.buildingFbxRoofTextureMode) +
        "）";
}

std::string BuildingFbxAtlasImageFormatValue(
    DownTool::AmapBuildingFbxAtlasImageFormat format)
{
    switch (format)
    {
    case DownTool::AmapBuildingFbxAtlasImageFormat::Png:
        return "png";
    case DownTool::AmapBuildingFbxAtlasImageFormat::Jpg:
        return "jpg";
    }
    return "png";
}

std::string BuildingFbxAtlasImageFormatName(
    DownTool::AmapBuildingFbxAtlasImageFormat format)
{
    switch (format)
    {
    case DownTool::AmapBuildingFbxAtlasImageFormat::Png:
        return "PNG";
    case DownTool::AmapBuildingFbxAtlasImageFormat::Jpg:
        return "JPG";
    }
    return "PNG";
}

DownTool::AmapBuildingFbxAtlasImageFormat BuildingFbxAtlasImageFormatFromValue(
    std::string_view value)
{
    if (value == "jpg" || value == "jpeg" || value == "JPG" || value == "JPEG")
    {
        return DownTool::AmapBuildingFbxAtlasImageFormat::Jpg;
    }
    return DownTool::AmapBuildingFbxAtlasImageFormat::Png;
}

bool UsesSatelliteRoofTexture(const ExportConfiguration& configuration)
{
    return configuration.buildingFbxRoofTextureMode ==
        DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
}

std::string GetJsonString(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return {};
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_string()
        ? iterator->get<std::string>()
        : std::string{};
}

std::string PathToUtf8(const std::filesystem::path& path);

double GetJsonDouble(
    const Json& object, const char* fieldName, double defaultValue = 0.)
{
    if (!object.is_object())
    {
        return defaultValue;
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_number()
        ? iterator->get<double>()
        : defaultValue;
}

bool GetJsonBool(const Json& object, const char* fieldName, bool defaultValue = false)
{
    if (!object.is_object())
    {
        return defaultValue;
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_boolean()
        ? iterator->get<bool>()
        : defaultValue;
}

Json ExportConfigurationToJson(const ExportConfiguration& configuration)
{
    Json satelliteSources = Json::array();
    for (const SatelliteExportSource& source : configuration.satelliteSources)
    {
        Json sourceJson = {
            {"id", source.id},
            {"name", source.name},
            {"cachePath", PathToUtf8(source.cachePath)}};
        if (!source.multidateYear.empty())
        {
            sourceJson["multidateYear"] = source.multidateYear;
        }
        if (!source.multidateKey.empty())
        {
            sourceJson["multidateKey"] = source.multidateKey;
        }
        if (!source.multidateGbCode.empty())
        {
            sourceJson["multidateGbCode"] = source.multidateGbCode;
        }
        if (!source.multidateRegionName.empty())
        {
            sourceJson["multidateRegionName"] = source.multidateRegionName;
        }
        satelliteSources.push_back(std::move(sourceJson));
    }

    Json gisCategories = Json::array();
    for (const DownTool::AmapVectorRenderCategory category : configuration.gisCategories)
    {
        gisCategories.push_back(static_cast<int>(category));
    }

    Json gisModes = Json::array();
    for (const GisExportMode mode : configuration.gisModes)
    {
        gisModes.push_back(static_cast<int>(mode));
    }

    Json gisColors = Json::object();
    for (const auto& [category, color] : configuration.gisColors)
    {
        gisColors[std::to_string(static_cast<int>(category))] = {
            {"red", color.red},
            {"green", color.green},
            {"blue", color.blue},
            {"alpha", color.alpha}};
    }

    Json polygonsWgs84 = Json::array();
    for (const DownTool::Wgs84Ring& ring : configuration.polygonsWgs84)
    {
        Json ringJson = Json::array();
        for (const DownTool::LonLat& coordinate : ring)
        {
            ringJson.push_back({coordinate.lon, coordinate.lat});
        }
        polygonsWgs84.push_back(std::move(ringJson));
    }

    Json json = {
        {"rangeMode", configuration.rangeMode == ExportRangeMode::KmlPolygon ? "custom-polygon"
                                                                             : "center-radius"},
        {"centerWgs84",
         {{"lon", configuration.centerWgs84.lon}, {"lat", configuration.centerWgs84.lat}}},
        {"radiusMeters", configuration.radiusMeters},
        {"polygonsWgs84", std::move(polygonsWgs84)},
        {"downloadRegions", DownTool::DownloadRegionsToJson(configuration.GetDownloadRegions())},
        {"kmlFileName", configuration.kmlFileName},
        {"zoom", configuration.zoom},
        {"satelliteSources", std::move(satelliteSources)},
        {"gisCategories", std::move(gisCategories)},
        {"gisModes", std::move(gisModes)},
        {"gisColors", std::move(gisColors)},
        {"outputRoot", PathToUtf8(configuration.outputRoot)},
        {"exportTiles", configuration.exportTiles},
        {"exportMergedImage", configuration.exportMergedImage},
        {"exportFbx", configuration.exportFbx},
        {"exportTerrainHeight", configuration.exportTerrainHeight},
        {"exportGeoJson", configuration.exportGeoJson},
        {"colorBuildingsByHeight", configuration.colorBuildingsByHeight},
        {"exportBuildingFbx", configuration.exportBuildingFbx},
        {"buildingWallTexturePath", PathToUtf8(configuration.buildingWallTexturePath)},
        {"buildingRoofTexturePath", PathToUtf8(configuration.buildingRoofTexturePath)},
        {"buildingFbxWallTextureMode",
         BuildingFbxTextureModeValue(configuration.buildingFbxWallTextureMode)},
        {"buildingFbxRoofTextureMode",
         BuildingFbxRoofTextureModeValue(configuration.buildingFbxRoofTextureMode)},
        {"buildingFbxAtlasImageFormat",
         BuildingFbxAtlasImageFormatValue(configuration.buildingFbxAtlasImageFormat)},
        {"buildingLayerHeightCentimeters", configuration.buildingLayerHeightCentimeters},
        {"buildingChunkSizeMeters", configuration.GetBuildingChunkSizeMeters()},
        {"buildingMaxWallMaterialsPerBucket", configuration.buildingMaxWallMaterialsPerBucket},
        {"buildingTextureRandomSeed", configuration.buildingTextureRandomSeed},
        {"mergedImageResolution", configuration.mergedImageResolution},
        {"mergedImageFormat", configuration.mergedImageFormat},
        {"fbxTextureSize", configuration.fbxTextureSize},
        {"fbxTextureFormat", NormalizeImageFormat(configuration.fbxTextureFormat)},
        {"fbxTerrainSampleRateMeters", configuration.fbxTerrainSampleRateMeters},
        {"terrainMaximumTriangles", configuration.GetTerrainMaximumTriangles()},
        {"fbxPerTile", configuration.GetFbxPerTile()},
        {"distanceLod", DownTool::FbxDistanceLodToJson(configuration.GetDistanceLod())}};
    if (!configuration.groups.empty())
    {
        json["groups"] = DomDownToolPreview::ExportGroupsToJson(configuration.groups);
    }
    return json;
}

std::optional<ExportConfiguration> ExportConfigurationFromJson(const Json& value)
{
    if (!value.is_object())
    {
        return std::nullopt;
    }
    const auto center = value.find("centerWgs84");
    if (center == value.end() || !center->is_object() ||
        !center->contains("lon") || !center->contains("lat") ||
        !center->at("lon").is_number() || !center->at("lat").is_number())
    {
        return std::nullopt;
    }

    ExportConfiguration configuration;
    configuration.rangeMode =
        (GetJsonString(value, "rangeMode") == "kml-polygon" ||
         GetJsonString(value, "rangeMode") == "custom-polygon") ?
            ExportRangeMode::KmlPolygon : ExportRangeMode::CenterRadius;
    configuration.centerWgs84.lon = center->at("lon").get<double>();
    configuration.centerWgs84.lat = center->at("lat").get<double>();
    configuration.radiusMeters = GetJsonDouble(value, "radiusMeters");
    configuration.kmlFileName = GetJsonString(value, "kmlFileName");
    const auto polygons = value.find("polygonsWgs84");
    if (polygons != value.end())
    {
        if (!polygons->is_array())
        {
            return std::nullopt;
        }
        for (const Json& ringJson : *polygons)
        {
            if (!ringJson.is_array())
            {
                return std::nullopt;
            }
            DownTool::Wgs84Ring ring;
            for (const Json& coordinateJson : ringJson)
            {
                if (!coordinateJson.is_array() || coordinateJson.size() < 2 ||
                    !coordinateJson[0].is_number() ||
                    !coordinateJson[1].is_number())
                {
                    return std::nullopt;
                }
                ring.push_back({
                    coordinateJson[0].get<double>(),
                    coordinateJson[1].get<double>()
                });
            }
            configuration.polygonsWgs84.push_back(std::move(ring));
        }
    }
    if (const auto regions = value.find("downloadRegions"); regions != value.end())
    {
        DownTool::DownloadRegions parsed;
        std::string regionError;
        if (!DownTool::ReadDownloadRegions(*regions, parsed, regionError))
        {
            return std::nullopt;
        }
        configuration.SetDownloadRegions(parsed);
        if (!parsed.empty() && configuration.rangeMode == ExportRangeMode::CustomPolygon)
        {
            configuration.polygonsWgs84 = DownTool::GetRegionPolygons(
                parsed, DownTool::DownloadRegionRole::Include);
        }
    }
    if (configuration.GetDownloadRegions().empty() && !configuration.polygonsWgs84.empty())
    {
        configuration.SetDownloadRegions(DownTool::MakeDownloadRegions(
            configuration.polygonsWgs84, configuration.kmlFileName));
    }
    if (configuration.rangeMode == ExportRangeMode::KmlPolygon)
    {
        std::string polygonError;
        if (!DownTool::ValidateWgs84Polygons(
                configuration.polygonsWgs84, polygonError))
        {
            return std::nullopt;
        }
    }
    configuration.zoom = static_cast<uint32_t>(
        std::max(0., GetJsonDouble(value, "zoom")));
    configuration.outputRoot = std::filesystem::u8path(
        GetJsonString(value, "outputRoot"));
    configuration.exportTiles = GetJsonBool(value, "exportTiles");
    configuration.exportMergedImage = GetJsonBool(value, "exportMergedImage");
    configuration.exportFbx = GetJsonBool(value, "exportFbx");
    configuration.SetFbxPerTile(GetJsonBool(value, "fbxPerTile"));
    configuration.exportTerrainHeight = GetJsonBool(value, "exportTerrainHeight");
    configuration.exportGeoJson = GetJsonBool(value, "exportGeoJson");
    configuration.colorBuildingsByHeight = GetJsonBool(value, "colorBuildingsByHeight");
    configuration.exportBuildingFbx = GetJsonBool(value, "exportBuildingFbx", false);
    const std::string wallTexturePath = GetJsonString(value, "buildingWallTexturePath");
    if (!wallTexturePath.empty())
    {
        configuration.buildingWallTexturePath = std::filesystem::u8path(wallTexturePath);
    }
    const std::string roofTexturePath = GetJsonString(value, "buildingRoofTexturePath");
    if (!roofTexturePath.empty())
    {
        configuration.buildingRoofTexturePath = std::filesystem::u8path(roofTexturePath);
    }
    const std::string legacyBuildingTextureMode = GetJsonString(
        value, "buildingFbxTextureMode");
    const std::string wallTextureMode = GetJsonString(
        value, "buildingFbxWallTextureMode");
    const std::string roofTextureMode = GetJsonString(
        value, "buildingFbxRoofTextureMode");
    const std::string resolvedWallTextureMode =
        wallTextureMode.empty() ? legacyBuildingTextureMode : wallTextureMode;
    if (!resolvedWallTextureMode.empty())
    {
        configuration.buildingFbxWallTextureMode = BuildingFbxTextureModeFromValue(
            resolvedWallTextureMode);
    }
    const std::string resolvedRoofTextureMode =
        roofTextureMode.empty() ? legacyBuildingTextureMode : roofTextureMode;
    if (!resolvedRoofTextureMode.empty())
    {
        configuration.buildingFbxRoofTextureMode = BuildingFbxRoofTextureModeFromValue(
            resolvedRoofTextureMode);
    }
    const std::string atlasImageFormat =
        GetJsonString(value, "buildingFbxAtlasImageFormat");
    if (!atlasImageFormat.empty())
    {
        configuration.buildingFbxAtlasImageFormat =
            BuildingFbxAtlasImageFormatFromValue(atlasImageFormat);
    }
    configuration.buildingLayerHeightCentimeters = GetJsonDouble(
        value, "buildingLayerHeightCentimeters", 45.);
    configuration.SetBuildingChunkSizeMeters(GetJsonDouble(
        value, "buildingChunkSizeMeters", configuration.GetBuildingChunkSizeMeters()));
    configuration.buildingMaxWallMaterialsPerBucket = static_cast<uint32_t>(
        std::max(1., GetJsonDouble(value, "buildingMaxWallMaterialsPerBucket", 1.)));
    configuration.buildingTextureRandomSeed = static_cast<uint32_t>(
        std::max(0., GetJsonDouble(value, "buildingTextureRandomSeed", 0.)));
    configuration.mergedImageResolution = static_cast<uint32_t>(
        std::max(0., GetJsonDouble(value, "mergedImageResolution")));
    configuration.mergedImageFormat = GetJsonString(value, "mergedImageFormat");
    if (configuration.mergedImageFormat != "jpg" &&
        configuration.mergedImageFormat != "png")
    {
        configuration.mergedImageFormat = "png";
    }
    configuration.fbxTextureSize = static_cast<uint32_t>(
        std::max(0., GetJsonDouble(value, "fbxTextureSize")));
    configuration.fbxTextureFormat = NormalizeImageFormat(
        GetJsonString(value, "fbxTextureFormat"));
    configuration.fbxTerrainSampleRateMeters =
        GetJsonDouble(value, "fbxTerrainSampleRateMeters", 30.);
    configuration.SetTerrainMaximumTriangles(static_cast<uint32_t>(std::clamp(
        GetJsonDouble(value, "terrainMaximumTriangles", 0.), 0.,
        double(std::numeric_limits<uint32_t>::max()))));
    if (const auto lodJson = value.find("distanceLod"); lodJson != value.end())
    {
        const auto settings = DownTool::FbxDistanceLodFromJson(*lodJson);
        if (!settings)
        {
            return std::nullopt;
        }
        configuration.SetDistanceLod(*settings);
    }

    const auto sources = value.find("satelliteSources");
    if (sources != value.end() && sources->is_array())
    {
        for (const Json& source : *sources)
        {
            if (!source.is_object() || GetJsonString(source, "id").empty())
            {
                return std::nullopt;
            }
            SatelliteExportSource exportSource{
                GetJsonString(source, "id"),
                GetJsonString(source, "name"),
                std::filesystem::u8path(GetJsonString(source, "cachePath"))};
            exportSource.multidateYear = GetJsonString(source, "multidateYear");
            exportSource.multidateKey = GetJsonString(source, "multidateKey");
            exportSource.multidateGbCode = GetJsonString(source, "multidateGbCode");
            exportSource.multidateRegionName = GetJsonString(source, "multidateRegionName");
            configuration.satelliteSources.push_back(std::move(exportSource));
        }
    }

    const auto categories = value.find("gisCategories");
    if (categories != value.end() && categories->is_array())
    {
        for (const Json& category : *categories)
        {
            if (!category.is_number_integer())
            {
                return std::nullopt;
            }
            const int value = category.get<int>();
            if (value < static_cast<int>(DownTool::AmapVectorRenderCategory::All) ||
                value > static_cast<int>(DownTool::AmapVectorRenderCategory::Other))
            {
                return std::nullopt;
            }
            configuration.gisCategories.insert(
                static_cast<DownTool::AmapVectorRenderCategory>(value));
        }
    }

    const auto modes = value.find("gisModes");
    if (modes != value.end() && modes->is_array())
    {
        for (const Json& mode : *modes)
        {
            if (!mode.is_number_integer())
            {
                return std::nullopt;
            }
            const int value = mode.get<int>();
            if (value < static_cast<int>(GisExportMode::OverlayOnSatellite) ||
                value > static_cast<int>(GisExportMode::SeparateLayers))
            {
                return std::nullopt;
            }
            configuration.gisModes.insert(static_cast<GisExportMode>(value));
        }
    }

    const auto colors = value.find("gisColors");
    if (colors != value.end() && colors->is_object())
    {
        for (const auto& [key, color] : colors->items())
        {
            if (!color.is_object())
            {
                return std::nullopt;
            }
            int category = 0;
            const auto parseResult = std::from_chars(
                key.data(), key.data() + key.size(), category);
            if (parseResult.ec != std::errc{} || parseResult.ptr != key.data() + key.size() ||
                category < static_cast<int>(DownTool::AmapVectorRenderCategory::All) ||
                category > static_cast<int>(DownTool::AmapVectorRenderCategory::Other))
            {
                return std::nullopt;
            }
            const auto channel = [&color](const char* name) -> std::optional<uint8_t>
            {
                const auto iterator = color.find(name);
                if (iterator == color.end() || !iterator->is_number_integer())
                {
                    return std::nullopt;
                }
                const int value = iterator->get<int>();
                return value >= 0 && value <= 255 ? std::optional<uint8_t>(value) :
                    std::nullopt;
            };
            const auto red = channel("red");
            const auto green = channel("green");
            const auto blue = channel("blue");
            const auto alpha = channel("alpha");
            if (!red || !green || !blue || !alpha)
            {
                return std::nullopt;
            }
            configuration.gisColors[
                static_cast<DownTool::AmapVectorRenderCategory>(category)] =
                {*red, *green, *blue, *alpha};
        }
    }
    if (!DomDownToolPreview::ReadExportGroups(value, configuration.groups))
    {
        return std::nullopt;
    }
    return configuration;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string CurrentTimeText()
{
    const std::time_t currentTime = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &currentTime);
    std::ostringstream output;
    output << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

std::string MakeTaskId()
{
    static std::atomic_uint64_t sequence = 0;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return fmt::format("{}-{}", milliseconds.count(), sequence.fetch_add(1));
}

std::string CategoryName(DownTool::AmapVectorRenderCategory category)
{
    switch (category)
    {
    case DownTool::AmapVectorRenderCategory::Point:
        return "点";
    case DownTool::AmapVectorRenderCategory::Road:
        return "道路";
    case DownTool::AmapVectorRenderCategory::Green:
        return "绿地";
    case DownTool::AmapVectorRenderCategory::Highway:
        return "高速";
    case DownTool::AmapVectorRenderCategory::Subway:
        return "地铁";
    case DownTool::AmapVectorRenderCategory::Building:
        return "建筑";
    case DownTool::AmapVectorRenderCategory::Water:
        return "水";
    case DownTool::AmapVectorRenderCategory::Boundary:
        return "边界";
    case DownTool::AmapVectorRenderCategory::Other:
        return "其他";
    case DownTool::AmapVectorRenderCategory::All:
        return "全部";
    }
    return "GIS";
}

std::string MakeCategoryName(
    const std::set<DownTool::AmapVectorRenderCategory>& categories)
{
    std::string name;
    for (const DownTool::AmapVectorRenderCategory category : categories)
    {
        if (!name.empty())
        {
            name += "+";
        }
        name += CategoryName(category);
    }
    return name.empty() ? "GIS" : name;
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
    return name.empty() ? "GIS" : name;
}

bool IsTianDiTuMultidateHistorySource(const SatelliteExportSource& source)
{
    return source.id == "tianditu.multidate" && !source.multidateYear.empty() &&
        !source.multidateKey.empty();
}

bool IsTianDiTuMultidateSource(const SatelliteExportSource& source)
{
    return source.id == "tianditu.multidate";
}

std::filesystem::path MergedImagePath(const std::filesystem::path& outputRoot,
                                      std::string_view imageFormat)
{
    std::filesystem::path mergedImagePath = outputRoot / "merged";
    mergedImagePath.replace_extension(std::string(imageFormat));
    return mergedImagePath;
}

std::filesystem::path MultidateTemporaryOutputRoot(
    const ExportConfiguration& configuration,
    const std::string& taskId,
    const SatelliteExportSource& source,
    std::string_view layerName)
{
    return DomDownToolPreview::ExportTaskOutputRoot(configuration, taskId) /
        std::filesystem::u8path("临时") /
        std::filesystem::u8path("天地图多时相") /
        std::filesystem::u8path(SanitizeFileName(source.name)) /
        std::filesystem::u8path(std::string(layerName));
}

bool IsBuildingFbxArtifact(const std::filesystem::path& path)
{
    const std::string fileName = PathToUtf8(path.filename());
    return fileName == "buildings.fbx" || fileName.starts_with("building_wall_") ||
        fileName.starts_with("building_roof_");
}

bool CopyBuildingFbxArtifacts(const std::filesystem::path& sourceRoot,
                              const std::filesystem::path& destinationRoot,
                              std::string& error)
{
    std::error_code existsError;
    if (!std::filesystem::exists(sourceRoot / "buildings.fbx", existsError))
    {
        return true;
    }

    std::error_code createError;
    std::filesystem::create_directories(destinationRoot, createError);
    if (createError)
    {
        error = "无法创建建筑 FBX 叠加输出目录：" + createError.message();
        return false;
    }

    std::error_code iteratorError;
    for (std::filesystem::directory_iterator iterator(sourceRoot, iteratorError), end;
         iterator != end && !iteratorError; iterator.increment(iteratorError))
    {
        if (!iterator->is_regular_file() || !IsBuildingFbxArtifact(iterator->path()))
        {
            continue;
        }
        std::error_code copyError;
        std::filesystem::copy_file(
            iterator->path(),
            destinationRoot / iterator->path().filename(),
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        if (copyError)
        {
            error = "复制建筑 FBX 叠加产物失败：" + copyError.message();
            return false;
        }
    }
    if (iteratorError)
    {
        error = "遍历建筑 FBX 叠加产物失败：" + iteratorError.message();
        return false;
    }
    return true;
}

std::unique_ptr<DownTool::DownToolBase> CreateDownloader(std::string_view sourceId)
{
    if (sourceId == "gaode.satellite")
    {
        return std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::Satellite);
    }
    if (sourceId == "gaode.electronic")
    {
        return std::make_unique<DownTool::GaoDeDown>(DownTool::GaoDeMapType::Electronic);
    }
    if (sourceId == "gaode.roads")
    {
        return std::make_unique<DownTool::GaoDeDown>(
            DownTool::GaoDeMapType::RoadNetworkOverlay);
    }
    if (sourceId == "tencent.satellite")
    {
        return std::make_unique<DownTool::TencentDown>();
    }
    if (sourceId == "tencent.electronic")
    {
        return std::make_unique<DownTool::TencentDown>(DownTool::TencentMapType::Electronic);
    }
    if (sourceId == "tianditu.satellite")
    {
        return std::make_unique<DownTool::TianDiTuDown>();
    }
    if (sourceId == "tianditu.multidate")
    {
        return std::make_unique<DownTool::TianDiTuMultidateDown>();
    }
    if (sourceId == "baidu.satellite")
    {
        return std::make_unique<DownTool::BaiduDown>();
    }
    if (sourceId == "baidu.electronic")
    {
        return std::make_unique<DownTool::BaiduDown>(DownTool::BaiduMapType::Electronic);
    }
    if (sourceId == "google.satellite")
    {
        return std::make_unique<DownTool::GoogleDown>();
    }
    if (sourceId == "bing.satellite")
    {
        return std::make_unique<DownTool::BingDown>();
    }
    if (sourceId == "arcgis.satellite")
    {
        return std::make_unique<DownTool::ArcGISDown>();
    }
    return nullptr;
}

double ProgressRatio(uint64_t completed, uint64_t total)
{
    if (total == 0)
    {
        return 0.;
    }
    return std::clamp(static_cast<double>(completed) / static_cast<double>(total), 0., 1.);
}

double GisProcessingRatio(const DownTool::GisProcessingProgress& progress)
{
    return (static_cast<int>(progress.GetStage()) +
            ProgressRatio(progress.GetCompleted(), progress.GetTotal())) / 4.;
}

double DownToolPhaseProgress(const DownTool::DownToolProgress& progress)
{
    const double phaseRatio = ProgressRatio(progress.completed, progress.total);
    if (progress.stage.find("下载瓦片") != std::string::npos || progress.stage == "下载失败")
    {
        return phaseRatio * 0.45;
    }
    if (progress.stage.find("合并大图") != std::string::npos)
    {
        return 0.72 + phaseRatio * 0.16;
    }
    if (progress.stage.find("FBX") != std::string::npos)
    {
        return 0.88 + phaseRatio * 0.12;
    }
    return 0.45 + phaseRatio * 0.27;
}

std::string GisModeName(GisExportMode mode)
{
    switch (mode)
    {
    case GisExportMode::OverlayOnSatellite:
        return "叠加卫星图";
    case GisExportMode::MergeSelectedLayers:
        return "GIS 合并";
    case GisExportMode::SeparateLayers:
        return "GIS 单层";
    }
    return "GIS";
}

std::filesystem::path TaskOutputRoot(const ExportConfiguration& configuration,
                                     const std::string& taskId)
{
    return DomDownToolPreview::ExportTaskOutputRoot(configuration, taskId);
}

std::filesystem::path SatelliteOutputRoot(const ExportConfiguration& configuration,
                                           const std::string& taskId,
                                           const SatelliteExportSource& source)
{
    const std::filesystem::path root = TaskOutputRoot(configuration, taskId);
    if (configuration.flatSourceOutput)
    {
        return root / std::filesystem::u8path(SanitizeFileName(source.name));
    }
    return root / std::filesystem::u8path("卫星图") /
        std::filesystem::u8path(SanitizeFileName(source.name));
}

std::optional<double> TryGetJsonDouble(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return std::nullopt;
    }
    const auto iterator = object.find(fieldName);
    if (iterator == object.end() || !iterator->is_number())
    {
        return std::nullopt;
    }
    return iterator->get<double>();
}

std::filesystem::path ResolveManifestTexturePath(
    const std::filesystem::path& fbxRoot,
    std::string_view pathText)
{
    std::filesystem::path texturePath = std::filesystem::u8path(std::string(pathText));
    if (texturePath.is_absolute())
    {
        return texturePath;
    }
    return fbxRoot / texturePath;
}

std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture>
CollectSatelliteRoofTexturesFromFbxRoot(const std::filesystem::path& fbxRoot)
{
    std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture> textures;
    std::ifstream input(fbxRoot / "manifest.json", std::ios::binary);
    if (!input.is_open())
    {
        return textures;
    }
    const Json manifest = Json::parse(input, nullptr, false);
    if (!manifest.is_object() || !manifest.contains("textures") ||
        !manifest.at("textures").is_array())
    {
        return textures;
    }

    for (const Json& texture : manifest.at("textures"))
    {
        const std::string texturePathText = GetJsonString(texture, "path");
        const auto boundsIterator = texture.find("bounds");
        if (texturePathText.empty() || boundsIterator == texture.end() ||
            !boundsIterator->is_object())
        {
            continue;
        }
        const std::optional<double> west = TryGetJsonDouble(*boundsIterator, "west");
        const std::optional<double> south = TryGetJsonDouble(*boundsIterator, "south");
        const std::optional<double> east = TryGetJsonDouble(*boundsIterator, "east");
        const std::optional<double> north = TryGetJsonDouble(*boundsIterator, "north");
        if (!west || !south || !east || !north)
        {
            continue;
        }
        textures.push_back({
            ResolveManifestTexturePath(fbxRoot, texturePathText),
            *west,
            *south,
            *east,
            *north});
    }
    return textures;
}

std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture> CollectSatelliteRoofTextures(
    const ExportConfiguration& configuration,
    const std::string& taskId)
{
    std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture> textures;
    for (const SatelliteExportSource& source : configuration.satelliteSources)
    {
        const std::filesystem::path fbxRoot = SatelliteOutputRoot(
            configuration,
            taskId,
            source) / "FBX";
        std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture> sourceTextures =
            CollectSatelliteRoofTexturesFromFbxRoot(fbxRoot);
        textures.insert(
            textures.end(),
            std::make_move_iterator(sourceTextures.begin()),
            std::make_move_iterator(sourceTextures.end()));
    }
    return textures;
}

std::filesystem::path GisOutputRoot(const ExportConfiguration& configuration,
                                     const std::string& taskId,
                                    std::string_view modeName,
                                    std::string_view categoryName)
{
    const std::filesystem::path root = TaskOutputRoot(configuration, taskId);
    return root / std::filesystem::u8path("GIS") /
        std::filesystem::u8path(modeName) /
        std::filesystem::u8path(categoryName);
}

std::filesystem::path OverlayOutputRoot(const ExportConfiguration& configuration,
                                        const std::string& taskId,
                                        const SatelliteExportSource& source)
{
    return TaskOutputRoot(configuration, taskId) /
        std::filesystem::u8path("叠加卫星图") /
        std::filesystem::u8path(SanitizeFileName(source.name));
}

bool WriteElevationMetadata(
    const ExportConfiguration& configuration,
    std::string_view taskId,
    const DownTool::CopernicusDemData& elevationData,
    std::string& error)
{
    const std::filesystem::path outputDirectory =
        TaskOutputRoot(configuration, std::string(taskId)) /
        std::filesystem::u8path("高度");
    std::error_code directoryError;
    std::filesystem::create_directories(outputDirectory, directoryError);
    if (directoryError)
    {
        error = "无法创建高度元数据目录：" + directoryError.message();
        return false;
    }

    Json sourceFiles = Json::array();
    for (const std::filesystem::path& sourceFile : elevationData.GetSourceFiles())
    {
        sourceFiles.push_back(PathToUtf8(sourceFile));
    }
    const DownTool::CopernicusDemBounds& bounds = elevationData.GetBounds();
    const Json metadata = {
        {"provider", "Copernicus DEM GLO-30"},
        {"resolutionMeters", 30},
        {"verticalDatum", "EGM2008"},
        {"referenceMode", "sea-level"},
        {"referenceElevationMeters", 0.},
        {"bounds", {{"west", bounds.west}, {"east", bounds.east},
                    {"south", bounds.south}, {"north", bounds.north}}},
        {"sourceFiles", std::move(sourceFiles)},
        {"version", 1}};
    std::ofstream output(outputDirectory / "meta.json",
                         std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        error = "无法创建高度元数据文件。";
        return false;
    }
    output << metadata.dump(2);
    if (!output.good())
    {
        error = "写入高度元数据失败。";
        return false;
    }
    return true;
}

void ConfigureFbxElevation(
    DownTool::DownToolBase& downloader,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData)
{
    if (!elevationData)
    {
        return;
    }
    downloader.SetFbxTerrainHeightEnabled(true);
    downloader.SetFbxFlattenRegions(elevationData->GetFlattenedRegions());
    // 固定海拔零面，避免改变下载范围后模型出现整体高度偏移。
    downloader.SetFbxElevationReferenceMeters(0.);
    downloader.SetFbxElevationSampler(
        [elevationData](const DownTool::LonLat& lonLat)
        {
            return elevationData->Sample(lonLat);
        });
}

void ConfigureTianDiTuMultidateDownloader(
    DownTool::DownToolBase& downloader,
    const SatelliteExportSource& source,
    bool transparentEmptyTiles,
    bool forceLatestWmts)
{
    if (source.id != "tianditu.multidate")
    {
        return;
    }

    auto* multidateDownloader = dynamic_cast<DownTool::TianDiTuMultidateDown*>(&downloader);
    if (!multidateDownloader)
    {
        return;
    }

    multidateDownloader->SetTransparentPlaceholderForEmptyTiles(transparentEmptyTiles);
    if (forceLatestWmts)
    {
        multidateDownloader->UseLatestWmtsOnly();
        return;
    }

    if (!source.multidateGbCode.empty() || !source.multidateRegionName.empty())
    {
        multidateDownloader->SetMultidateRegion(
            source.multidateGbCode, source.multidateRegionName);
    }
    if (!source.multidateCookie.empty())
    {
        multidateDownloader->SetSessionCookie(source.multidateCookie);
    }
    if (!source.multidateYear.empty() && !source.multidateKey.empty())
    {
        multidateDownloader->SetYearKey(source.multidateYear, source.multidateKey);
        return;
    }

    multidateDownloader->UseLatestYear();
}

std::string FormatSourceNames(const ExportConfiguration& configuration)
{
    std::string names;
    for (const SatelliteExportSource& source : configuration.satelliteSources)
    {
        if (!names.empty())
        {
            names += "、";
        }
        names += source.name;
    }
    return names;
}

std::string FormatGisModes(const ExportConfiguration& configuration)
{
    std::string modes;
    for (const GisExportMode mode : configuration.gisModes)
    {
        if (!modes.empty())
        {
            modes += "、";
        }
        modes += GisModeName(mode);
    }
    return modes;
}

std::string BuildTaskDetailsText(const ExportConfiguration& configuration)
{
    if (!configuration.groups.empty())
    {
        std::vector<std::string> outputs;
        const auto addOutput = [&outputs](const std::string& name)
        {
            if (std::ranges::find(outputs, name) == outputs.end())
            {
                outputs.push_back(name);
            }
        };
        for (const ExportContentGroup& group : configuration.groups)
        {
            if (!group.enabled)
            {
                continue;
            }
            for (const ExportComponent& component : group.components)
            {
                switch (component.kind)
                {
                case ExportComponentKind::MergedImage:
                    addOutput("大图");
                    break;
                case ExportComponentKind::DomGeoTiff:
                    addOutput("DOM GeoTIFF");
                    break;
                case ExportComponentKind::DemGeoTiff:
                    addOutput("DEM GeoTIFF");
                    break;
                case ExportComponentKind::StandardXyz:
                    addOutput("标准 XYZ");
                    break;
                case ExportComponentKind::RawTiles:
                    addOutput("原始瓦片");
                    break;
                case ExportComponentKind::BasePlate:
                    addOutput("底板 FBX");
                    break;
                case ExportComponentKind::BuildingModel:
                    addOutput("建筑 FBX");
                    break;
                case ExportComponentKind::CesiumTerrain:
                    addOutput("Cesium 地形");
                    break;
                case ExportComponentKind::CesiumBuildings:
                    addOutput("3D Tiles 建筑");
                    break;
                case ExportComponentKind::GeoJson:
                    addOutput("GeoJSON");
                    break;
                case ExportComponentKind::Shapefile:
                    addOutput("SHP");
                    break;
                }
            }
        }
        std::string result;
        for (const std::string& output : outputs)
        {
            if (!result.empty())
            {
                result += "、";
            }
            result += output;
        }
        return result.empty() ? "未选择组件" : result;
    }

    std::vector<std::string> outputs;
    if (configuration.exportTiles)
    {
        outputs.emplace_back("XYZ 切片");
    }
    if (configuration.exportMergedImage)
    {
        outputs.emplace_back("合并大图");
    }
    if (configuration.exportFbx)
    {
        outputs.emplace_back(configuration.exportTerrainHeight ?
            "FBX 切片、网格与高度" : "FBX 切片与网格");
        if (configuration.exportBuildingFbx)
        {
            outputs.emplace_back(BuildingFbxModeName(configuration));
        }
    }
    if (configuration.exportGeoJson)
    {
        outputs.emplace_back("GeoJSON");
    }

    std::string result;
    for (const std::string& output : outputs)
    {
        if (!result.empty())
        {
            result += "、";
        }
        result += output;
    }
    return result;
}

bool WriteTaskRecordFile(const std::filesystem::path& outputRoot,
                         std::string_view taskId,
                         const ExportConfiguration& configuration,
                         std::string_view status,
                         std::string_view result,
                         std::string& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(outputRoot, directoryError);
    if (directoryError)
    {
        error = "无法创建导出记录目录：" + directoryError.message();
        return false;
    }

    std::ofstream output(
        outputRoot / std::filesystem::u8path(
            "转换记录-" + std::string(taskId) + ".txt"),
        std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        error = "无法创建导出转换记录。";
        return false;
    }

    output << "DomDownTool 导出转换记录\n"
           << "任务 ID: " << taskId << "\n"
           << "状态: " << status << "\n"
           << "记录更新时间: " << CurrentTimeText() << "\n"
           << std::fixed << std::setprecision(6)
           << "中心坐标 (WGS84): " << configuration.centerWgs84.lon << ", "
           << configuration.centerWgs84.lat << "\n"
           << std::setprecision(0)
           << "范围模式: "
           << (configuration.rangeMode == ExportRangeMode::KmlPolygon ?
               "自定义多边形" : "中心点 + 半径") << "\n";
    if (configuration.rangeMode == ExportRangeMode::KmlPolygon)
    {
        output << "KML 文件: "
               << (configuration.kmlFileName.empty() ?
                   "未记录" : configuration.kmlFileName) << "\n"
               << "多边形数量: " << configuration.polygonsWgs84.size() << "\n";
    }
    else
    {
        output << "半径 (米): " << configuration.radiusMeters << "\n";
    }
    output << "GIS 排除区数量: " << DownTool::GetRegionPolygons(
        configuration.GetDownloadRegions(), DownTool::DownloadRegionRole::ExcludeGis).size()
           << "\n";
    output << "底板排除区数量: " << DownTool::GetRegionPolygons(
        configuration.GetDownloadRegions(), DownTool::DownloadRegionRole::ExcludeTerrain).size()
           << "\n";
    output << "平整区域数量: "
           << DownTool::GetRegionPolygons(configuration.GetDownloadRegions(),
                                          DownTool::DownloadRegionRole::FlattenTerrain)
                  .size()
           << "\n";
    output
           << "层级: " << configuration.zoom << "\n"
           << "卫星图来源: " << FormatSourceNames(configuration) << "\n"
           << "用户指定输出根目录: "
           << (configuration.outputRoot.empty() ? "未指定，使用 cache" :
               PathToUtf8(configuration.outputRoot)) << "\n";
    for (const SatelliteExportSource& source : configuration.satelliteSources)
    {
        output << "卫星源: " << source.name << " [" << source.id << "]\n"
               << "  缓存目录: " << PathToUtf8(source.cachePath) << "\n"
               << "  产物目录: "
               << PathToUtf8(SatelliteOutputRoot(configuration, std::string(taskId), source))
               << "\n"
               << "  XYZ 目录: "
               << PathToUtf8(SatelliteOutputRoot(
                      configuration, std::string(taskId), source) / "XYZ")
               << "\n"
               << "  FBX 目录: "
               << PathToUtf8(SatelliteOutputRoot(
                      configuration, std::string(taskId), source) / "FBX")
               << "\n";
        if (source.id == "tianditu.multidate")
        {
            output << "  多时相年份: "
                   << (source.multidateYear.empty() ? "最新影像" : source.multidateYear)
                   << "\n";
            if (!source.multidateGbCode.empty() || !source.multidateRegionName.empty())
            {
                output << "  多时相区域: " << source.multidateRegionName
                       << " [" << source.multidateGbCode << "]\n";
            }
            if (!source.multidateKey.empty())
            {
                output << "  多时相 key: " << source.multidateKey << "\n";
            }
        }
    }
    output << "GIS 产物根目录: "
           << PathToUtf8(TaskOutputRoot(configuration, std::string(taskId)) /
                         std::filesystem::u8path("GIS"))
           << "\n"
           << "GIS 分类: " << MakeCategoryName(configuration.gisCategories) << "\n"
           << "GIS 组合方式: " << FormatGisModes(configuration) << "\n"
           << "导出类型: " << BuildTaskDetailsText(configuration) << "\n"
           << "合并大图: " << configuration.mergedImageFormat << "，最大边 "
           << configuration.mergedImageResolution << "\n"
           << "GIS 透明大图格式: PNG\n"
           << "FBX 贴图尺寸: " << configuration.fbxTextureSize << "\n"
           << "FBX 每块单独导出: " << (configuration.GetFbxPerTile() ? "是" : "否") << "\n"
           << "FBX 贴图格式: " << NormalizeImageFormat(configuration.fbxTextureFormat) << "\n"
           << "FBX 导出高度: " << (configuration.exportTerrainHeight ? "是" : "否") << "\n"
           << "FBX 高度采样率 (米): " << configuration.fbxTerrainSampleRateMeters << "\n"
           << "地形每块三角面上限（0不限）：" << configuration.GetTerrainMaximumTriangles() << "\n"
           << "FBX 距离档位: "
           << DownTool::FbxDistanceLodToJson(configuration.GetDistanceLod()).dump() << "\n"
           << "建筑按高度着色: " << (configuration.colorBuildingsByHeight ? "是" : "否") << "\n"
           << "建筑 FBX: " << (configuration.exportBuildingFbx ? "是" : "否") << "\n"
           << "建筑 FBX 墙体贴图模式: "
           << BuildingFbxTextureModeName(configuration.buildingFbxWallTextureMode) << "\n"
           << "建筑 FBX 屋顶贴图模式: "
           << BuildingFbxRoofTextureModeName(configuration.buildingFbxRoofTextureMode) << "\n"
           << "建筑 FBX 合并贴图格式: "
           << BuildingFbxAtlasImageFormatName(configuration.buildingFbxAtlasImageFormat) << "\n"
           << "建筑 FBX 卫星屋顶输出: "
           << (UsesSatelliteRoofTexture(configuration)
                   ? "写入对应卫星或叠加卫星 FBX 目录，复用 manifest 贴图"
                   : "否")
           << "\n"
           << "建筑墙体贴图目录: " << PathToUtf8(configuration.buildingWallTexturePath) << "\n"
           << "建筑屋顶贴图目录: " << PathToUtf8(configuration.buildingRoofTexturePath) << "\n"
           << "建筑楼层高度 (厘米): " << configuration.buildingLayerHeightCentimeters << "\n"
           << "建筑分块边长 (米): " << configuration.GetBuildingChunkSizeMeters() << "\n"
           << "建筑墙体材质上限: " << configuration.buildingMaxWallMaterialsPerBucket << "\n"
           << "建筑贴图随机种子: " << configuration.buildingTextureRandomSeed << "\n"
           << "结果: " << result << "\n";
    for (const ExportContentGroup& group : configuration.groups)
    {
        if (!group.enabled)
        {
            continue;
        }
        for (const ExportComponent& component : group.components)
        {
            if (component.kind == ExportComponentKind::BasePlate)
            {
                output << "底板组件: " << group.name << " [" << component.id << "]\n"
                       << "  距离档位: "
                       << DownTool::FbxDistanceLodToJson(component.GetDistanceLod()).dump() << "\n"
                       << "  高度采样率 (米): " << component.elevationSampleMeters << "\n"
                       << "  每块三角面上限（0不限）: "
                       << component.GetTerrainMaximumTriangles() << "\n";
            }
        }
    }
    if (!output.good())
    {
        error = "写入导出转换记录失败。";
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> TaskRecordRoots(
    const ExportConfiguration& configuration, const std::string& taskId)
{
    return {TaskOutputRoot(configuration, taskId)};
}

bool WriteTaskRecordFiles(const ExportConfiguration& configuration,
                          const std::string& taskId,
                          std::string_view status,
                          std::string_view result,
                          std::string& error)
{
    for (const std::filesystem::path& root : TaskRecordRoots(configuration, taskId))
    {
        if (!WriteTaskRecordFile(root, taskId, configuration, status, result, error))
        {
            return false;
        }
    }
    return true;
}

DownTool::AmapVectorDecodedData FilterGisData(
    const DownTool::AmapVectorDecodedData& decodedData,
    const std::set<DownTool::AmapVectorRenderCategory>& categories,
    const std::function<bool()>& cancellationRequested)
{
    DownTool::AmapVectorDecodedData filteredData;
    filteredData.version = decodedData.version;
    filteredData.status = decodedData.status;
    filteredData.packetCount = decodedData.packetCount;
    filteredData.features.reserve(decodedData.features.size());
    for (const DownTool::AmapVectorFeature& feature : decodedData.features)
    {
        if (cancellationRequested && cancellationRequested())
        {
            break;
        }
        if (categories.contains(DownTool::ClassifyAmapVectorFeature(feature)))
        {
            filteredData.features.push_back(feature);
        }
    }
    return filteredData;
}

bool IsGisCategorySelected(
    const std::set<DownTool::AmapVectorRenderCategory>& categories,
    DownTool::AmapVectorRenderCategory category)
{
    return categories.contains(DownTool::AmapVectorRenderCategory::All) ||
        categories.contains(category);
}

DownTool::AmapVectorDecodedData FilterBuildingFbxData(
    const DownTool::AmapVectorDecodedData& decodedData,
    const std::set<DownTool::AmapVectorRenderCategory>& categories,
    const std::function<bool()>& cancellationRequested)
{
    DownTool::AmapVectorDecodedData filteredData;
    filteredData.version = decodedData.version;
    filteredData.status = decodedData.status;
    filteredData.packetCount = decodedData.packetCount;
    filteredData.features.reserve(decodedData.features.size());
    if (!IsGisCategorySelected(categories, DownTool::AmapVectorRenderCategory::Building))
    {
        return filteredData;
    }
    for (const DownTool::AmapVectorFeature& feature : decodedData.features)
    {
        if (cancellationRequested && cancellationRequested())
        {
            break;
        }
        if (DownTool::IsAmapVectorBuildingWithHeight(feature))
        {
            filteredData.features.push_back(feature);
        }
    }
    return filteredData;
}

void ApplyBuildingFbxElevation(
    DownTool::AmapBuildingFbxExportOptions& exportOptions,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData)
{
    if (!elevationData)
    {
        return;
    }
    exportOptions.terrainHeightEnabled = true;
    exportOptions.elevationReferenceMeters = 0.;
    exportOptions.elevationSampler =
        [elevationData](const DownTool::LonLat& lonLat)
        {
            return elevationData->Sample(lonLat);
        };
}

bool ExportBuildingFbxDataToRoot(
    const ExportConfiguration& configuration,
    const DownTool::AmapVectorDecodedData& buildingData,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    const std::filesystem::path& fbxRoot,
    const std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture>& satelliteRoofTextures,
    const std::function<bool()>& cancellationRequested,
    std::string& error)
{
    DownTool::AmapBuildingFbxExportOptions exportOptions;
    exportOptions.outputPath = fbxRoot / "buildings.fbx";
    exportOptions.centerWgs84 = configuration.centerWgs84;
    exportOptions.shape = configuration.executionFbxShape.value_or(
        DownTool::FbxPlanetShape::Plane);
    exportOptions.wallTexturePath = configuration.buildingWallTexturePath;
    exportOptions.roofTexturePath = configuration.buildingRoofTexturePath;
    exportOptions.wallTextureMode = configuration.buildingFbxWallTextureMode;
    exportOptions.roofTextureMode = configuration.buildingFbxRoofTextureMode;
    exportOptions.atlasImageFormat = configuration.buildingFbxAtlasImageFormat;
    exportOptions.satelliteRoofTextures = satelliteRoofTextures;
    exportOptions.layerHeightCentimeters = configuration.buildingLayerHeightCentimeters;
    exportOptions.SetChunkSizeMeters(configuration.GetBuildingChunkSizeMeters());
    exportOptions.maxWallMaterialsPerBucket =
        configuration.buildingMaxWallMaterialsPerBucket;
    exportOptions.randomSeed = configuration.buildingTextureRandomSeed;
    exportOptions.cancellationRequested = cancellationRequested;
    ApplyBuildingFbxElevation(exportOptions, elevationData);

    const DownTool::AmapBuildingFbxExportResult exportResult =
        DownTool::ExportAmapBuildingsToFbx(buildingData, exportOptions);
    if (!exportResult.success)
    {
        error = exportResult.error;
        return false;
    }
    if (!std::filesystem::exists(exportOptions.outputPath))
    {
        error = BuildingFbxModeName(configuration) + " 没有生成有效网格。";
        return false;
    }
    return true;
}

bool ExportSatelliteRoofBuildingFbxToRoot(
    const ExportConfiguration& configuration,
    const DownTool::AmapVectorDecodedData& buildingData,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    const std::filesystem::path& fbxRoot,
    const std::function<bool()>& cancellationRequested,
    std::string& error)
{
    std::vector<DownTool::AmapBuildingFbxSatelliteRoofTexture> satelliteRoofTextures =
        CollectSatelliteRoofTexturesFromFbxRoot(fbxRoot);
    if (satelliteRoofTextures.empty())
    {
        error = "屋顶卫星图模式没有找到同目录卫星 FBX 贴图清单：" +
            PathToUtf8(fbxRoot / "manifest.json");
        return false;
    }
    return ExportBuildingFbxDataToRoot(
        configuration,
        buildingData,
        elevationData,
        fbxRoot,
        satelliteRoofTextures,
        cancellationRequested,
        error);
}

std::string BuildTaskTitle(const ExportConfiguration& configuration)
{
    const std::string rangeText =
        configuration.rangeMode == ExportRangeMode::KmlPolygon
            ? (configuration.kmlFileName.empty() ? "自定义范围"
                                                 : "KML " + configuration.kmlFileName)
            : fmt::format("{:.0f} 米", configuration.radiusMeters);
    if (!configuration.groups.empty())
    {
        std::string contents;
        for (const ExportContentGroup& group : configuration.groups)
        {
            if (!group.enabled)
            {
                continue;
            }
            if (!contents.empty())
            {
                contents += "、";
            }
            contents += group.name.empty() ? "未命名" : group.name;
        }
        return fmt::format("z{} {} {}", configuration.zoom, rangeText,
                           contents.empty() ? "未选择内容组" : contents);
    }
    std::string contents = FormatSourceNames(configuration);
    if (!configuration.gisCategories.empty())
    {
        if (!contents.empty())
        {
            contents += " + ";
        }
        contents += "高德 GIS";
    }
    return fmt::format("z{} {} {}", configuration.zoom, rangeText, contents);
}

std::optional<std::string> NormalizeTaskTitle(std::string_view title, std::string& error)
{
    const size_t first = title.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
    {
        error = "任务名称不能为空。";
        return std::nullopt;
    }
    title = title.substr(first, title.find_last_not_of(" \t\r\n") - first + 1);
    if (title.size() > 512 || std::any_of(title.begin(), title.end(),
                                          [](unsigned char character) { return character < 32; }))
    {
        error = "任务名称过长或包含换行等控制字符。";
        return std::nullopt;
    }
    return std::string(title);
}

std::string BuildTaskDetail(const ExportConfiguration& configuration)
{
    const std::string outputs = BuildTaskDetailsText(configuration);
    const std::string modes = FormatGisModes(configuration);
    return modes.empty() ? outputs : outputs + " · " + modes;
}

std::string BuildTaskOutputPath(const ExportConfiguration& configuration, const std::string& taskId)
{
    std::string paths;
    for (const std::filesystem::path& root : TaskRecordRoots(configuration, taskId))
    {
        if (!paths.empty())
        {
            paths += "；";
        }
        paths += PathToUtf8(root);
    }
    return paths.empty() ? "无输出目录" : paths;
}

ExportTaskState ParseTaskState(std::string_view value)
{
    if (value == "running")
    {
        return ExportTaskState::Running;
    }
    if (value == "succeeded")
    {
        return ExportTaskState::Succeeded;
    }
    if (value == "failed")
    {
        return ExportTaskState::Failed;
    }
    if (value == "canceled")
    {
        return ExportTaskState::Canceled;
    }
    return ExportTaskState::Queued;
}

std::string TaskStateValue(ExportTaskState state)
{
    switch (state)
    {
    case ExportTaskState::Queued:
        return "queued";
    case ExportTaskState::Running:
        return "running";
    case ExportTaskState::Succeeded:
        return "succeeded";
    case ExportTaskState::Failed:
        return "failed";
    case ExportTaskState::Canceled:
        return "canceled";
    }
    return "failed";
}

bool IsFinishedState(ExportTaskState state)
{
    return state == ExportTaskState::Succeeded || state == ExportTaskState::Failed ||
        state == ExportTaskState::Canceled;
}

std::optional<std::vector<std::filesystem::path>> ValidatedTaskOutputPaths(
    const ExportTaskRecord& record, std::string& error)
{
    constexpr std::string_view kSeparator = "；";
    std::vector<std::filesystem::path> paths;
    size_t start = 0;
    while (start <= record.outputPath.size())
    {
        const size_t end = record.outputPath.find(kSeparator, start);
        const std::string_view pathText = std::string_view(record.outputPath).substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (pathText.empty())
        {
            error = "任务导出路径为空，已阻止删除。";
            return std::nullopt;
        }

        std::error_code pathError;
        const std::filesystem::path path = std::filesystem::absolute(
            std::filesystem::u8path(std::string(pathText)), pathError).lexically_normal();
        if (pathError || path.filename() != std::filesystem::u8path(record.id) ||
            path.parent_path().filename() != std::filesystem::u8path("导出任务"))
        {
            error = "任务导出路径不符合“导出任务/任务ID”结构，已阻止删除。";
            return std::nullopt;
        }
        paths.push_back(path);
        if (end == std::string::npos)
        {
            break;
        }
        start = end + kSeparator.size();
    }
    return paths;
}

bool PublishTaskOutput(const std::filesystem::path& taskRoot,
                       const std::filesystem::path& stagingRoot, std::string& error)
{
    const std::filesystem::path backupRoot = stagingRoot.wstring() + L".previous";
    std::error_code pathError;
    const bool hadOutput = std::filesystem::exists(taskRoot, pathError);
    if (!pathError && hadOutput)
    {
        std::filesystem::rename(taskRoot, backupRoot, pathError);
    }
    if (pathError)
    {
        error = "无法更新原任务目录，请关闭占用导出文件的程序后重试：" + pathError.message();
        return false;
    }

    std::filesystem::rename(stagingRoot, taskRoot, pathError);
    if (pathError)
    {
        error = "无法将新结果写回原任务目录：" + pathError.message();
        if (hadOutput)
        {
            std::error_code restoreError;
            std::filesystem::rename(backupRoot, taskRoot, restoreError);
            if (restoreError)
            {
                error += "；旧结果保留在 " + PathToUtf8(backupRoot) + "，恢复失败：" +
                         restoreError.message();
            }
        }
        return false;
    }
    if (hadOutput)
    {
        std::filesystem::remove_all(backupRoot, pathError);
        if (pathError)
        {
            error = "新结果已更新，旧结果备份未能清理：" + PathToUtf8(backupRoot) + "（" +
                    pathError.message() + "）";
        }
    }
    return true;
}
} // namespace

std::string DomDownToolPreview::ExportTaskStateName(ExportTaskState state)
{
    switch (state)
    {
    case ExportTaskState::Queued:
        return "排队中";
    case ExportTaskState::Running:
        return "导出中";
    case ExportTaskState::Succeeded:
        return "已完成";
    case ExportTaskState::Failed:
        return "失败";
    case ExportTaskState::Canceled:
        return "已取消";
    }
    return "未知";
}

std::filesystem::path DomDownToolPreview::ExportTaskOutputRoot(
    const ExportConfiguration& configuration, std::string_view taskId)
{
    if (!configuration.executionRootOverride.empty())
    {
        return configuration.executionRootOverride;
    }
    const std::filesystem::path outputRoot = configuration.outputRoot.empty() ?
        std::filesystem::u8path("cache") : configuration.outputRoot;
    return outputRoot / std::filesystem::u8path("导出任务") /
        std::filesystem::u8path(std::string(taskId));
}

std::filesystem::path DomDownToolPreview::ExportGisOverlayInputRoot(
    bool exportTiles, const std::filesystem::path& tileOutputPath,
    const std::filesystem::path& temporaryRoot)
{
    return exportTiles ? tileOutputPath :
        temporaryRoot / std::filesystem::u8path("后处理");
}

DomDownToolPreview::ExportTaskManager::ExportTaskManager(
    std::function<void()> changeCallback, std::filesystem::path recordsPath,
    ExportDownloaderFactory downloaderFactory)
    : changeCallback_(std::move(changeCallback)),
      downloaderFactory_(std::move(downloaderFactory)),
      recordsPath_(std::move(recordsPath))
{
    LoadRecords();
    worker_ = std::thread([this] { WorkerLoop(); });
}

DomDownToolPreview::ExportTaskManager::~ExportTaskManager()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        cancelActive_.store(true, std::memory_order_relaxed);
        if (activeDownloader_)
        {
            activeDownloader_->StopDown();
        }
    }
    condition_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
    SaveRecords(true);
}

std::string DomDownToolPreview::ExportTaskManager::Enqueue(const ExportConfiguration& configuration)
{
    std::string error;
    return Enqueue(configuration, {}, error);
}

std::string DomDownToolPreview::ExportTaskManager::Enqueue(const ExportConfiguration& configuration,
                                                           std::string_view title,
                                                           std::string& error)
{
    error.clear();
    const auto taskTitle = title.empty() ? std::make_optional(BuildTaskTitle(configuration))
                                         : NormalizeTaskTitle(title, error);
    if (!taskTitle)
    {
        return {};
    }
    ExportTaskRecord record;
    record.id = MakeTaskId();
    record.title = *taskTitle;
    record.detail = BuildTaskDetail(configuration);
    record.outputPath = BuildTaskOutputPath(configuration, record.id);
    record.createdAt = CurrentTimeText();
    record.configuration = configuration;
    const std::string taskId = record.id;
    std::string recordError;
    const bool recordWritten =
        WriteTaskRecordFiles(configuration, taskId, "排队中", "等待执行", recordError);
    if (!recordWritten)
    {
        record.state = ExportTaskState::Failed;
        record.stage = "记录失败";
        record.finishedAt = CurrentTimeText();
        record.error = recordError;
    }
    {
        const std::lock_guard lock(mutex_);
        records_.insert(records_.begin(), record);
        if (recordWritten)
        {
            queue_.push_back(QueuedExport{taskId, configuration});
        }
    }
    SaveRecords(true);
    condition_.notify_one();
    NotifyChanged();
    return taskId;
}

bool DomDownToolPreview::ExportTaskManager::Restart(std::string_view taskId,
                                                    const ExportConfiguration& configuration,
                                                    std::string& error,
                                                    std::optional<std::string_view> title)
{
    error.clear();
    std::optional<std::string> taskTitle;
    if (title)
    {
        taskTitle = title->empty() ? std::make_optional(BuildTaskTitle(configuration))
                                   : NormalizeTaskTitle(*title, error);
        if (!taskTitle)
        {
            return false;
        }
    }
    {
        const std::lock_guard lock(mutex_);
        const auto record =
            std::find_if(records_.begin(), records_.end(),
                         [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end() || !IsFinishedState(record->state))
        {
            error = "只能修改并重新下载已完成、失败或已取消的任务。";
            return false;
        }
        const auto paths = ValidatedTaskOutputPaths(*record, error);
        if (!paths || paths->size() != 1)
        {
            error = "原任务没有唯一的有效输出目录，无法在原任务中重新下载。";
            return false;
        }
        ExportConfiguration updated = configuration;
        updated.outputRoot = paths->front().parent_path().parent_path();
        updated.executionRootOverride.clear();
        updated.geoJsonOutputFileOverride.clear();
        updated.buildingFbxOutputOverride.clear();
        if (taskTitle)
        {
            record->title = *taskTitle;
        }
        record->configuration = updated;
        record->detail = BuildTaskDetail(updated);
        record->state = ExportTaskState::Queued;
        record->stage = "等待重新下载";
        record->progress = 0.;
        record->completed = 0;
        record->total = 0;
        record->finishedAt.clear();
        record->error.clear();
        queue_.push_back(QueuedExport{record->id, std::move(updated), true});
    }
    SaveRecords(true);
    condition_.notify_one();
    NotifyChanged();
    return true;
}

bool DomDownToolPreview::ExportTaskManager::Rename(std::string_view taskId, std::string_view title,
                                                   std::string& error)
{
    error.clear();
    const auto taskTitle = NormalizeTaskTitle(title, error);
    if (!taskTitle)
    {
        return false;
    }
    {
        const std::lock_guard lock(mutex_);
        const auto record =
            std::find_if(records_.begin(), records_.end(),
                         [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end())
        {
            error = "任务不存在，无法重命名。";
            return false;
        }
        record->title = *taskTitle;
    }
    SaveRecords(true);
    NotifyChanged();
    return true;
}

bool DomDownToolPreview::ExportTaskManager::Cancel(std::string_view taskId)
{
    bool changed = false;
    std::optional<ExportConfiguration> queuedConfiguration;
    {
        const std::lock_guard lock(mutex_);
        const auto record =
            std::find_if(records_.begin(), records_.end(),
                         [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end())
        {
            return false;
        }
        if (record->state == ExportTaskState::Queued)
        {
            const auto queued =
                std::find_if(queue_.begin(), queue_.end(),
                             [taskId](const QueuedExport& item) { return item.taskId == taskId; });
            if (queued != queue_.end() && !queued->replaceOutput)
            {
                queuedConfiguration = queued->configuration;
            }
            queue_.erase(std::remove_if(
                             queue_.begin(), queue_.end(),
                             [taskId](const QueuedExport& item) { return item.taskId == taskId; }),
                         queue_.end());
            record->state = ExportTaskState::Canceled;
            record->stage = "已取消";
            record->finishedAt = CurrentTimeText();
            changed = true;
        }
        else if (record->state == ExportTaskState::Running)
        {
            if (publishingOutput_)
            {
                return false;
            }
            cancelActive_.store(true, std::memory_order_relaxed);
            if (activeDownloader_)
            {
                activeDownloader_->StopDown();
            }
            record->stage = "正在取消";
            changed = true;
        }
    }
    if (changed)
    {
        if (queuedConfiguration.has_value())
        {
            std::string recordError;
            WriteTaskRecordFiles(
                *queuedConfiguration, std::string(taskId), "已取消", "任务已取消", recordError);
        }
        SaveRecords(true);
        NotifyChanged();
    }
    return changed;
}

bool DomDownToolPreview::ExportTaskManager::RemoveFinished(std::string_view taskId)
{
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end() || !IsFinishedState(record->state))
        {
            return false;
        }
        records_.erase(record);
    }
    SaveRecords(true);
    NotifyChanged();
    return true;
}

bool DomDownToolPreview::ExportTaskManager::DeleteFinishedOutput(
    std::string_view taskId, std::string& error)
{
    ExportTaskRecord record;
    {
        const std::lock_guard lock(mutex_);
        const auto found = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (found == records_.end() || !IsFinishedState(found->state))
        {
            error = "只能永久删除已完成、失败或已取消的任务。";
            return false;
        }
        record = *found;
    }

    const std::optional<std::vector<std::filesystem::path>> outputPaths =
        ValidatedTaskOutputPaths(record, error);
    if (!outputPaths.has_value())
    {
        return false;
    }
    for (const std::filesystem::path& outputPath : *outputPaths)
    {
        std::error_code removeError;
        std::filesystem::remove_all(outputPath, removeError);
        if (removeError)
        {
            error = fmt::format("无法删除导出目录：{}（{}）",
                                PathToUtf8(outputPath), removeError.message());
            return false;
        }
    }
    if (!RemoveFinished(taskId))
    {
        error = "导出文件已删除，但任务记录状态已变化，无法移除记录。";
        return false;
    }
    return true;
}

void DomDownToolPreview::ExportTaskManager::ClearFinished()
{
    {
        const std::lock_guard lock(mutex_);
        records_.erase(std::remove_if(
                           records_.begin(), records_.end(),
                           [](const ExportTaskRecord& record)
                           {
                               return IsFinishedState(record.state);
                           }),
                       records_.end());
    }
    SaveRecords(true);
    NotifyChanged();
}

std::vector<DomDownToolPreview::ExportTaskRecord>
DomDownToolPreview::ExportTaskManager::GetRecords() const
{
    const std::lock_guard lock(mutex_);
    return records_;
}

void DomDownToolPreview::ExportTaskManager::WorkerLoop()
{
    while (true)
    {
        QueuedExport queuedExport;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_)
            {
                return;
            }
            queuedExport = std::move(queue_.front());
            queue_.pop_front();
            cancelActive_.store(false, std::memory_order_relaxed);
            publishingOutput_ = false;
            lastProgressNotificationTime_ = {};
            const auto record = std::find_if(
                records_.begin(), records_.end(),
                [&queuedExport](const ExportTaskRecord& item)
                { return item.id == queuedExport.taskId; });
            if (record == records_.end() || record->state != ExportTaskState::Queued)
            {
                continue;
            }
            record->state = ExportTaskState::Running;
            record->stage = "准备导出";
        }
        SaveRecords(true);
        NotifyChanged();

        std::string error;
        bool succeeded = false;
        try
        {
            succeeded = queuedExport.replaceOutput ? RunReplacementExport(queuedExport, error)
                                                   : RunExport(queuedExport, error);
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
        }
        catch (...)
        {
            error = "导出任务发生未知异常。";
        }

        ExportTaskState finalState = ExportTaskState::Failed;
        if (succeeded && queuedExport.replaceOutput)
        {
            finalState = ExportTaskState::Succeeded;
        }
        else if (IsCancellationRequested())
        {
            finalState = ExportTaskState::Canceled;
        }
        else if (succeeded)
        {
            finalState = ExportTaskState::Succeeded;
        }
        const std::string result = finalState == ExportTaskState::Succeeded ?
            "导出完成" : finalState == ExportTaskState::Canceled ?
            "任务已取消" : error;
        std::string recordError;
        if (!queuedExport.replaceOutput &&
            !WriteTaskRecordFiles(queuedExport.configuration, queuedExport.taskId,
                                  ExportTaskStateName(finalState), result, recordError))
        {
            finalState = ExportTaskState::Failed;
            error = error.empty() ? recordError : error + "；" + recordError;
        }
        FinishTask(queuedExport.taskId, finalState, std::move(error));
    }
}

bool DomDownToolPreview::ExportTaskManager::RunReplacementExport(const QueuedExport& queuedExport,
                                                                 std::string& error)
{
    const std::filesystem::path taskRoot =
        ExportTaskOutputRoot(queuedExport.configuration, queuedExport.taskId);
    const std::filesystem::path stagingRoot =
        taskRoot.parent_path() /
        std::filesystem::u8path(queuedExport.taskId + ".rebuild-" + MakeTaskId());
    std::error_code directoryError;
    std::filesystem::create_directories(taskRoot.parent_path(), directoryError);
    if (directoryError || !std::filesystem::create_directory(stagingRoot, directoryError))
    {
        error = "无法创建任务重新下载的临时目录：" + directoryError.message();
        return false;
    }

    // 新结果完整生成后才替换原目录，取消或失败不会破坏三维软件引用的旧文件。
    bool succeeded = false;
    try
    {
        QueuedExport staged = queuedExport;
        staged.configuration.executionRootOverride = stagingRoot;
        succeeded = RunExport(staged, error) && !IsCancellationRequested();
        if (succeeded)
        {
            succeeded =
                WriteTaskRecordFile(stagingRoot, queuedExport.taskId, queuedExport.configuration,
                                    "已完成", "导出完成", error);
        }
        if (succeeded)
        {
            {
                const std::lock_guard lock(mutex_);
                succeeded = !cancelActive_.load(std::memory_order_relaxed) && !stopping_;
                publishingOutput_ = succeeded;
            }
            if (succeeded)
            {
                UpdateProgress(queuedExport.taskId, "正在更新导出文件", 0.99, 0, 0);
                succeeded = PublishTaskOutput(taskRoot, stagingRoot, error);
            }
        }
    }
    catch (const std::exception& exception)
    {
        succeeded = false;
        error = exception.what();
    }
    catch (...)
    {
        succeeded = false;
        error = "重新下载任务发生未知异常。";
    }
    std::filesystem::remove_all(stagingRoot, directoryError);
    if (directoryError)
    {
        error += "；临时目录清理失败：" + PathToUtf8(stagingRoot) + "（" +
                 directoryError.message() + "）";
    }
    return succeeded;
}

bool DomDownToolPreview::ExportTaskManager::RunGroupedExport(
    const QueuedExport& queuedExport, std::string& error)
{
    return ExportGroupedRunner::Run(*this, queuedExport, error);
}

bool DomDownToolPreview::ExportTaskManager::RunExport(
    const QueuedExport& queuedExport, std::string& error)
{
    if (IsCancellationRequested())
    {
        return false;
    }
    if (!queuedExport.configuration.groups.empty())
    {
        return RunGroupedExport(queuedExport, error);
    }

    const ExportConfiguration& configuration = queuedExport.configuration;
    if (configuration.exportFbx)
    {
        error = configuration.GetDistanceLod().Validate();
        if (!error.empty())
        {
            return false;
        }
    }
    if (configuration.exportBuildingFbx &&
        (!std::isfinite(configuration.GetBuildingChunkSizeMeters()) ||
            configuration.GetBuildingChunkSizeMeters() <= 0.))
    {
        error = "建筑分块边长必须是大于 0 的米数。";
        return false;
    }
    if (configuration.exportTerrainHeight && !configuration.exportFbx)
    {
        error = "导出高度需要同时启用 FBX。";
        return false;
    }
    if (configuration.exportBuildingFbx && !configuration.exportFbx)
    {
        error = "建筑 FBX 需要同时启用 FBX。";
        return false;
    }
    if (configuration.exportBuildingFbx &&
        !IsGisCategorySelected(
            configuration.gisCategories, DownTool::AmapVectorRenderCategory::Building))
    {
        error = "建筑 FBX 需要选择 GIS 建筑图层。";
        return false;
    }
    if (configuration.exportBuildingFbx &&
        configuration.buildingFbxRoofTextureMode ==
            DownTool::AmapBuildingFbxRoofTextureMode::Satellite &&
        configuration.satelliteSources.empty())
    {
        error = "屋顶卫星图模式需要至少选择一个卫星图来源。";
        return false;
    }
    const bool satelliteRequested = !configuration.satelliteSources.empty() &&
        (configuration.exportTiles || configuration.exportMergedImage ||
         configuration.exportFbx);
    const bool gisModeVisualRequested = !configuration.gisCategories.empty() &&
        !configuration.gisModes.empty() &&
        (configuration.exportTiles || configuration.exportMergedImage ||
         configuration.exportFbx);
    const bool standaloneBuildingFbxRequested = configuration.exportBuildingFbx &&
        !gisModeVisualRequested;
    if (!configuration.gisModes.empty() &&
        !configuration.exportTiles && !configuration.exportMergedImage &&
        !configuration.exportFbx)
    {
        error = "GIS 组合方式需要选择 XYZ、合并大图或 FBX。";
        return false;
    }
    if (!configuration.gisCategories.empty() &&
        (configuration.exportTiles || configuration.exportMergedImage ||
         configuration.exportFbx) && configuration.gisModes.empty() &&
        !configuration.exportGeoJson && !configuration.exportBuildingFbx)
    {
        error = "所选 GIS 数据需要至少一种 GIS 组合方式。";
        return false;
    }
    const bool gisRequested = gisModeVisualRequested || standaloneBuildingFbxRequested ||
        (!configuration.gisCategories.empty() && configuration.exportGeoJson);
    const size_t separateCount = gisModeVisualRequested && configuration.gisModes.contains(
        GisExportMode::SeparateLayers) ? configuration.gisCategories.size() : 0u;
    const size_t mergedCount = gisModeVisualRequested && configuration.gisModes.contains(
        GisExportMode::MergeSelectedLayers) ? 1u : 0u;
    const size_t overlayCount = gisModeVisualRequested && configuration.gisModes.contains(
        GisExportMode::OverlayOnSatellite) ? 1u : 0u;
    const size_t geoJsonCount = configuration.exportGeoJson ? 1u : 0u;
    const size_t elevationCount = configuration.exportTerrainHeight ? 1u : 0u;
    const size_t stepCount = (satelliteRequested ? configuration.satelliteSources.size() : 0u) +
        (gisRequested ? separateCount + mergedCount + overlayCount + geoJsonCount : 0u) +
        (standaloneBuildingFbxRequested ? 1u : 0u) + elevationCount;
    if (stepCount == 0)
    {
        error = "没有选择任何导出内容。";
        return false;
    }

    if (configuration.gisModes.contains(GisExportMode::OverlayOnSatellite) &&
        configuration.satelliteSources.empty())
    {
        error = "GIS 叠加输出至少需要选择一个卫星图来源。";
        return false;
    }

    size_t stepIndex = 0;
    std::shared_ptr<DownTool::CopernicusDemData> elevationData;
    if (configuration.exportTerrainHeight)
    {
        DownTool::CopernicusDemOptions elevationOptions;
        elevationOptions.centerWgs84 = configuration.centerWgs84;
        elevationOptions.radiusMeters = configuration.radiusMeters;
        elevationOptions.polygonsWgs84 = configuration.polygonsWgs84;
        elevationOptions.SetFlattenPolygonsWgs84(DownTool::GetRegionPolygons(
            configuration.GetDownloadRegions(), DownTool::DownloadRegionRole::FlattenTerrain));
        elevationOptions.cancellationRequested =
            [this] { return IsCancellationRequested(); };
        elevationOptions.progressCallback =
            [this, taskId = queuedExport.taskId, stepIndex, stepCount](
                const DownTool::CopernicusDemProgress& progress)
            {
                const double overall =
                    (static_cast<double>(stepIndex) +
                     ProgressRatio(progress.completed, progress.total)) /
                    static_cast<double>(stepCount);
                UpdateProgress(taskId, progress.stage, overall,
                               progress.completed, progress.total);
            };
        elevationData = DownTool::CopernicusDemData::Prepare(elevationOptions, error);
        if (!elevationData)
        {
            return false;
        }
        if (!WriteElevationMetadata(
                configuration, queuedExport.taskId, *elevationData, error))
        {
            return false;
        }
        UpdateProgress(
            queuedExport.taskId, "高程准备完成",
            static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount), 1, 1);
        ++stepIndex;
    }
    if (satelliteRequested)
    {
        for (const SatelliteExportSource& source : configuration.satelliteSources)
        {
            if (IsCancellationRequested())
            {
                return false;
            }
            if (!RunSatelliteExport(
                    queuedExport, source, elevationData, stepIndex++, stepCount, error))
            {
                return false;
            }
        }
    }
    if (gisRequested)
    {
        if (IsCancellationRequested())
        {
            return false;
        }
        if (mergedCount != 0 &&
            !RunGisExport(queuedExport, configuration.gisCategories,
                          GisExportMode::MergeSelectedLayers, "合并图层",
                          elevationData,
                          stepIndex++, stepCount, error))
        {
            return false;
        }
        if (overlayCount != 0 &&
            !RunGisExport(queuedExport, configuration.gisCategories,
                          GisExportMode::OverlayOnSatellite, "叠加卫星图",
                          elevationData,
                          stepIndex++, stepCount, error))
        {
            return false;
        }
        if (separateCount != 0)
        {
            for (const DownTool::AmapVectorRenderCategory category :
                 configuration.gisCategories)
            {
                if (IsCancellationRequested())
                {
                    return false;
                }
                if (!RunGisExport(queuedExport, {category}, GisExportMode::SeparateLayers,
                                  CategoryName(category), elevationData,
                                  stepIndex++, stepCount, error))
                {
                    return false;
                }
            }
        }
        if (geoJsonCount != 0 &&
            !RunGeoJsonExport(queuedExport, stepIndex++, stepCount, error))
        {
            return false;
        }
        if (standaloneBuildingFbxRequested &&
            !RunBuildingFbxExport(queuedExport, elevationData, stepIndex++, stepCount, error))
        {
            return false;
        }
    }
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunSatelliteExport(
    const QueuedExport& queuedExport, const SatelliteExportSource& source,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    size_t stepIndex, size_t stepCount,
    std::string& error,
    const bool maxZoomTilesOnly)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsTianDiTuMultidateHistorySource(source))
    {
        return RunTianDiTuMultidateCompositeExport(
            queuedExport,
            source,
            elevationData,
            stepIndex,
            stepCount,
            error,
            maxZoomTilesOnly);
    }

    SatelliteDownloadOptions options;
    options.outputRoot = SatelliteOutputRoot(configuration, queuedExport.taskId, source);
    options.mergedImageFormat = NormalizeImageFormat(configuration.mergedImageFormat);
    options.fbxTextureFormat = NormalizeImageFormat(configuration.fbxTextureFormat);
    options.forceMultidateLatestWmts = IsTianDiTuMultidateSource(source);
    options.maxZoomTilesOnly = maxZoomTilesOnly;
    return RunSatelliteDownloaderToRoot(
        queuedExport, source, options, elevationData, stepIndex, stepCount, error);
}

bool DomDownToolPreview::ExportTaskManager::RunSatelliteDownloaderToRoot(
    const QueuedExport& queuedExport,
    const SatelliteExportSource& source,
    const SatelliteDownloadOptions& options,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    size_t stepIndex, size_t stepCount,
    std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return false;
    }
    std::unique_ptr<DownTool::DownToolBase> downloader =
        downloaderFactory_ ? downloaderFactory_(source.id) : CreateDownloader(source.id);
    if (!downloader)
    {
        error = "当前地图源不能导出卫星图。";
        return false;
    }
    downloader->SetCancellationRequestedCallback(
        [this] { return IsCancellationRequested(); });
    if (IsCancellationRequested())
    {
        return false;
    }

    downloader->SetCenterLonLat(configuration.centerWgs84);
    downloader->SetRadius(configuration.radiusMeters);
    downloader->SetDownloadPolygonsWgs84(configuration.polygonsWgs84);
    downloader->SetZoom(configuration.zoom);
    const uint32_t tileSize = options.tileSizeOverride.value_or(
        configuration.executionTileSize);
    if (tileSize != 0)
    {
        downloader->SetTileSize(tileSize);
    }
    const std::optional<DownTool::FbxPlanetShape> fbxShape =
        options.fbxShapeOverride.has_value() ? options.fbxShapeOverride :
        configuration.executionFbxShape;
    if (fbxShape.has_value())
    {
        downloader->SetFbxShape(*fbxShape);
    }
    const uint32_t meshSubdivision = options.fbxSubdivisionOverride.value_or(
        configuration.executionFbxSubdivision);
    if (meshSubdivision != 0)
    {
        downloader->SetFbxMeshSubdivision(meshSubdivision);
    }
    downloader->SetCachePath(PathToUtf8(source.cachePath));
    ConfigureTianDiTuMultidateDownloader(
        *downloader,
        source,
        options.transparentMultidateEmptyTiles,
        options.forceMultidateLatestWmts);
    const uint32_t hardwareQueue = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t configuredQueue = options.maxDownloadQueue == 0 ?
        hardwareQueue : std::min(options.maxDownloadQueue, hardwareQueue);
    downloader->SetMaxDownQueue(std::max(1u, configuredQueue));
    const std::filesystem::path outputRoot = options.outputRoot;
    std::error_code outputError;
    std::filesystem::create_directories(outputRoot, outputError);
    if (outputError)
    {
        error = "无法创建卫星图输出目录：" + outputError.message();
        return false;
    }
    const bool exportTiles = options.exportTilesOverride.value_or(
        configuration.exportTiles);
    const bool exportMergedImage = options.exportMergedImageOverride.value_or(
        configuration.exportMergedImage);
    const bool exportFbx = options.exportFbxOverride.value_or(configuration.exportFbx);
    const uint32_t mergedResolution = options.mergedImageResolutionOverride.value_or(
        configuration.mergedImageResolution);
    const uint32_t textureSize = options.fbxTextureSizeOverride.value_or(
        configuration.fbxTextureSize);
    const std::filesystem::path xyzOutputRoot = options.xyzOutputPathOverride.empty() ?
        outputRoot / "XYZ" : options.xyzOutputPathOverride;
    const std::filesystem::path fbxOutputRoot = options.fbxOutputPathOverride.empty() ?
        outputRoot / "FBX" : options.fbxOutputPathOverride;
    downloader->SetPostProcessOutputPath(PathToUtf8(xyzOutputRoot));
    downloader->SetPostProcessEnabled(exportTiles);
    downloader->SetPostProcessMaxZoomOnlyEnabled(options.maxZoomTilesOnly);
    downloader->SetPostProcessMergedImageEnabled(exportMergedImage);
    const std::string mergedImageFormat = NormalizeImageFormat(options.mergedImageFormat);
    const std::filesystem::path mergedImagePath =
        options.mergedImageOutputPathOverride.empty() ?
        MergedImagePath(outputRoot, mergedImageFormat) :
        options.mergedImageOutputPathOverride;
    downloader->SetPostProcessMergedImageOutputPath(PathToUtf8(mergedImagePath));
    downloader->SetPostProcessMergedImageResolution(mergedResolution);
    downloader->SetPostProcessFbxEnabled(exportFbx);
    downloader->SetFbxOutputPath(PathToUtf8(fbxOutputRoot));
    downloader->SetFbxTextureSize(textureSize);
    downloader->SetFbxTextureFormat(NormalizeImageFormat(options.fbxTextureFormat));
    downloader->SetFbxTerrainSampleRateMeters(
        configuration.fbxTerrainSampleRateMeters);
    downloader->SetFbxTerrainMaximumTriangles(configuration.GetTerrainMaximumTriangles());
    downloader->SetFbxPerTile(configuration.GetFbxPerTile());
    downloader->SetFbxDistanceLod(configuration.GetDistanceLod());
    downloader->SetFbxExcludedPolygonsWgs84(DownTool::GetRegionPolygons(
        configuration.GetDownloadRegions(), DownTool::DownloadRegionRole::ExcludeTerrain));
    ConfigureFbxElevation(*downloader, elevationData);
    downloader->SetProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, options](
            const DownTool::DownToolProgress& progress)
        {
            const double phaseProgress = std::clamp(
                options.stepProgressStart +
                    DownToolPhaseProgress(progress) * options.stepProgressSpan,
                0., 1.);
            const double overall = (static_cast<double>(stepIndex) + phaseProgress) /
                static_cast<double>(stepCount);
            UpdateProgress(taskId, options.progressStagePrefix + progress.stage, overall,
                           progress.completed, progress.total);
        });
    SetActiveDownloader(downloader.get());
    if (IsCancellationRequested())
    {
        downloader->StopDown();
        ClearActiveDownloader();
        return false;
    }
    try
    {
        downloader->StartDown();
    }
    catch (...)
    {
        ClearActiveDownloader();
        throw;
    }
    ClearActiveDownloader();
    if (IsCancellationRequested())
    {
        return false;
    }

    if (!downloader->GetDownloadError().empty())
    {
        error = downloader->GetDownloadError();
        return false;
    }

    if (exportTiles &&
        !std::filesystem::exists(xyzOutputRoot / "meta.json"))
    {
        error = "卫星 XYZ 瓦片未生成完整元数据。";
        return false;
    }
    if (options.maxZoomTilesOnly &&
        !std::filesystem::exists(
            xyzOutputRoot / std::to_string(configuration.zoom)))
    {
        error = "卫星图最高层级瓦片未生成。";
        return false;
    }
    if (exportMergedImage && !std::filesystem::exists(mergedImagePath))
    {
        error = "卫星合并大图未生成。";
        return false;
    }
    if (exportFbx && !std::filesystem::exists(fbxOutputRoot / "manifest.json"))
    {
        error = "卫星 FBX 贴图清单未生成。";
        return false;
    }

    if (options.reportStepComplete)
    {
        UpdateProgress(queuedExport.taskId, "卫星图导出完成",
                       static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                       1, 1);
    }
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunTianDiTuMultidateCompositeExport(
    const QueuedExport& queuedExport,
    const SatelliteExportSource& source,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    size_t stepIndex, size_t stepCount,
    std::string& error,
    const bool maxZoomTilesOnly)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    const std::filesystem::path finalOutputRoot = SatelliteOutputRoot(
        configuration, queuedExport.taskId, source);
    const std::filesystem::path baseOutputRoot = MultidateTemporaryOutputRoot(
        configuration, queuedExport.taskId, source, "最新底图");
    const std::filesystem::path historyOutputRoot = MultidateTemporaryOutputRoot(
        configuration, queuedExport.taskId, source, "历史覆盖");
    const std::filesystem::path temporaryOutputRoot = baseOutputRoot.parent_path();

    std::error_code cleanupError;
    std::filesystem::remove_all(temporaryOutputRoot, cleanupError);
    if (cleanupError)
    {
        error = "无法清理多时相临时目录：" + cleanupError.message();
        return false;
    }
    std::filesystem::remove_all(finalOutputRoot, cleanupError);
    if (cleanupError)
    {
        error = "无法清理多时相输出目录：" + cleanupError.message();
        return false;
    }

    SatelliteExportSource baseSource = source;
    baseSource.name = source.name + " 最新底图";
    baseSource.multidateYear.clear();
    baseSource.multidateKey.clear();
    baseSource.multidateGbCode.clear();
    baseSource.multidateRegionName.clear();
    baseSource.multidateCookie.clear();

    SatelliteDownloadOptions baseOptions;
    baseOptions.outputRoot = baseOutputRoot;
    baseOptions.mergedImageFormat = NormalizeImageFormat(configuration.mergedImageFormat);
    baseOptions.fbxTextureFormat = NormalizeImageFormat(configuration.fbxTextureFormat);
    baseOptions.progressStagePrefix = "多时相底图 · ";
    baseOptions.stepProgressSpan = 0.42;
    baseOptions.forceMultidateLatestWmts = true;
    baseOptions.maxZoomTilesOnly = maxZoomTilesOnly;
    baseOptions.reportStepComplete = false;
    if (!RunSatelliteDownloaderToRoot(
            queuedExport, baseSource, baseOptions, elevationData,
            stepIndex, stepCount, error))
    {
        return false;
    }

    SatelliteDownloadOptions historyOptions;
    historyOptions.outputRoot = historyOutputRoot;
    historyOptions.mergedImageFormat = "png";
    historyOptions.fbxTextureFormat = "png";
    historyOptions.progressStagePrefix = "多时相历史 · ";
    historyOptions.stepProgressStart = 0.42;
    historyOptions.stepProgressSpan = 0.42;
    historyOptions.maxDownloadQueue = 2;
    historyOptions.transparentMultidateEmptyTiles = true;
    historyOptions.maxZoomTilesOnly = maxZoomTilesOnly;
    historyOptions.reportStepComplete = false;
    if (!RunSatelliteDownloaderToRoot(
            queuedExport, source, historyOptions, elevationData,
            stepIndex, stepCount, error))
    {
        return false;
    }

    const ExportCancellationCallback cancellationRequested =
        [this] { return IsCancellationRequested(); };
    UpdateProgress(
        queuedExport.taskId,
        "合成多时相输出",
        (static_cast<double>(stepIndex) + 0.88) / static_cast<double>(stepCount),
        0,
        1);
    if (!ExportImageComposer::CopyDirectory(
            baseOutputRoot, finalOutputRoot, error, cancellationRequested))
    {
        return false;
    }

    if ((configuration.exportTiles || maxZoomTilesOnly) &&
        !ExportImageComposer::CompositeXyzTileTree(
            finalOutputRoot / "XYZ",
            historyOutputRoot / "XYZ",
            finalOutputRoot / "XYZ",
            error,
            cancellationRequested))
    {
        return false;
    }
    if (configuration.exportFbx &&
        !ExportImageComposer::CompositeTileTree(
            finalOutputRoot / "FBX",
            historyOutputRoot / "FBX",
            finalOutputRoot / "FBX",
            error,
            cancellationRequested))
    {
        return false;
    }
    if (configuration.exportMergedImage &&
        !ExportImageComposer::CompositeImage(
            MergedImagePath(finalOutputRoot, NormalizeImageFormat(configuration.mergedImageFormat)),
            MergedImagePath(historyOutputRoot, "png"),
            MergedImagePath(finalOutputRoot, NormalizeImageFormat(configuration.mergedImageFormat)),
            error,
            cancellationRequested))
    {
        return false;
    }

    if (configuration.exportTiles &&
        !std::filesystem::exists(finalOutputRoot / "XYZ" / "meta.json"))
    {
        error = "卫星 XYZ 瓦片未生成完整元数据。";
        return false;
    }
    if (maxZoomTilesOnly &&
        !std::filesystem::exists(
            finalOutputRoot / "XYZ" /
            std::to_string(configuration.zoom)))
    {
        error = "多时相最高层级瓦片未生成。";
        return false;
    }
    if (configuration.exportMergedImage &&
        !std::filesystem::exists(
            MergedImagePath(finalOutputRoot, NormalizeImageFormat(configuration.mergedImageFormat))))
    {
        error = "卫星合并大图未生成。";
        return false;
    }
    if (configuration.exportFbx &&
        !std::filesystem::exists(finalOutputRoot / "FBX" / "manifest.json"))
    {
        error = "卫星 FBX 贴图清单未生成。";
        return false;
    }

    std::filesystem::remove_all(temporaryOutputRoot, cleanupError);
    UpdateProgress(queuedExport.taskId, "卫星图导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunGisExport(
    const QueuedExport& queuedExport,
    const std::set<DownTool::AmapVectorRenderCategory>& categories,
    GisExportMode mode, std::string_view name,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    size_t stepIndex, size_t stepCount, std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return false;
    }
    if (categories.empty())
    {
        error = "GIS 导出组不能为空。";
        return false;
    }

    const std::string groupName = SanitizeFileName(std::string(name));
    const std::filesystem::path temporaryRoot =
        std::filesystem::u8path("cache/高德地图/gis数据/临时") /
        std::filesystem::u8path(queuedExport.taskId) /
        std::filesystem::u8path(groupName);
    std::error_code cleanupError;
    std::filesystem::remove_all(temporaryRoot, cleanupError);
    if (IsCancellationRequested())
    {
        return false;
    }

    DownTool::AmapVectorPngDownloadOptions options;
    options.vectorDownload.centerWgs84 = configuration.centerWgs84;
    options.vectorDownload.radiusMeters = configuration.radiusMeters;
    options.vectorDownload.polygonsWgs84 = configuration.polygonsWgs84;
    options.vectorDownload.SetGisSpatialRange(ExportGisSpatialRange(configuration));
    options.vectorDownload.zoom = configuration.zoom;
    options.vectorDownload.includeBaseLayers = true;
    options.vectorDownload.includeLabels =
        categories.contains(DownTool::AmapVectorRenderCategory::Point);
    options.vectorDownload.maxConcurrency = 0;
    options.vectorDownload.SetProcessingProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::GisProcessingProgress& progress)
        {
            const double processing = GisProcessingRatio(progress);
            UpdateProgress(taskId,
                std::string(DownTool::GisProcessingStageName(progress.GetStage())) + " · " +
                    groupName,
                (static_cast<double>(stepIndex) + 0.2 + processing * 0.1) /
                    static_cast<double>(stepCount),
                progress.GetCompleted(), progress.GetTotal());
        });
    options.vectorDownload.cancellationRequested =
        [this] { return IsCancellationRequested(); };
    options.vectorDownload.progressCallback =
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::AmapVectorDownloadProgress& progress)
        {
            const double subProgress = ProgressRatio(
                progress.completedTaskCount, progress.totalTaskCount) * 0.2;
            UpdateProgress(taskId, "下载 GIS PBF · " + groupName,
                           (static_cast<double>(stepIndex) + subProgress) /
                               static_cast<double>(stepCount),
                           progress.completedTaskCount, progress.totalTaskCount);
        };
    options.render.categories = categories;
    options.render.colors = configuration.gisColors;
    options.render.colorBuildingsByHeight = configuration.colorBuildingsByHeight;
    const bool satelliteRoofBuildingFbx = configuration.exportBuildingFbx &&
        UsesSatelliteRoofTexture(configuration);
    options.render.exportBuildingsToFbx =
        configuration.exportBuildingFbx && !satelliteRoofBuildingFbx;
    options.render.buildingWallTexturePath = configuration.buildingWallTexturePath;
    options.render.buildingRoofTexturePath = configuration.buildingRoofTexturePath;
    options.render.buildingFbxWallTextureMode = configuration.buildingFbxWallTextureMode;
    options.render.buildingFbxRoofTextureMode = configuration.buildingFbxRoofTextureMode;
    options.render.buildingFbxAtlasImageFormat =
        configuration.buildingFbxAtlasImageFormat;
    options.render.buildingFbxSatelliteRoofTextures = CollectSatelliteRoofTextures(
        configuration,
        queuedExport.taskId);
    options.render.buildingLayerHeightCentimeters =
        configuration.buildingLayerHeightCentimeters;
    options.render.SetBuildingChunkSizeMeters(configuration.GetBuildingChunkSizeMeters());
    options.render.buildingMaxWallMaterialsPerBucket =
        configuration.buildingMaxWallMaterialsPerBucket;
    options.render.buildingTextureRandomSeed = configuration.buildingTextureRandomSeed;
    options.render.pngTileCachePath = temporaryRoot;
    options.render.maxConcurrency = 0;
    options.render.cancellationRequested =
        [this] { return IsCancellationRequested(); };
    options.render.progressCallback =
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::AmapVectorRenderProgress& progress)
        {
            const double subProgress = 0.3 + ProgressRatio(
                progress.completedTileCount, progress.totalTileCount) * 0.35;
            UpdateProgress(taskId, "渲染 GIS PNG · " + groupName,
                           (static_cast<double>(stepIndex) + subProgress) /
                               static_cast<double>(stepCount),
                           progress.completedTileCount, progress.totalTileCount);
        };

    const std::filesystem::path finalOutputRoot = GisOutputRoot(
        configuration, queuedExport.taskId, GisModeName(mode), groupName);
    const std::filesystem::path outputRoot = mode == GisExportMode::OverlayOnSatellite ?
        temporaryRoot / std::filesystem::u8path("叠加后处理") : finalOutputRoot;
    options.vectorDownload.geoJsonOutputPath.clear();
    const std::filesystem::path tileOutputPath = outputRoot / "XYZ";
    const std::filesystem::path postProcessPath = configuration.exportTiles ?
        tileOutputPath : temporaryRoot / std::filesystem::u8path("后处理");
    std::filesystem::path mergedImagePath = outputRoot / "merged";
    mergedImagePath.replace_extension("png");
    const std::filesystem::path fbxOutputPath = outputRoot / "FBX";

    DownTool::AmapVectorPngDownloader downloader;
    downloader.SetPostProcessOutputPath(PathToUtf8(postProcessPath));
    downloader.SetPostProcessEnabled(configuration.exportTiles);
    downloader.SetPostProcessMergedImageEnabled(configuration.exportMergedImage);
    downloader.SetPostProcessMergedImageOutputPath(PathToUtf8(mergedImagePath));
    downloader.SetPostProcessMergedImageResolution(configuration.mergedImageResolution);
    const bool exportFbx = configuration.exportFbx;
    downloader.SetPostProcessFbxEnabled(exportFbx);
    downloader.SetFbxOutputPath(PathToUtf8(fbxOutputPath));
    downloader.SetFbxTextureSize(configuration.fbxTextureSize);
    downloader.SetFbxTextureFormat(mode == GisExportMode::OverlayOnSatellite ?
        "png" : NormalizeImageFormat(configuration.fbxTextureFormat));
    downloader.SetFbxTerrainSampleRateMeters(
        configuration.fbxTerrainSampleRateMeters);
    downloader.SetFbxTerrainMaximumTriangles(configuration.GetTerrainMaximumTriangles());
    downloader.SetFbxPerTile(configuration.GetFbxPerTile());
    downloader.SetFbxDistanceLod(configuration.GetDistanceLod());
    downloader.SetFbxExcludedPolygonsWgs84(DownTool::GetRegionPolygons(
        configuration.GetDownloadRegions(), DownTool::DownloadRegionRole::ExcludeTerrain));
    ConfigureFbxElevation(downloader, elevationData);
    downloader.SetMaxDownQueue(std::max(1u, std::thread::hardware_concurrency()));
    downloader.SetProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex, stepCount, groupName](
            const DownTool::DownToolProgress& progress)
        {
            const double subProgress = 0.65 + DownToolPhaseProgress(progress) * 0.35;
            UpdateProgress(taskId, progress.stage + " · " + groupName,
                           (static_cast<double>(stepIndex) + subProgress) /
                               static_cast<double>(stepCount),
                           progress.completed, progress.total);
        });
    SetActiveDownloader(&downloader);
    DownTool::AmapVectorPngDownloadResult result;
    try
    {
        result = downloader.DownloadPngTiles(options);
    }
    catch (...)
    {
        ClearActiveDownloader();
        throw;
    }
    ClearActiveDownloader();
    if (IsCancellationRequested())
    {
        return false;
    }
    if (!result.success)
    {
        error = result.error;
        return false;
    }
    if (configuration.exportTiles &&
        !std::filesystem::exists(tileOutputPath / "meta.json"))
    {
        error = "GIS XYZ 瓦片未生成完整元数据：" + groupName;
        return false;
    }
    if (configuration.exportMergedImage && !std::filesystem::exists(mergedImagePath))
    {
        error = "GIS 合并大图未生成：" + groupName;
        return false;
    }
    if (exportFbx && !std::filesystem::exists(fbxOutputPath / "manifest.json"))
    {
        error = "GIS FBX 贴图清单未生成：" + groupName;
        return false;
    }

    DownTool::AmapVectorDecodedData satelliteRoofBuildingData;
    if (satelliteRoofBuildingFbx)
    {
        satelliteRoofBuildingData = FilterBuildingFbxData(
            result.vectorDownload.decodedData,
            categories,
            [this] { return IsCancellationRequested(); });
        if (IsCancellationRequested())
        {
            return false;
        }
    }

    if (mode == GisExportMode::OverlayOnSatellite)
    {
        const ExportCancellationCallback cancellationRequested =
            [this] { return IsCancellationRequested(); };
        for (const SatelliteExportSource& source : configuration.satelliteSources)
        {
            if (IsCancellationRequested())
            {
                return false;
            }
            const std::filesystem::path satelliteRoot = SatelliteOutputRoot(
                configuration, queuedExport.taskId, source);
            const std::filesystem::path overlayRoot = ExportGisOverlayInputRoot(
                configuration.exportTiles, tileOutputPath, temporaryRoot);
            const std::filesystem::path compositeRoot = OverlayOutputRoot(
                configuration, queuedExport.taskId, source);
            if (!ExportImageComposer::CopyDirectory(
                    satelliteRoot, compositeRoot, error, cancellationRequested))
            {
                return false;
            }
            if ((configuration.exportTiles || configuration.exportFbx) &&
                !ExportImageComposer::CompositeXyzTileTree(
                    compositeRoot / "XYZ", overlayRoot,
                    compositeRoot / "XYZ", error, cancellationRequested))
            {
                return false;
            }
            if (configuration.exportFbx &&
                !ExportImageComposer::CompositeTileTree(
                    compositeRoot / "FBX", fbxOutputPath,
                    compositeRoot / "FBX", error, cancellationRequested))
            {
                return false;
            }
            if (configuration.exportFbx && satelliteRoofBuildingFbx &&
                !satelliteRoofBuildingData.features.empty() &&
                !ExportSatelliteRoofBuildingFbxToRoot(
                    configuration,
                    satelliteRoofBuildingData,
                    elevationData,
                    compositeRoot / "FBX",
                    cancellationRequested,
                    error))
            {
                return false;
            }
            if (configuration.exportFbx && configuration.exportBuildingFbx &&
                !satelliteRoofBuildingFbx &&
                !CopyBuildingFbxArtifacts(fbxOutputPath, compositeRoot / "FBX", error))
            {
                return false;
            }
            if (configuration.exportMergedImage)
            {
                const std::filesystem::path satelliteImage = satelliteRoot /
                    std::filesystem::u8path(
                        "merged." + configuration.mergedImageFormat);
                const std::filesystem::path gisImage = mergedImagePath;
                const std::filesystem::path compositeImage = compositeRoot / "merged.png";
                if (!ExportImageComposer::CompositeImage(
                        satelliteImage, gisImage, compositeImage, error,
                        cancellationRequested))
                {
                    return false;
                }
                if (configuration.mergedImageFormat != "png")
                {
                    std::error_code removeError;
                    std::filesystem::remove(
                        compositeRoot /
                            std::filesystem::u8path(
                                "merged." + configuration.mergedImageFormat),
                        removeError);
                }
            }
        }
    }
    else if (satelliteRoofBuildingFbx && !satelliteRoofBuildingData.features.empty())
    {
        const ExportCancellationCallback cancellationRequested =
            [this] { return IsCancellationRequested(); };
        for (const SatelliteExportSource& source : configuration.satelliteSources)
        {
            if (IsCancellationRequested())
            {
                return false;
            }
            const std::filesystem::path satelliteFbxRoot = SatelliteOutputRoot(
                configuration,
                queuedExport.taskId,
                source) / "FBX";
            if (!ExportSatelliteRoofBuildingFbxToRoot(
                    configuration,
                    satelliteRoofBuildingData,
                    elevationData,
                    satelliteFbxRoot,
                    cancellationRequested,
                    error))
            {
                return false;
            }
        }
    }

    UpdateProgress(queuedExport.taskId, "GIS 导出完成 · " + groupName,
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunBuildingFbxExport(
    const QueuedExport& queuedExport,
    const std::shared_ptr<DownTool::CopernicusDemData>& elevationData,
    size_t stepIndex, size_t stepCount,
    std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return false;
    }

    DownTool::AmapVectorDownloadOptions options;
    options.centerWgs84 = configuration.centerWgs84;
    options.radiusMeters = configuration.radiusMeters;
    options.polygonsWgs84 = configuration.polygonsWgs84;
    options.SetGisSpatialRange(ExportGisSpatialRange(configuration));
    options.zoom = configuration.zoom;
    options.includeBaseLayers = true;
    options.includeLabels = false;
    ConfigureGisVectorSources(options, configuration.GetExecutionVectorSources());
    options.maxConcurrency = 0;
    const auto sourceFilter = options.GetFeatureFilter();
    options.SetFeatureFilter(
        [sourceFilter, selected = IsGisCategorySelected(
             configuration.gisCategories, DownTool::AmapVectorRenderCategory::Building)](
            const DownTool::AmapVectorFeature& feature)
        {
            return selected && (!sourceFilter || sourceFilter(feature)) &&
                DownTool::IsAmapVectorBuildingWithHeight(feature);
        });
    options.SetProcessingProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex, stepCount](
            const DownTool::GisProcessingProgress& progress)
        {
            const double processing = GisProcessingRatio(progress);
            UpdateProgress(taskId, DownTool::GisProcessingStageName(progress.GetStage()),
                (static_cast<double>(stepIndex) + 0.7 + processing * 0.12) /
                    static_cast<double>(stepCount),
                progress.GetCompleted(), progress.GetTotal());
        });
    options.cancellationRequested = [this] { return IsCancellationRequested(); };
    options.progressCallback =
        [this, taskId = queuedExport.taskId, stepIndex, stepCount](
            const DownTool::AmapVectorDownloadProgress& progress)
        {
            const double subProgress =
                ProgressRatio(progress.completedTaskCount, progress.totalTaskCount) * 0.7;
            UpdateProgress(
                taskId, "下载 GIS 建筑 PBF",
                (static_cast<double>(stepIndex) + subProgress) /
                    static_cast<double>(stepCount),
                progress.completedTaskCount, progress.totalTaskCount);
        };

    DownTool::AmapVectorDownloader downloader;
    const DownTool::AmapVectorDownloadResult downloadResult = downloader.Download(options);
    if (IsCancellationRequested())
    {
        return false;
    }
    if (!downloadResult.success)
    {
        error = downloadResult.error;
        return false;
    }

    const std::function<bool()> cancellationRequested =
        [this] { return IsCancellationRequested(); };
    const DownTool::AmapVectorDecodedData& buildingData = downloadResult.decodedData;
    if (IsCancellationRequested())
    {
        return false;
    }
    if (buildingData.features.empty())
    {
        error = "所选范围没有可导出的带高度 GIS 建筑。";
        return false;
    }

    const std::string outputModeName = BuildingFbxModeName(configuration);
    UpdateProgress(
        queuedExport.taskId, "导出" + outputModeName,
        (static_cast<double>(stepIndex) + 0.85) / static_cast<double>(stepCount),
        0, 1);

    if (UsesSatelliteRoofTexture(configuration))
    {
        for (const SatelliteExportSource& source : configuration.satelliteSources)
        {
            if (IsCancellationRequested())
            {
                return false;
            }
            const std::filesystem::path satelliteFbxRoot = SatelliteOutputRoot(
                configuration,
                queuedExport.taskId,
                source) / "FBX";
            if (!ExportSatelliteRoofBuildingFbxToRoot(
                    configuration,
                    buildingData,
                    elevationData,
                    satelliteFbxRoot,
                    cancellationRequested,
                    error))
            {
                return false;
            }
        }
    }
    else
    {
        const std::filesystem::path outputRoot =
            configuration.buildingFbxOutputOverride.empty() ?
            GisOutputRoot(configuration, queuedExport.taskId, outputModeName, "建筑") /
                "FBX" :
            configuration.buildingFbxOutputOverride;
        if (!ExportBuildingFbxDataToRoot(
                configuration,
                buildingData,
                elevationData,
                outputRoot,
                {},
                cancellationRequested,
                error))
        {
            return false;
        }
    }
    UpdateProgress(queuedExport.taskId, outputModeName + " 导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   1, 1);
    return true;
}

std::optional<DownTool::AmapVectorDecodedData> DomDownToolPreview::ExportTaskManager::
    DownloadGisVectorData(const QueuedExport& queuedExport, size_t stepIndex, size_t stepCount,
                          std::string& error)
{
    const ExportConfiguration& configuration = queuedExport.configuration;
    if (IsCancellationRequested())
    {
        return std::nullopt;
    }
    DownTool::AmapVectorDownloadOptions options;
    options.centerWgs84 = configuration.centerWgs84;
    options.radiusMeters = configuration.radiusMeters;
    options.polygonsWgs84 = configuration.polygonsWgs84;
    options.SetGisSpatialRange(ExportGisSpatialRange(configuration));
    options.zoom = configuration.zoom;
    options.includeBaseLayers = configuration.includeBaseLayersOverride.value_or(true);
    options.includeLabels = configuration.includeLabelsOverride.value_or(
        configuration.gisCategories.contains(DownTool::AmapVectorRenderCategory::Point));
    ConfigureGisVectorSources(options, configuration.GetExecutionVectorSources());
    options.maxConcurrency = 0;
    options.cancellationRequested = [this] { return IsCancellationRequested(); };
    options.SetProcessingProgressCallback(
        [this, taskId = queuedExport.taskId, stepIndex,
         stepCount](const DownTool::GisProcessingProgress& progress) {
            const double processing = GisProcessingRatio(progress);
            UpdateProgress(taskId, DownTool::GisProcessingStageName(progress.GetStage()),
                           (static_cast<double>(stepIndex) + 0.7 + processing * 0.25) /
                               static_cast<double>(stepCount),
                           progress.GetCompleted(), progress.GetTotal());
        });
    options.progressCallback = [this, taskId = queuedExport.taskId, stepIndex,
                                stepCount](const DownTool::AmapVectorDownloadProgress& progress) {
        UpdateProgress(taskId, "下载 GIS 矢量数据",
                       (static_cast<double>(stepIndex) +
                        ProgressRatio(progress.completedTaskCount, progress.totalTaskCount) * 0.7) /
                           static_cast<double>(stepCount),
                       progress.completedTaskCount, progress.totalTaskCount);
    };

    DownTool::AmapVectorDownloader downloader;
    const DownTool::AmapVectorDownloadResult result = downloader.Download(options);
    if (IsCancellationRequested())
    {
        return std::nullopt;
    }
    if (!result.success)
    {
        error = result.error;
        return std::nullopt;
    }
    const std::function<bool()> cancellationRequested = [this] {
        return IsCancellationRequested();
    };
    auto filteredData =
        FilterGisData(result.decodedData, configuration.gisCategories, cancellationRequested);
    if (IsCancellationRequested())
    {
        return std::nullopt;
    }
    return filteredData;
}

bool DomDownToolPreview::ExportTaskManager::RunGeoJsonExport(const QueuedExport& queuedExport,
                                                             size_t stepIndex, size_t stepCount,
                                                             std::string& error)
{
    const auto filteredData = DownloadGisVectorData(queuedExport, stepIndex, stepCount, error);
    if (!filteredData)
    {
        return false;
    }
    const ExportConfiguration& configuration = queuedExport.configuration;
    const std::filesystem::path outputRoot =
        GisOutputRoot(configuration, queuedExport.taskId, "GeoJSON", "所选图层");
    const std::filesystem::path outputFile = configuration.geoJsonOutputFileOverride.empty()
                                                 ? outputRoot / "数据.geojson"
                                                 : configuration.geoJsonOutputFileOverride;
    const std::function<bool()> cancellationRequested = [this] {
        return IsCancellationRequested();
    };
    if (!DownTool::AmapVectorGeoJsonWriter::WriteFile(*filteredData, outputFile, error,
                                                      cancellationRequested))
    {
        return false;
    }
    UpdateProgress(queuedExport.taskId, "GeoJSON 导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount), 1, 1);
    return true;
}

bool DomDownToolPreview::ExportTaskManager::RunShapefileExport(const QueuedExport& queuedExport,
                                                               size_t stepIndex, size_t stepCount,
                                                               std::string& error)
{
    const auto decoded = DownloadGisVectorData(queuedExport, stepIndex, stepCount, error);
    if (!decoded)
    {
        return false;
    }
    UpdateProgress(queuedExport.taskId, "写入 SHP",
                   (static_cast<double>(stepIndex) + 0.95) / static_cast<double>(stepCount), 0,
                   decoded->features.size());
    const auto outputDirectory =
        ExportTaskOutputRoot(queuedExport.configuration, queuedExport.taskId);
    if (!DownTool::AmapVectorShapefileWriter::WriteDirectory(
            *decoded, outputDirectory, error, [this] { return IsCancellationRequested(); }))
    {
        return false;
    }
    UpdateProgress(queuedExport.taskId, "SHP 导出完成",
                   static_cast<double>(stepIndex + 1) / static_cast<double>(stepCount),
                   decoded->features.size(), decoded->features.size());
    return true;
}

void DomDownToolPreview::ExportTaskManager::UpdateProgress(
    std::string_view taskId, std::string stage, double progress,
    uint64_t completed, uint64_t total)
{
    bool notifyChanged = false;
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end() || record->state != ExportTaskState::Running)
        {
            return;
        }
        if (cancelActive_.load(std::memory_order_relaxed))
        {
            return;
        }
        const bool stageChanged = record->stage != stage;
        record->stage = std::move(stage);
        record->progress = std::max(record->progress, std::clamp(progress, 0., 1.));
        record->completed = completed;
        record->total = total;
        const auto now = std::chrono::steady_clock::now();
        const bool reachedEnd = total != 0 && completed >= total;
        notifyChanged = stageChanged || reachedEnd ||
            lastProgressNotificationTime_ == std::chrono::steady_clock::time_point{} ||
            now - lastProgressNotificationTime_ >= kProgressNotificationInterval;
        if (notifyChanged)
        {
            lastProgressNotificationTime_ = now;
        }
    }
    if (!notifyChanged)
    {
        return;
    }
    SaveRecords(false);
    NotifyChanged();
}

void DomDownToolPreview::ExportTaskManager::FinishTask(
    std::string_view taskId, ExportTaskState state, std::string error)
{
    {
        const std::lock_guard lock(mutex_);
        const auto record = std::find_if(
            records_.begin(), records_.end(),
            [taskId](const ExportTaskRecord& item) { return item.id == taskId; });
        if (record == records_.end())
        {
            return;
        }
        record->state = state;
        record->stage = ExportTaskStateName(state);
        record->finishedAt = CurrentTimeText();
        record->error = std::move(error);
        if (state == ExportTaskState::Succeeded)
        {
            record->progress = 1.;
        }
    }
    SaveRecords(true);
    NotifyChanged();
}

void DomDownToolPreview::ExportTaskManager::SetActiveDownloader(
    DownTool::DownToolBase* downloader)
{
    const std::lock_guard lock(mutex_);
    activeDownloader_ = downloader;
}

void DomDownToolPreview::ExportTaskManager::ClearActiveDownloader()
{
    const std::lock_guard lock(mutex_);
    activeDownloader_ = nullptr;
}

void DomDownToolPreview::ExportTaskManager::LoadRecords()
{
    std::ifstream input(recordsPath_, std::ios::binary);
    if (!input.is_open())
    {
        return;
    }
    const std::string json((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    input.close();
    const Json root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        return;
    }

    const auto tasks = root.find("tasks");
    if (tasks == root.end() || !tasks->is_array())
    {
        return;
    }
    bool recordsChanged = false;
    for (const Json& item : *tasks)
    {
        ExportTaskRecord record;
        record.id = GetJsonString(item, "id");
        record.title = GetJsonString(item, "title");
        record.detail = GetJsonString(item, "detail");
        record.outputPath = GetJsonString(item, "outputPath");
        record.createdAt = GetJsonString(item, "createdAt");
        record.finishedAt = GetJsonString(item, "finishedAt");
        record.state = ParseTaskState(GetJsonString(item, "state"));
        record.stage = GetJsonString(item, "stage");
        record.progress = GetJsonDouble(item, "progress");
        record.completed = static_cast<uint64_t>(
            std::max(0., GetJsonDouble(item, "completed")));
        record.total = static_cast<uint64_t>(
            std::max(0., GetJsonDouble(item, "total")));
        record.error = GetJsonString(item, "error");
        const auto configuration = item.find("configuration");
        if (configuration != item.end())
        {
            record.configuration = ExportConfigurationFromJson(*configuration);
        }
        if (record.id.empty())
        {
            continue;
        }
        if (record.state == ExportTaskState::Queued ||
            record.state == ExportTaskState::Running)
        {
            record.state = ExportTaskState::Failed;
            record.stage = "已中断";
            record.finishedAt = CurrentTimeText();
            record.error = "DomDownToolPreview 上次退出时任务尚未完成。";
            recordsChanged = true;
        }
        records_.push_back(std::move(record));
    }
    if (recordsChanged)
    {
        SaveRecords(true);
    }
}

void DomDownToolPreview::ExportTaskManager::SaveRecords(bool force)
{
    const std::lock_guard fileLock(recordsFileMutex_);
    std::vector<ExportTaskRecord> records;
    {
        const std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastSaveTime_ != std::chrono::steady_clock::time_point{} &&
            now - lastSaveTime_ < std::chrono::milliseconds(500))
        {
            return;
        }
        lastSaveTime_ = now;
        records = records_;
    }

    Json tasks = Json::array();
    for (const ExportTaskRecord& record : records)
    {
        Json task = {
            {"id", record.id},
            {"title", record.title},
            {"detail", record.detail},
            {"outputPath", record.outputPath},
            {"createdAt", record.createdAt},
            {"finishedAt", record.finishedAt},
            {"state", TaskStateValue(record.state)},
            {"stage", record.stage},
            {"progress", record.progress},
            {"completed", static_cast<double>(record.completed)},
            {"total", static_cast<double>(record.total)},
            {"error", record.error}};
        if (record.configuration.has_value())
        {
            task["configuration"] = ExportConfigurationToJson(*record.configuration);
        }
        tasks.push_back(std::move(task));
    }
    const Json root = {{"version", 1}, {"tasks", std::move(tasks)}};

    std::error_code directoryError;
    if (!recordsPath_.parent_path().empty())
    {
        std::filesystem::create_directories(recordsPath_.parent_path(), directoryError);
    }
    if (directoryError)
    {
        return;
    }
    std::ofstream output(recordsPath_, std::ios::binary | std::ios::trunc);
    if (output.is_open())
    {
        output << root.dump(2) << '\n';
    }
}

void DomDownToolPreview::ExportTaskManager::NotifyChanged() const
{
    if (changeCallback_)
    {
        changeCallback_();
    }
}

bool DomDownToolPreview::ExportTaskManager::IsCancellationRequested() const
{
    return cancelActive_.load(std::memory_order_relaxed);
}
