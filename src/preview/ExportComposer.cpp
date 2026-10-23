#include "preview/PreviewController.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "skui_runtime.h"

#include "preview/ExportDistanceLodUi.h"

namespace
{
using DomDownToolPreview::ExportComponent;
using DomDownToolPreview::ExportComponentKind;
using DomDownToolPreview::ExportContentGroup;
using DomDownToolPreview::ExportSourceEntry;
using DomDownToolPreview::ExportSourceKind;
using DomDownToolPreview::GisOverlayCategory;

struct ImageryCatalogItem
{
    const char* id;
    const char* name;
};

constexpr ImageryCatalogItem kImageryCatalog[] = {
    {"gaode.satellite", "高德卫星"},
    {"gaode.electronic", "高德电子"},
    {"gaode.roads", "高德路网"},
    {"baidu.satellite", "百度卫星"},
    {"baidu.electronic", "百度电子"},
    {"tencent.satellite", "腾讯卫星"},
    {"tencent.electronic", "腾讯电子"},
    {"tianditu.satellite", "天地图影像"},
    {"tianditu.multidate", "天地图多时相"},
    {"google.satellite", "Google 卫星"},
    {"bing.satellite", "Bing 卫星"},
    {"arcgis.satellite", "ArcGIS 影像"}};

struct GisCatalogItem
{
    const char* id;
    const char* name;
    DownTool::AmapVectorRenderCategory category;
    GisOverlayCategory preview;
};

constexpr GisCatalogItem kGisCatalog[] = {
    {"gis.road", "道路", DownTool::AmapVectorRenderCategory::Road, GisOverlayCategory::Road},
    {"gis.building", "建筑", DownTool::AmapVectorRenderCategory::Building,
     GisOverlayCategory::Building},
    {"gis.water", "水", DownTool::AmapVectorRenderCategory::Water, GisOverlayCategory::Water},
    {"gis.point", "点", DownTool::AmapVectorRenderCategory::Point, GisOverlayCategory::Point},
    {"gis.green", "绿地", DownTool::AmapVectorRenderCategory::Green, GisOverlayCategory::Green},
    {"gis.boundary", "边界", DownTool::AmapVectorRenderCategory::Boundary,
     GisOverlayCategory::Boundary},
    {"gis.highway", "高速", DownTool::AmapVectorRenderCategory::Highway,
     GisOverlayCategory::Highway},
    {"gis.subway", "地铁", DownTool::AmapVectorRenderCategory::Subway,
     GisOverlayCategory::Subway},
    {"gis.other", "其他", DownTool::AmapVectorRenderCategory::Other, GisOverlayCategory::Other}};

std::string EscapeHtml(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

const ImageryCatalogItem* FindImagery(std::string_view id)
{
    const auto item = std::ranges::find_if(
        kImageryCatalog, [id](const ImageryCatalogItem& entry) { return entry.id == id; });
    return item == std::end(kImageryCatalog) ? nullptr : &*item;
}

const GisCatalogItem* FindGis(std::string_view id)
{
    const auto item = std::ranges::find_if(
        kGisCatalog, [id](const GisCatalogItem& entry) { return entry.id == id; });
    return item == std::end(kGisCatalog) ? nullptr : &*item;
}

const GisCatalogItem* FindGis(DownTool::AmapVectorRenderCategory category)
{
    const auto item = std::ranges::find_if(
        kGisCatalog,
        [category](const GisCatalogItem& entry) { return entry.category == category; });
    return item == std::end(kGisCatalog) ? nullptr : &*item;
}

std::string KindName(ExportComponentKind kind)
{
    switch (kind)
    {
    case ExportComponentKind::MergedImage:
        return "大图";
    case ExportComponentKind::DomGeoTiff:
        return "DOM GeoTIFF";
    case ExportComponentKind::DemGeoTiff:
        return "DEM GeoTIFF";
    case ExportComponentKind::StandardXyz:
        return "标准 XYZ";
    case ExportComponentKind::RawTiles:
        return "原始瓦片";
    case ExportComponentKind::BasePlate:
        return "底板模型";
    case ExportComponentKind::BuildingModel:
        return "建筑模型";
    case ExportComponentKind::CesiumTerrain:
        return "Cesium 地形";
    case ExportComponentKind::CesiumBuildings:
        return "3D Tiles 建筑";
    case ExportComponentKind::GeoJson:
        return "GeoJSON";
    case ExportComponentKind::Shapefile:
        return "SHP";
    }
    return "组件";
}

std::string KindValue(ExportComponentKind kind)
{
    switch (kind)
    {
    case ExportComponentKind::MergedImage:
        return "merged-image";
    case ExportComponentKind::DomGeoTiff:
        return "dom-geotiff";
    case ExportComponentKind::DemGeoTiff:
        return "dem-geotiff";
    case ExportComponentKind::StandardXyz:
        return "standard-xyz";
    case ExportComponentKind::RawTiles:
        return "raw-tiles";
    case ExportComponentKind::BasePlate:
        return "base-plate";
    case ExportComponentKind::BuildingModel:
        return "building-model";
    case ExportComponentKind::CesiumTerrain:
        return "cesium-terrain";
    case ExportComponentKind::CesiumBuildings:
        return "cesium-buildings";
    case ExportComponentKind::GeoJson:
        return "geojson";
    case ExportComponentKind::Shapefile:
        return "shapefile";
    }
    return {};
}

std::optional<ExportComponentKind> KindFrom(std::string_view value)
{
    if (value == "merged-image")
    {
        return ExportComponentKind::MergedImage;
    }
    if (value == "dom-geotiff")
    {
        return ExportComponentKind::DomGeoTiff;
    }
    if (value == "dem-geotiff")
    {
        return ExportComponentKind::DemGeoTiff;
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
    if (value == "shapefile")
    {
        return ExportComponentKind::Shapefile;
    }
    if (value == "cesium-terrain")
    {
        return ExportComponentKind::CesiumTerrain;
    }
    if (value == "cesium-buildings")
    {
        return ExportComponentKind::CesiumBuildings;
    }
    return std::nullopt;
}

bool AllowsImagery(ExportComponentKind kind)
{
    return kind != ExportComponentKind::GeoJson &&
        kind != ExportComponentKind::Shapefile &&
        kind != ExportComponentKind::BuildingModel &&
        kind != ExportComponentKind::CesiumBuildings &&
        kind != ExportComponentKind::CesiumTerrain &&
        kind != ExportComponentKind::DemGeoTiff;
}

bool AllowsGis(ExportComponentKind kind)
{
    return kind != ExportComponentKind::RawTiles &&
        kind != ExportComponentKind::CesiumTerrain &&
        kind != ExportComponentKind::DemGeoTiff;
}

std::string ShapeName(DownTool::FbxPlanetShape shape)
{
    switch (shape)
    {
    case DownTool::FbxPlanetShape::Sphere:
        return "球面";
    case DownTool::FbxPlanetShape::Both:
        return "平面+球面";
    case DownTool::FbxPlanetShape::Plane:
        return "平面";
    }
    return "球面";
}

std::string FormatName(std::string_view format)
{
    return format == "jpg" ? "JPG" : "PNG";
}

std::string ColorHex(const DownTool::AmapVectorColor& color)
{
    return fmt::format("#{:02x}{:02x}{:02x}", color.red, color.green, color.blue);
}

std::optional<DownTool::AmapVectorColor> ParseColorHex(std::string_view value)
{
    if (value.size() != 7 || value.front() != '#')
    {
        return std::nullopt;
    }
    unsigned rgb = 0;
    const auto [pointer, error] = std::from_chars(value.data() + 1, value.data() + value.size(),
                                                   rgb, 16);
    if (error != std::errc() || pointer != value.data() + value.size())
    {
        return std::nullopt;
    }
    return DownTool::AmapVectorColor{
        static_cast<uint8_t>((rgb >> 16) & 0xFF),
        static_cast<uint8_t>((rgb >> 8) & 0xFF),
        static_cast<uint8_t>(rgb & 0xFF), 255};
}

std::optional<uint32_t> ParseUnsigned(std::string_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    uint32_t parsed = 0;
    const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(),
                                                   parsed);
    if (error != std::errc() || pointer != value.data() + value.size())
    {
        return std::nullopt;
    }
    return parsed;
}

std::optional<double> ParsePositive(std::string_view value)
{
    try
    {
        size_t parsedLength = 0;
        const double parsed = std::stod(std::string(value), &parsedLength);
        if (parsedLength != value.size() || !std::isfinite(parsed) || parsed <= 0.)
        {
            return std::nullopt;
        }
        return parsed;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

bool ParseAction(std::string_view id, std::string& verb, std::string& first, std::string& second)
{
    constexpr std::string_view kPrefix = "export-act-";
    if (!id.starts_with(kPrefix))
    {
        return false;
    }
    std::string_view rest = id.substr(kPrefix.size());
    const size_t firstSplit = rest.find("__");
    verb = std::string(rest.substr(0, firstSplit));
    if (firstSplit == std::string_view::npos)
    {
        return !verb.empty();
    }
    rest.remove_prefix(firstSplit + 2);
    const size_t secondSplit = rest.find("__");
    first = std::string(rest.substr(0, secondSplit));
    if (secondSplit != std::string_view::npos)
    {
        second = std::string(rest.substr(secondSplit + 2));
    }
    return !verb.empty();
}

void ConsiderSerial(std::string_view id, uint64_t& serial)
{
    if (id.size() < 2 || (id.front() != 'g' && id.front() != 'c'))
    {
        return;
    }
    uint64_t value = 0;
    const auto [pointer, error] = std::from_chars(id.data() + 1, id.data() + id.size(), value);
    if (error == std::errc() && pointer == id.data() + id.size())
    {
        serial = std::max(serial, value + 1);
    }
}

bool ComponentHasSource(const ExportComponent& component, std::string_view sourceId,
                        std::string_view multidateKey)
{
    return std::ranges::any_of(
        component.sources,
        [&](const ExportSourceEntry& source)
        {
            return source.id == sourceId && source.multidateKey == multidateKey;
        });
}

std::string JoinedSourceNames(const ExportComponent& component)
{
    std::string names;
    for (const ExportSourceEntry& source : component.sources)
    {
        if (!names.empty())
        {
            names += "、";
        }
        names += source.name.empty() ? source.id : source.name;
    }
    return names.empty() ? "未选择图源" : names;
}

std::string ComponentSummary(const ExportComponent& component)
{
    const std::string names = JoinedSourceNames(component);
    switch (component.kind)
    {
    case ExportComponentKind::MergedImage:
        return fmt::format("{} · {} · {}", names, FormatName(component.imageFormat),
                           component.maxEdge);
    case ExportComponentKind::DomGeoTiff:
        return fmt::format("{} · EPSG:3857 · 原始分辨率", names);
    case ExportComponentKind::DemGeoTiff:
        return fmt::format(
            "EPSG:3857 · {:.0f} 米",
            component.elevationSampleMeters);
    case ExportComponentKind::StandardXyz:
        return fmt::format("{} · {}", names, component.xyzTileSize);
    case ExportComponentKind::RawTiles:
        return names;
    case ExportComponentKind::BasePlate:
        if (component.exportElevation)
        {
            return fmt::format("{} · {} · {} · 高程 {:.0f} 米", names, ShapeName(component.shape),
                               component.textureSize, component.elevationSampleMeters) +
                   (component.GetFbxPerTile() ? " · 每块独立 FBX" : "");
        }
        return fmt::format("{} · {} · {}", names, ShapeName(component.shape),
                           component.textureSize) +
               (component.GetFbxPerTile() ? " · 每块独立 FBX" : "");
    case ExportComponentKind::BuildingModel:
        if (component.roofTextureMode == DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
        {
            return fmt::format("建筑 · {} · 屋顶引用底板模型",
                               ShapeName(component.shape));
        }
        return fmt::format("建筑 · {}", ShapeName(component.shape));
    case ExportComponentKind::CesiumTerrain:
        return fmt::format("quantized-mesh · {:.0f} 米", component.elevationSampleMeters);
    case ExportComponentKind::CesiumBuildings:
        return fmt::format("{} 层 · {:.0f} 米分块", component.GetCesiumLodLevels(),
            component.GetBuildingChunkSizeMeters());
    case ExportComponentKind::GeoJson:
    case ExportComponentKind::Shapefile:
        return names;
    }
    return names;
}

std::string OptionTags(const std::vector<std::pair<std::string, std::string>>& options,
                       std::string_view selected)
{
    std::string html;
    for (const auto& [value, label] : options)
    {
        html += fmt::format("<option value=\"{}\"{}>{}</option>", EscapeHtml(value),
                            value == selected ? " selected" : "", EscapeHtml(label));
    }
    return html;
}
} // namespace

void DomDownToolPreview::PreviewController::ResetExportComposer(
    const ExportConfiguration* configuration)
{
    exportContentGroups_.clear();
    exportSelectedComponentId_.clear();
    exportComponentMenuGroupId_.clear();
    exportSourceMenuComponentId_.clear();
    exportComposerSerial_ = 1;
    lastExportGroupListHtml_.clear();
    lastExportParamPanelHtml_.clear();
    lastExportParamPanelComponentId_.clear();

    const auto nextId = [this](char prefix)
    {
        return std::string(1, prefix) + std::to_string(exportComposerSerial_++);
    };
    const auto colorFor = [this](DownTool::AmapVectorRenderCategory category)
    {
        const auto saved = exportGisColors_.find(category);
        if (saved != exportGisColors_.end())
        {
            return saved->second;
        }
        const GisCatalogItem* item = FindGis(category);
        return item == nullptr ? DownTool::AmapVectorColor{} :
            renderer_.GetGisOverlayColor(item->preview);
    };
    const auto makeGis = [&](const GisCatalogItem& item)
    {
        ExportSourceEntry source;
        source.id = item.id;
        source.name = item.name;
        source.kind = ExportSourceKind::Gis;
        source.gisCategory = item.category;
        source.color = colorFor(item.category);
        return source;
    };
    const auto makeImagery = [this](const SatelliteExportSource& saved)
    {
        ExportSourceEntry source;
        source.id = saved.id;
        const ImageryCatalogItem* catalog = FindImagery(saved.id);
        source.name = saved.name.empty() && catalog != nullptr ? catalog->name : saved.name;
        if (source.name.empty() && catalog != nullptr)
        {
            source.name = catalog->name;
        }
        if (source.name.empty())
        {
            source.name = saved.id;
        }
        source.kind = ExportSourceKind::Imagery;
        source.cachePath = saved.cachePath;
        source.multidateYear = saved.multidateYear;
        source.multidateKey = saved.multidateKey;
        source.multidateGbCode = saved.multidateGbCode;
        source.multidateRegionName = saved.multidateRegionName;
        source.multidateCookie = saved.multidateCookie;
        return source;
    };

    if (configuration != nullptr && !configuration->groups.empty())
    {
        exportContentGroups_ = configuration->groups;
        for (auto& group : exportContentGroups_)
        {
            for (auto& component : group.components)
            {
                for (auto& source : component.sources)
                {
                    if (source.GetShapefile().GetPath().empty())
                    {
                        continue;
                    }
                    const auto existing = std::ranges::find_if(
                        importedShapefileSources_, [&source](const auto& item)
                        { return item.GetShapefile().GetPath() == source.GetShapefile().GetPath(); });
                    if (existing != importedShapefileSources_.end())
                    {
                        source.id = existing->id;
                    }
                    else
                    {
                        while (std::ranges::any_of(
                            importedShapefileSources_,
                            [&source](const auto& item) { return item.id == source.id; }))
                        {
                            source.id += "x";
                        }
                        importedShapefileSources_.push_back(source);
                    }
                    DownTool::ShapefileInfo info;
                    std::string ignored;
                    if (DownTool::Wgs84ShapefileReader::Inspect(
                            source.GetShapefile().GetPath(), info, ignored))
                    {
                        importedShapefileInfo_[source.GetShapefile().GetPath()] = std::move(info);
                    }
                }
            }
        }
        SyncShapefilePreview();
    }
    else if (configuration != nullptr)
    {
        ExportContentGroup group;
        group.id = nextId('g');
        group.name = "当前预览";
        group.enabled = true;
        std::vector<ExportSourceEntry> imagery;
        for (const SatelliteExportSource& source : configuration->satelliteSources)
        {
            if (FindImagery(source.id) != nullptr)
            {
                imagery.push_back(makeImagery(source));
            }
        }
        std::vector<ExportSourceEntry> gis;
        const bool allGis = configuration->gisCategories.contains(
            DownTool::AmapVectorRenderCategory::All);
        for (const GisCatalogItem& item : kGisCatalog)
        {
            if (allGis || configuration->gisCategories.contains(item.category))
            {
                gis.push_back(makeGis(item));
            }
        }
        const bool overlay = configuration->gisModes.contains(GisExportMode::OverlayOnSatellite);
        const bool mergedGis = configuration->gisModes.contains(GisExportMode::MergeSelectedLayers);
        const bool separateGis = configuration->gisModes.contains(GisExportMode::SeparateLayers);
        const auto appendVisual = [&](ExportComponentKind kind, std::vector<ExportSourceEntry> sources)
        {
            if (sources.empty())
            {
                return;
            }
            ExportComponent component;
            component.id = nextId('c');
            component.kind = kind;
            component.sources = std::move(sources);
            component.imageFormat = configuration->mergedImageFormat;
            component.maxEdge = configuration->mergedImageResolution;
            component.textureSize = configuration->fbxTextureSize;
            component.textureFormat = configuration->fbxTextureFormat;
            component.exportElevation = configuration->exportTerrainHeight;
            component.elevationSampleMeters = configuration->fbxTerrainSampleRateMeters;
            component.SetTerrainMaximumTriangles(configuration->GetTerrainMaximumTriangles());
            component.SetFbxPerTile(configuration->GetFbxPerTile());
            component.SetDistanceLod(configuration->GetDistanceLod());
            group.components.push_back(std::move(component));
        };
        if (configuration->exportMergedImage || configuration->exportTiles ||
            configuration->exportFbx)
        {
            std::vector<ExportSourceEntry> combined = imagery;
            if (overlay)
            {
                combined.insert(combined.end(), gis.begin(), gis.end());
            }
            if (configuration->exportMergedImage)
            {
                appendVisual(ExportComponentKind::MergedImage, combined);
                if (mergedGis && !overlay)
                {
                    appendVisual(ExportComponentKind::MergedImage, gis);
                }
                if (separateGis)
                {
                    for (const ExportSourceEntry& source : gis)
                    {
                        appendVisual(ExportComponentKind::MergedImage, {source});
                    }
                }
            }
            if (configuration->exportTiles)
            {
                appendVisual(ExportComponentKind::StandardXyz, combined.empty() ? imagery : combined);
            }
            if (configuration->exportFbx)
            {
                appendVisual(ExportComponentKind::BasePlate, combined.empty() ? imagery : combined);
            }
        }
        if (configuration->exportGeoJson)
        {
            appendVisual(ExportComponentKind::GeoJson, gis);
        }
        if (configuration->exportBuildingFbx)
        {
            ExportComponent building;
            building.id = nextId('c');
            building.kind = ExportComponentKind::BuildingModel;
            if (const GisCatalogItem* item = FindGis(DownTool::AmapVectorRenderCategory::Building))
            {
                building.sources.push_back(makeGis(*item));
            }
            building.wallTexturePath = configuration->buildingWallTexturePath;
            building.roofTexturePath = configuration->buildingRoofTexturePath;
            building.wallTextureMode = configuration->buildingFbxWallTextureMode;
            building.roofTextureMode = configuration->buildingFbxRoofTextureMode;
            building.atlasImageFormat = configuration->buildingFbxAtlasImageFormat;
            building.layerHeightCentimeters = configuration->buildingLayerHeightCentimeters;
            building.SetBuildingChunkSizeMeters(configuration->GetBuildingChunkSizeMeters());
            building.maxWallMaterialsPerBucket = configuration->buildingMaxWallMaterialsPerBucket;
            building.textureRandomSeed = configuration->buildingTextureRandomSeed;
            building.followBaseElevation = configuration->exportTerrainHeight;
            if (building.roofTextureMode == DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
            {
                for (const ExportComponent& component : group.components)
                {
                    if (component.kind == ExportComponentKind::BasePlate)
                    {
                        building.basePlateComponentId = component.id;
                        break;
                    }
                }
            }
            group.components.push_back(std::move(building));
        }
        exportContentGroups_.push_back(std::move(group));
    }
    else
    {
        ExportContentGroup group;
        group.id = nextId('g');
        group.name = "当前预览";
        group.enabled = true;
        ExportComponent merged;
        merged.id = nextId('c');
        merged.kind = ExportComponentKind::MergedImage;
        const MapSourceSummary active = renderer_.activeSource();
        if (FindImagery(active.id) != nullptr)
        {
            SatelliteExportSource saved{active.id, "", active.cacheRoot};
            const ImageryCatalogItem* catalog = FindImagery(active.id);
            saved.name = catalog == nullptr ? active.name : catalog->name;
            if (active.id == "tianditu.multidate" &&
                tianDiTuMultidateSelectedValue_ != "latest")
            {
                const auto timeline = std::ranges::find_if(
                    tianDiTuMultidateTimelines_,
                    [this](const auto& item)
                    {
                        return item.key == tianDiTuMultidateSelectedValue_;
                    });
                if (timeline != tianDiTuMultidateTimelines_.end())
                {
                    const std::string year = timeline->year.empty() ? timeline->time :
                        timeline->year;
                    saved.name = "天地图多时相 " + year;
                    saved.multidateYear = year;
                    saved.multidateKey = timeline->key;
                    saved.multidateGbCode = tianDiTuMultidateRegionGbCode_;
                    saved.multidateRegionName = tianDiTuMultidateRegionName_;
                    saved.multidateCookie = tianDiTuMultidateCookie_;
                }
            }
            merged.sources.push_back(makeImagery(saved));
        }
        const GisOverlayCategories selected = renderer_.GetGisOverlayCategories();
        const bool allSelected = selected.contains(GisOverlayCategory::All);
        if (!selected.empty())
        {
            for (const GisCatalogItem& item : kGisCatalog)
            {
                if (allSelected || selected.contains(item.preview))
                {
                    merged.sources.push_back(makeGis(item));
                }
            }
        }
        for (const auto& source : importedShapefileSources_)
        {
            if (!hiddenShapefileSourceIds_.contains(source.id))
            {
                merged.sources.push_back(source);
            }
        }
        if (!merged.sources.empty())
        {
            group.components.push_back(std::move(merged));
        }
        exportContentGroups_.push_back(std::move(group));
    }

    for (const ExportContentGroup& group : exportContentGroups_)
    {
        ConsiderSerial(group.id, exportComposerSerial_);
        for (const ExportComponent& component : group.components)
        {
            ConsiderSerial(component.id, exportComposerSerial_);
            for (const ExportSourceEntry& source : component.sources)
            {
                if (source.id == "tianditu.multidate")
                {
                    exportMapSourceIds_.insert(source.id);
                }
            }
        }
    }
    for (const ExportContentGroup& group : exportContentGroups_)
    {
        if (!group.components.empty())
        {
            exportSelectedComponentId_ = group.components.front().id;
            break;
        }
    }
}

std::vector<DomDownToolPreview::ExportContentGroup>
DomDownToolPreview::PreviewController::ResolveExportGroups(std::string& error) const
{
    error.clear();
    std::vector<ExportContentGroup> groups = exportContentGroups_;
    const std::vector<MapSourceSummary> mapSources = renderer_.sources();
    for (ExportContentGroup& group : groups)
    {
        for (ExportComponent& component : group.components)
        {
            for (ExportSourceEntry& source : component.sources)
            {
                if (source.kind != ExportSourceKind::Imagery)
                {
                    continue;
                }
                const auto mapSource = std::ranges::find_if(
                    mapSources,
                    [&source](const MapSourceSummary& item) { return item.id == source.id; });
                if (mapSource == mapSources.end())
                {
                    error = "图源无效：" + source.name;
                    return {};
                }
                source.cachePath = source.id == "tianditu.multidate" ?
                    std::filesystem::u8path("cache/天地图多时相") : mapSource->cacheRoot;
                if (source.id == "tianditu.multidate" && !source.multidateKey.empty())
                {
                    source.multidateCookie = tianDiTuMultidateCookie_;
                    source.multidateGbCode = tianDiTuMultidateRegionGbCode_;
                    source.multidateRegionName = tianDiTuMultidateRegionName_;
                }
            }
        }
    }
    return groups;
}

namespace
{
std::string FieldSelect(std::string_view label, std::string_view controlId, std::string_view options)
{
    return fmt::format(
        "<label class=\"export-field-row\"><span>{}</span>"
        "<select id=\"{}\" class=\"export-select\">{}</select></label>",
        label, controlId, options);
}

std::string SegmentButton(std::string_view controlId, std::string_view label, bool active)
{
    return fmt::format(
        "<button id=\"{}\" class=\"export-segment-button{}\" type=\"button\" "
        "data-action=\"export-composer\">{}</button>",
        controlId, active ? " is-active" : "", label);
}

std::string ShapeButtons(const ExportComponent& component)
{
    std::string html;
    html += SegmentButton(
        "export-act-set-shape__" + component.id + "__plane",
        "平面",
        component.shape != DownTool::FbxPlanetShape::Sphere);
    html += SegmentButton(
        "export-act-set-shape__" + component.id + "__sphere",
        "球面",
        component.shape != DownTool::FbxPlanetShape::Plane);
    return html;
}

std::string ShapeSection(const ExportComponent& component)
{
    return "<div class=\"export-panel-section\"><span class=\"export-panel-label\">"
        "形态</span><div class=\"export-segment\">" +
        ShapeButtons(component) + "</div></div>";
}

std::string ShapeRow(const ExportComponent& component)
{
    return "<div class=\"export-field-row export-building-mode-row\"><span>形态</span>"
        "<div class=\"export-segment\">" +
        ShapeButtons(component) + "</div></div>";
}

std::string DirectoryField(std::string_view label, std::string_view inputId,
                           std::string_view browseId, std::string_view value)
{
    return fmt::format(
        "<div class=\"export-field-row export-directory-row\"><span>{}</span>"
        "<div class=\"export-directory-control\">"
        "<input id=\"{}\" type=\"text\" value=\"{}\">"
        "<button id=\"{}\" class=\"export-directory-button\" type=\"button\" "
        "data-action=\"export-composer\" title=\"选择{}\" aria-label=\"选择{}\">"
        "<svg viewBox=\"0 0 24 24\" aria-hidden=\"true\">"
        "<path fill=\"none\" stroke=\"#c9cfcc\" stroke-width=\"1.6\" "
        "stroke-linecap=\"round\" stroke-linejoin=\"round\" "
        "d=\"M3 7a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 0 1 2 2V17"
        "a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\"></path></svg></button>"
        "</div></div>",
        label, inputId, EscapeHtml(value), browseId, label, label);
}

std::string YearOptions(
    const ExportSourceEntry& source,
    const std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo>& timelines)
{
    std::string html = fmt::format(
        "<option value=\"\"{}>最新影像</option>",
        source.multidateKey.empty() ? " selected" : "");
    for (const auto& timeline : timelines)
    {
        if (timeline.key.empty())
        {
            continue;
        }
        const std::string year = timeline.year.empty() ? timeline.time : timeline.year;
        const std::string label = timeline.time.empty() ? year : timeline.time;
        html += fmt::format(
            "<option value=\"{}\"{}>{}</option>", EscapeHtml(timeline.key),
            timeline.key == source.multidateKey ? " selected" : "", EscapeHtml(label));
    }
    return html;
}

std::string SourceBlockHtml(
    const ExportComponent& component, std::string_view openSourceMenu,
    const std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo>& timelines,
    const std::vector<ExportSourceEntry>& importedSources,
    const std::map<std::filesystem::path, DownTool::ShapefileInfo>& importedInfo,
    bool allowAdd)
{
    std::string html = "<div class=\"export-source-list\">";
    for (size_t index = 0; index < component.sources.size(); ++index)
    {
        const ExportSourceEntry& source = component.sources[index];
        const bool fixedBuilding = component.kind == ExportComponentKind::BuildingModel ||
            component.kind == ExportComponentKind::CesiumBuildings;
        std::string color;
        if (source.kind == ExportSourceKind::Gis && !fixedBuilding)
        {
            color = fmt::format(
                "<input id=\"export-act-set-color__{}__{}\" class=\"export-color\" "
                "type=\"color\" value=\"{}\" aria-label=\"图源颜色\">",
                component.id, index, ColorHex(source.color));
        }
        const std::string remove = fmt::format(
            "<button id=\"export-act-remove-source__{}__{}\" class=\"export-icon-button\" "
            "type=\"button\" data-action=\"export-composer\" "
            "aria-label=\"移除图源\">×</button>",
            component.id, index);
        std::string year;
        if (source.id == "tianditu.multidate")
        {
            year = fmt::format(
                "<select id=\"export-act-set-year__{}__{}\" class=\"export-select\">{}</select>",
                component.id, index, YearOptions(source, timelines));
        }
        const std::string rowStart = fixedBuilding
            ? "<div class=\"export-source-row is-fixed\">"
            : "<div class=\"export-source-row\">";
        html += fmt::format(
            "{}{}<span class=\"export-source-name\">{}</span>{}{}</div>",
            rowStart, color, EscapeHtml(source.name), year, remove);
        if (!source.GetShapefile().GetPath().empty())
        {
            std::string categories;
            for (const auto& item : kGisCatalog)
            {
                if (fixedBuilding && item.category != DownTool::AmapVectorRenderCategory::Building)
                {
                    continue;
                }
                categories += fmt::format("<option value=\"{}\"{}>{}</option>",
                    item.id, item.category == source.gisCategory ? " selected" : "", item.name);
            }
            html += FieldSelect("GIS 类别",
                fmt::format("export-act-shp-category__{}__{}", component.id, index), categories);
            std::string fields = fmt::format(
                "<option value=\"\"{}>自动识别</option>"
                "<option value=\"-\"{}>不使用高度</option>",
                source.GetShapefile().GetHeightField().empty() ? " selected" : "",
                source.GetShapefile().GetHeightField() == "-" ? " selected" : "");
            const auto info = importedInfo.find(source.GetShapefile().GetPath());
            if (info != importedInfo.end())
            {
                for (const auto& field : info->second.GetNumericFields())
                {
                    fields += fmt::format("<option value=\"{}\"{}>{}</option>", EscapeHtml(field),
                        source.GetShapefile().GetHeightField() == field ? " selected" : "",
                        EscapeHtml(field));
                }
            }
            html += FieldSelect("高度字段（米）",
                fmt::format("export-act-shp-height__{}__{}", component.id, index), fields);
        }
    }
    html += "</div>";
    if (!allowAdd)
    {
        return html;
    }
    html += fmt::format(
        "<button id=\"export-act-open-sources__{}\" class=\"export-link-button\" "
        "type=\"button\" data-action=\"export-composer\">添加图源</button>",
        component.id);
    if (openSourceMenu != component.id)
    {
        return html;
    }
    std::string menu = "<div class=\"export-menu\">";
    bool any = false;
    if (AllowsImagery(component.kind))
    {
        for (const ImageryCatalogItem& item : kImageryCatalog)
        {
            if (std::string_view(item.id) != "tianditu.multidate" &&
                ComponentHasSource(component, item.id, {}))
            {
                continue;
            }
            any = true;
            menu += fmt::format(
                "<button id=\"export-act-add-source__{}__{}\" type=\"button\" "
                "data-action=\"export-composer\">{}</button>",
                component.id, item.id, item.name);
        }
    }
    if (AllowsGis(component.kind))
    {
        for (const GisCatalogItem& item : kGisCatalog)
        {
            if ((component.kind == ExportComponentKind::BuildingModel ||
                 component.kind == ExportComponentKind::CesiumBuildings) &&
                item.category != DownTool::AmapVectorRenderCategory::Building)
            {
                continue;
            }
            if (ComponentHasSource(component, item.id, {}))
            {
                continue;
            }
            any = true;
            menu += fmt::format(
                "<button id=\"export-act-add-source__{}__{}\" type=\"button\" "
                "data-action=\"export-composer\">{}</button>",
                component.id, item.id, item.name);
        }
        for (const auto& source : importedSources)
        {
            if (ComponentHasSource(component, source.id, {}) ||
                ((component.kind == ExportComponentKind::BuildingModel ||
                  component.kind == ExportComponentKind::CesiumBuildings) &&
                 source.gisCategory != DownTool::AmapVectorRenderCategory::Building))
            {
                continue;
            }
            any = true;
            menu += fmt::format(
                "<button id=\"export-act-add-source__{}__{}\" type=\"button\" "
                "data-action=\"export-composer\">{}</button>",
                component.id, source.id, EscapeHtml(source.name));
        }
    }
    if (!any)
    {
        menu += "<span class=\"export-menu-empty\">没有可添加的图源</span>";
    }
    menu += "</div>";
    return html + menu;
}


std::string PanelBodyHtml(
    const std::vector<ExportContentGroup>& groups, const ExportComponent& component,
    std::string_view openSourceMenu,
    const std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo>& timelines,
    const std::vector<ExportSourceEntry>& importedSources,
    const std::map<std::filesystem::path, DownTool::ShapefileInfo>& importedInfo)
{
    std::string html;
    const bool building = component.kind == ExportComponentKind::BuildingModel;
    const bool cesiumBuilding = component.kind == ExportComponentKind::CesiumBuildings;
    const bool cesium = cesiumBuilding || component.kind == ExportComponentKind::CesiumTerrain;
    if (component.kind != ExportComponentKind::DemGeoTiff &&
        component.kind != ExportComponentKind::CesiumTerrain)
    {
        html += "<div class=\"export-panel-section\"><span class=\"export-panel-label\">图源</span>";
        html += SourceBlockHtml(component, openSourceMenu, timelines,
                                importedSources, importedInfo, true);
        html += "</div>";
    }
    if (cesium)
    {
        if (!cesiumBuilding)
        {
            html += fmt::format(
                "<label class=\"export-field-row\"><span>采样间距（米）</span>"
                "<input id=\"export-act-set-sample__{}\" type=\"number\" "
                "min=\"1\" step=\"1\" value=\"{}\"></label>",
                component.id, component.elevationSampleMeters);
        }
        html += fmt::format(
            "<label class=\"export-field-row\"><span>几何误差（米）</span>"
            "<input id=\"export-act-set-cesium-error__{}\" type=\"number\" "
            "min=\"0.1\" step=\"0.1\" value=\"{}\"></label>"
            "<label class=\"export-field-row\"><span>高度偏移（米）</span>"
            "<input id=\"export-act-set-cesium-offset__{}\" type=\"number\" "
            "step=\"0.1\" value=\"{}\"></label>"
            "<label class=\"export-field-row\"><span>Geoid PGM 文件</span>"
            "<input id=\"export-act-set-cesium-geoid__{}\" type=\"text\" value=\"{}\"></label>",
            component.id, component.GetCesiumErrorMeters(),
            component.id, component.GetCesiumHeightOffsetMeters(),
            component.id, EscapeHtml(PathToUtf8(component.GetCesiumGeoidPath())));
        if (cesiumBuilding)
        {
            html += fmt::format(
                "<label class=\"export-field-row\"><span>最细层分块（米）</span>"
                "<input id=\"export-act-set-building-chunk-size__{}\" type=\"number\" "
                "min=\"1\" step=\"1\" value=\"{}\"></label>"
                "<label class=\"export-field-row\"><span>LOD 层数</span>"
                "<input id=\"export-act-set-cesium-levels__{}\" type=\"number\" "
                "min=\"2\" max=\"8\" step=\"1\" value=\"{}\"></label>",
                component.id, component.GetBuildingChunkSizeMeters(),
                component.id, component.GetCesiumLodLevels());
            html += FieldSelect("粗层图集尺寸", "export-act-set-texture-size__" + component.id,
                OptionTags({{"256", "256"}, {"512", "512"}, {"1024", "1024"},
                    {"2048", "2048"}, {"4096", "4096"}}, std::to_string(component.textureSize)));
            html += fmt::format(
                "<label class=\"export-switch-row\"><span>生成 Mipmap</span>"
                "<input id=\"export-act-toggle-cesium-mipmaps__{}\" "
                "class=\"export-switch\" type=\"checkbox\"{}></label>",
                component.id, component.GetCesiumTextureMipmaps() ? " checked" : "");
            html += fmt::format(
                "<label class=\"export-field-row\"><span>楼层高度（厘米）</span>"
                "<input id=\"export-act-set-layer-height__{}\" type=\"number\" "
                "min=\"1\" value=\"{}\"></label>"
                "<label class=\"export-field-row\"><span>随机种子</span>"
                "<input id=\"export-act-set-seed__{}\" type=\"number\" "
                "min=\"0\" value=\"{}\"></label>"
                "<label class=\"export-switch-row\"><span>采样地形高程</span>"
                "<input id=\"export-act-toggle-follow-elevation__{}\" "
                "class=\"export-switch\" type=\"checkbox\"{}></label>",
                component.id, component.layerHeightCentimeters,
                component.id, component.textureRandomSeed,
                component.id, component.followBaseElevation ? " checked" : "");
            html += DirectoryField("墙体贴图目录",
                "export-act-set-wall-path__" + component.id,
                "export-act-browse-wall-path__" + component.id, PathToUtf8(component.wallTexturePath));
            html += DirectoryField("屋顶贴图目录",
                "export-act-set-roof-path__" + component.id,
                "export-act-browse-roof-path__" + component.id, PathToUtf8(component.roofTexturePath));
        }
    }
    else if (component.kind == ExportComponentKind::MergedImage)
    {
        const bool jpgLosesAlpha = component.imageFormat == "jpg" &&
            std::ranges::any_of(component.sources, [](const ExportSourceEntry& source)
            {
                return source.kind == ExportSourceKind::Gis;
            });
        html += "<div class=\"export-panel-section\"><span class=\"export-panel-label\">输出</span>";
        html += FieldSelect("格式", "export-act-set-format__" + component.id,
            OptionTags({{"png", "PNG"}, {"jpg", "JPG"}}, component.imageFormat));
        html += FieldSelect("最大边", "export-act-set-edge__" + component.id,
            OptionTags({{"4096", "4096"}, {"8192", "8192"}, {"16384", "16384"}, {"32768", "32768"}},
                       std::to_string(component.maxEdge)));
        if (jpgLosesAlpha)
        {
            html += "<p class=\"export-hint\">JPG 会丢失 GIS 透明通道。</p>";
        }
        html += "</div>";
    }
    else if (component.kind == ExportComponentKind::DomGeoTiff)
    {
        html += "<div class=\"export-panel-section\"><span class=\"export-panel-label\">输出</span>";
        html += "<p class=\"export-hint\">按当前导出层级的原始瓦片分辨率输出，"
            "投影坐标系固定为 EPSG:3857。</p>";
        html += "</div>";
    }
    else if (component.kind == ExportComponentKind::DemGeoTiff)
    {
        html += "<div class=\"export-panel-section\"><span class=\"export-panel-label\">输出</span>";
        html += fmt::format(
            "<label class=\"export-field-row\"><span>采样间距（米）</span>"
            "<input id=\"export-act-set-sample__{}\" type=\"number\" "
            "min=\"1\" step=\"1\" value=\"{:.0f}\"></label>",
            component.id,
            component.elevationSampleMeters);
        html += "<p class=\"export-hint\">32 位浮点高程，投影坐标系固定为 EPSG:3857。</p>";
        html += "</div>";
    }
    else if (component.kind == ExportComponentKind::StandardXyz)
    {
        html += "<div class=\"export-panel-section\"><span class=\"export-panel-label\">输出</span>";
        html += FieldSelect("分辨率", "export-act-set-xyz__" + component.id,
            OptionTags({{"256", "256"}, {"512", "512"}, {"1024", "1024"}},
                       std::to_string(component.xyzTileSize)));
        html += "</div>";
    }
    else if (component.kind == ExportComponentKind::BasePlate)
    {
        const bool subdivisionEnabled = !component.exportElevation &&
            component.shape != DownTool::FbxPlanetShape::Plane;
        html += "<div class=\"export-panel-section\"><span class=\"export-panel-label\">贴图</span>";
        html += FieldSelect("贴图尺寸", "export-act-set-texture-size__" + component.id,
            OptionTags({{"1024", "1024"}, {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192"}, {"16384", "16384"}},
                       std::to_string(component.textureSize)));
        html += FieldSelect("贴图格式", "export-act-set-texture-format__" + component.id,
            OptionTags({{"png", "PNG"}, {"jpg", "JPG"}}, component.textureFormat));
        html += "</div>";
        html += ShapeSection(component);
        html +=
            fmt::format("<label class=\"export-switch-row\" title=\"按贴图分块分别生成 "
                        "FBX，保留统一的任务坐标原点\">"
                        "<span>每块单独导出 FBX</span>"
                        "<input id=\"export-act-toggle-fbx-per-tile__{}\" class=\"export-switch\" "
                        "type=\"checkbox\"{}></label>",
                        component.id, component.GetFbxPerTile() ? " checked" : "");
        html += fmt::format(
            "<label class=\"export-switch-row\"><span>导出高度</span>"
            "<input id=\"export-act-toggle-elevation__{}\" class=\"export-switch\" type=\"checkbox\"{}></label>",
            component.id, component.exportElevation ? " checked" : "");
        html += fmt::format(
            "<label class=\"export-field-row{}\"><span>采样率（米）</span>"
            "<input id=\"export-act-set-sample__{}\" type=\"number\" min=\"1\" step=\"1\" value=\"{:.0f}\"{}></label>",
            component.exportElevation ? "" : " is-disabled", component.id,
            component.elevationSampleMeters, component.exportElevation ? "" : " disabled");
        html += fmt::format(
            "<label class=\"export-field-row{}\"><span class=\"export-terrain-budget-label\">"
            "<span>每块三角面上限</span><span>（0不限）</span></span>"
            "<input id=\"export-act-set-terrain-budget__{}\" type=\"number\" min=\"0\" "
            "max=\"4294967295\" step=\"1\" value=\"{}\"{}></label>",
            component.exportElevation ? "" : " is-disabled", component.id,
            component.GetTerrainMaximumTriangles(), component.exportElevation ? "" : " disabled");
        html += fmt::format(
            "<label class=\"export-field-row{}\"><span>网格细分</span>"
            "<input id=\"export-act-set-subdivision__{}\" type=\"number\" min=\"1\" step=\"1\" value=\"{}\"{}></label>",
            subdivisionEnabled ? "" : " is-disabled", component.id, component.meshSubdivision,
            subdivisionEnabled ? "" : " disabled");
        html += ExportDistanceLodHtml(component);
        html += "<p class=\"export-hint\">高程零面为海拔 0 米。</p>";
    }
    else if (building)
    {
        const bool satelliteRoof = component.roofTextureMode ==
            DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
        html += ShapeRow(component);
        html += fmt::format(
            "<label class=\"export-field-row\"><span>分块边长 (米)</span>"
            "<input id=\"export-act-set-building-chunk-size__{}\" type=\"number\" "
            "min=\"1\" step=\"1\" value=\"{}\"></label>",
            component.id, component.GetBuildingChunkSizeMeters());
        html += "<div class=\"export-field-row export-building-mode-row\">"
            "<span>墙体模式</span><div class=\"export-segment\">";
        html += SegmentButton("export-act-set-wall-mode__" + component.id + "__scattered", "散图",
            component.wallTextureMode == DownTool::AmapBuildingFbxTextureMode::Scattered);
        html += SegmentButton("export-act-set-wall-mode__" + component.id + "__merged-atlas", "合并贴图",
            component.wallTextureMode == DownTool::AmapBuildingFbxTextureMode::MergedAtlas);
        html += "</div></div>";
        html += "<div class=\"export-field-row export-building-mode-row\">"
            "<span>屋顶模式</span><div class=\"export-segment\">";
        html += SegmentButton("export-act-set-roof-mode__" + component.id + "__scattered", "散图",
            component.roofTextureMode == DownTool::AmapBuildingFbxRoofTextureMode::Scattered);
        html += SegmentButton("export-act-set-roof-mode__" + component.id + "__merged-atlas", "合并贴图",
            component.roofTextureMode == DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas);
        html += SegmentButton("export-act-set-roof-mode__" + component.id + "__satellite", "卫星图",
            satelliteRoof);
        html += "</div></div>";
        if (satelliteRoof)
        {
            std::string options = "<option value=\"\">选择底板模型</option>";
            for (const ExportContentGroup& group : groups)
            {
                for (const ExportComponent& item : group.components)
                {
                    if (item.kind != ExportComponentKind::BasePlate)
                    {
                        continue;
                    }
                    const std::string label = (group.name.empty() ? "未命名" : group.name) + " / 底板模型";
                    options += fmt::format("<option value=\"{}\"{}>{}</option>", EscapeHtml(item.id),
                        item.id == component.basePlateComponentId ? " selected" : "", EscapeHtml(label));
                }
            }
            html += FieldSelect("屋顶来源", "export-act-set-base-plate__" + component.id, options);
            html += "<p class=\"export-hint\">buildings.fbx 写入被引用底板的 FBX 目录。</p>";
        }
        const bool atlas = component.wallTextureMode == DownTool::AmapBuildingFbxTextureMode::MergedAtlas ||
            component.roofTextureMode == DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
        if (atlas)
        {
            html += FieldSelect("合并格式", "export-act-set-atlas__" + component.id,
                OptionTags({{"png", "PNG"}, {"jpg", "JPG"}},
                    component.atlasImageFormat == DownTool::AmapBuildingFbxAtlasImageFormat::Jpg ? "jpg" : "png"));
        }
        html += fmt::format(
            "<label class=\"export-field-row\"><span>楼层高度</span>"
            "<input id=\"export-act-set-layer-height__{}\" type=\"number\" min=\"1\" step=\"1\" value=\"{:.0f}\"></label>",
            component.id, component.layerHeightCentimeters);
        html += fmt::format(
            "<label class=\"export-field-row\"><span>墙体材质上限</span>"
            "<input id=\"export-act-set-wall-materials__{}\" type=\"number\" min=\"1\" step=\"1\" value=\"{}\"></label>",
            component.id, component.maxWallMaterialsPerBucket);
        html += fmt::format(
            "<label class=\"export-field-row\"><span>随机种子</span>"
            "<input id=\"export-act-set-seed__{}\" type=\"number\" min=\"0\" step=\"1\" value=\"{}\"></label>",
            component.id, component.textureRandomSeed);
        html += fmt::format(
            "<label class=\"export-switch-row\"><span>跟随底板高程</span>"
            "<input id=\"export-act-toggle-follow-elevation__{}\" class=\"export-switch\" type=\"checkbox\"{}></label>",
            component.id, component.followBaseElevation ? " checked" : "");
        html += DirectoryField("墙体贴图目录",
            "export-act-set-wall-path__" + component.id,
            "export-act-browse-wall-path__" + component.id,
            PathToUtf8(component.wallTexturePath));
        if (!satelliteRoof)
        {
            html += DirectoryField("屋顶贴图目录",
                "export-act-set-roof-path__" + component.id,
                "export-act-browse-roof-path__" + component.id,
                PathToUtf8(component.roofTexturePath));
        }
    }
    else if (component.kind == ExportComponentKind::GeoJson ||
             component.kind == ExportComponentKind::Shapefile)
    {
        html += fmt::format(
            "<label class=\"export-check-row\"><input id=\"export-act-toggle-base-layers__{}\" type=\"checkbox\"{}>"
            "<span>基础图层</span></label>",
            component.id, component.includeBaseLayers ? " checked" : "");
        html += fmt::format(
            "<label class=\"export-check-row\"><input id=\"export-act-toggle-labels__{}\" type=\"checkbox\"{}>"
            "<span>标注图层</span></label>",
            component.id, component.includeLabels ? " checked" : "");
    }
    return html;
}

}

void DomDownToolPreview::PreviewController::RenderExportComposer(skui::Runtime& runtime)
{
    std::string groupsHtml;
    if (exportContentGroups_.empty())
    {
        groupsHtml = "<p class=\"export-empty\">当前内容区不可用，请重新打开导出配置。</p>";
    }
    for (const ExportContentGroup& group : exportContentGroups_)
    {
        std::string rows;
        for (const ExportComponent& component : group.components)
        {
            const bool selected = component.id == exportSelectedComponentId_;
            rows += fmt::format(
                "<div id=\"export-act-select-component__{}\" "
                "class=\"export-component-row{}\" data-action=\"export-composer\">"
                "<span class=\"export-drag-handle\" aria-hidden=\"true\">&#10303;</span>"
                "<strong>{}</strong><span id=\"export-summary__{}\">{}</span>"
                "<button id=\"export-act-open-params__{}\" "
                "class=\"export-quiet-button export-component-action{}\" "
                "type=\"button\" data-action=\"export-composer\">参数</button>"
                "<button id=\"export-act-remove-component__{}\" "
                "class=\"export-quiet-button export-component-action\" "
                "type=\"button\" data-action=\"export-composer\">移除</button></div>",
                component.id, selected ? " is-selected" : "",
                KindName(component.kind), component.id,
                EscapeHtml(ComponentSummary(component)), component.id,
                selected ? " is-active" : "", component.id);
        }
        std::string menu;
        if (exportComponentMenuGroupId_ == group.id)
        {
            menu = "<div class=\"export-menu\">";
            constexpr ExportComponentKind kKinds[] = {
                ExportComponentKind::MergedImage,
                ExportComponentKind::DomGeoTiff,
                ExportComponentKind::DemGeoTiff,
                ExportComponentKind::StandardXyz,
                ExportComponentKind::RawTiles,
                ExportComponentKind::BasePlate,
                ExportComponentKind::BuildingModel,
                ExportComponentKind::CesiumTerrain,
                ExportComponentKind::CesiumBuildings,
                ExportComponentKind::GeoJson,
                ExportComponentKind::Shapefile};
            for (const ExportComponentKind kind : kKinds)
            {
                menu += fmt::format(
                    "<button id=\"export-act-add-component__{}__{}\" type=\"button\" "
                    "data-action=\"export-composer\">{}</button>",
                    group.id, KindValue(kind), KindName(kind));
            }
            menu += "</div>";
        }
        const bool onlyGroup = exportContentGroups_.size() == 1;
        const std::string removeGroupButton = onlyGroup ?
            std::string() :
            fmt::format(
                "<button id=\"export-act-remove-group__{}\" "
                "class=\"export-quiet-button export-group-remove-button\" "
                "type=\"button\" data-action=\"export-composer\">移除内容组</button>",
                group.id);
        groupsHtml += fmt::format(
            "<section id=\"export-group__{}\" class=\"export-group-card{}\">"
            "<header class=\"export-group-head\">"
            "<label class=\"export-group-check\"><span class=\"export-check-control\">"
            "<input id=\"export-act-toggle-group__{}\" type=\"checkbox\"{}>"
            "<span class=\"export-check-indicator{}\">&#10003;</span></span>"
            "<input id=\"export-act-rename-group__{}\" type=\"text\" value=\"{}\" "
            "aria-label=\"内容组名称\"></label>{}"
            "</header>{}<div class=\"export-add-row\">"
            "<button id=\"export-act-open-components__{}\" "
            "class=\"export-quiet-button export-add-component-button\" "
            "type=\"button\" data-action=\"export-composer\">添加组件</button>"
            "{}</div></section>",
            group.id, onlyGroup ? " is-only-group" : "", group.id,
            group.enabled ? " checked" : "", group.enabled ? " is-checked" : "",
            group.id, EscapeHtml(group.name), removeGroupButton, rows, group.id, menu);
    }
    const std::string listHtml = fmt::format(
        "<div id=\"export-group-list\" class=\"export-group-list\">{}</div>", groupsHtml);

    const ExportComponent* selected = nullptr;
    for (const ExportContentGroup& group : exportContentGroups_)
    {
        for (const ExportComponent& component : group.components)
        {
            if (component.id == exportSelectedComponentId_)
            {
                selected = &component;
            }
        }
    }
    const std::string panelScrollContentHtml = selected == nullptr
        ? std::string()
        : fmt::format(
            "<div id=\"export-panel-scroll-content\" "
            "class=\"export-panel-scroll-content\">{}</div>",
            PanelBodyHtml(exportContentGroups_, *selected, exportSourceMenuComponentId_,
                          tianDiTuMultidateTimelines_, importedShapefileSources_,
                          importedShapefileInfo_));
    const std::string panelHtml = selected == nullptr
        ? "<aside id=\"export-param-panel\" class=\"export-param-panel\" style=\"display:none\"></aside>"
        : fmt::format(
            "<aside id=\"export-param-panel\" class=\"export-param-panel\">"
            "<header class=\"export-panel-head\"><h2>{}</h2>"
            "<button id=\"export-act-close-panel\" class=\"export-icon-button\" "
            "type=\"button\" data-action=\"export-composer\" "
            "aria-label=\"关闭参数\">×</button>"
            "</header><div id=\"export-panel-body\" class=\"export-panel-body\">"
            "{}</div>"
            "<button id=\"export-act-done-panel\" "
            "class=\"primary-command export-panel-done\" type=\"button\" "
            "data-action=\"export-composer\">完成</button></aside>",
            KindName(selected->kind), panelScrollContentHtml);
    if (listHtml != lastExportGroupListHtml_)
    {
        if (runtime.replaceHtmlById("export-group-list", listHtml))
        {
            lastExportGroupListHtml_ = listHtml;
        }
        else
        {
            lastExportGroupListHtml_.clear();
        }
    }
    if (panelHtml != lastExportParamPanelHtml_)
    {
        const bool sameSelectedPanel = selected != nullptr &&
            selected->id == lastExportParamPanelComponentId_;
        const bool panelUpdated = sameSelectedPanel
            ? runtime.replaceHtmlById(
                "export-panel-scroll-content", panelScrollContentHtml)
            : runtime.replaceHtmlById("export-param-panel", panelHtml);
        if (panelUpdated)
        {
            lastExportParamPanelHtml_ = panelHtml;
            lastExportParamPanelComponentId_ = selected == nullptr ? "" : selected->id;
            if (selected != nullptr && !sameSelectedPanel &&
                runtime.width() <= 700.F * runtime.effectiveScale())
            {
                runtime.scrollIntoViewById("export-param-panel");
            }
        }
        else
        {
            lastExportParamPanelHtml_.clear();
            lastExportParamPanelComponentId_.clear();
        }
    }
    runtime.setVisibleById("export-footer", selected == nullptr);
}

bool DomDownToolPreview::PreviewController::HandleExportComposerEvent(
    skui::Runtime& runtime, std::string_view id, std::string_view value, bool checked,
    bool fromClick, bool fromChange)
{
    std::string verb;
    std::string first;
    std::string second;
    if (!ParseAction(id, verb, first, second))
    {
        return false;
    }
    const auto findGroup = [this](std::string_view groupId) -> ExportContentGroup*
    {
        for (ExportContentGroup& group : exportContentGroups_)
        {
            if (group.id == groupId)
            {
                return &group;
            }
        }
        return nullptr;
    };
    const auto findComponent = [this](std::string_view componentId) -> ExportComponent*
    {
        for (ExportContentGroup& group : exportContentGroups_)
        {
            for (ExportComponent& component : group.components)
            {
                if (component.id == componentId)
                {
                    return &component;
                }
            }
        }
        return nullptr;
    };
    const auto nextId = [this](char prefix)
    {
        return std::string(1, prefix) + std::to_string(exportComposerSerial_++);
    };
    const bool checkboxVerb = verb == "toggle-group" || verb == "toggle-elevation" ||
                              verb == "toggle-fbx-per-tile" || verb == "toggle-follow-elevation" ||
                              verb == "toggle-base-layers" || verb == "toggle-labels";
    if (checkboxVerb && !fromChange)
    {
        return true;
    }
    const bool textVerb = verb == "rename-group" || verb == "set-sample" || verb == "set-subdivision" ||
        verb == "set-wall-path" || verb == "set-roof-path" || verb == "set-layer-height" ||
        verb == "set-building-chunk-size" || verb == "set-wall-materials" ||
        verb == "set-seed" || verb == "set-color";
    if (textVerb && fromClick)
    {
        return true;
    }

    bool refresh = true;
    if (verb.starts_with("lod-"))
    {
        const bool command = verb.ends_with("-add") || verb.ends_with("-remove");
        if ((command && !fromClick) || (!command && fromClick))
        {
            return true;
        }
        if (ExportComponent* component = findComponent(first))
        {
            UpdateExportDistanceLod(*component, verb, second, value);
        }
        refresh = command;
    }
    else if (verb == "remove-group" && fromClick)
    {
        std::erase_if(exportContentGroups_, [&](const ExportContentGroup& group)
        {
            if (group.id != first)
            {
                return false;
            }
            for (const ExportComponent& component : group.components)
            {
                if (component.id == exportSelectedComponentId_)
                {
                    exportSelectedComponentId_.clear();
                }
            }
            return true;
        });
        exportComponentMenuGroupId_.clear();
    }
    else if (verb == "toggle-group")
    {
        if (ExportContentGroup* group = findGroup(first))
        {
            group->enabled = checked;
        }
    }
    else if (verb == "rename-group")
    {
        if (ExportContentGroup* group = findGroup(first))
        {
            group->name = std::string(value);
        }
        refresh = false;
    }
    else if (verb == "open-components" && fromClick)
    {
        exportComponentMenuGroupId_ = exportComponentMenuGroupId_ == first ? std::string() : first;
        exportSourceMenuComponentId_.clear();
    }
    else if (verb == "add-component" && fromClick)
    {
        ExportContentGroup* group = findGroup(first);
        const std::optional<ExportComponentKind> kind = KindFrom(second);
        if (group != nullptr && kind.has_value())
        {
            ExportComponent component;
            component.id = nextId('c');
            component.kind = *kind;
            if (*kind == ExportComponentKind::BasePlate)
            {
                component.SetDistanceLod(DownTool::DefaultFbxDistanceLod());
            }
            if (*kind == ExportComponentKind::CesiumBuildings)
            {
                component.textureSize = 1024;
                component.followBaseElevation = true;
            }
            if (*kind == ExportComponentKind::BuildingModel ||
                *kind == ExportComponentKind::CesiumBuildings)
            {
                if (const GisCatalogItem* item = FindGis(DownTool::AmapVectorRenderCategory::Building))
                {
                    ExportSourceEntry source;
                    source.id = item->id;
                    source.name = item->name;
                    source.kind = ExportSourceKind::Gis;
                    source.gisCategory = item->category;
                    source.color = renderer_.GetGisOverlayColor(item->preview);
                    component.sources.push_back(std::move(source));
                }
            }
            exportSelectedComponentId_ = component.id;
            group->components.push_back(std::move(component));
        }
        exportComponentMenuGroupId_.clear();
        exportSourceMenuComponentId_.clear();
    }
    else if ((verb == "select-component" || verb == "open-params") && fromClick)
    {
        exportSelectedComponentId_ = first;
        exportComponentMenuGroupId_.clear();
        exportSourceMenuComponentId_.clear();
    }
    else if (verb == "remove-component" && fromClick)
    {
        for (ExportContentGroup& group : exportContentGroups_)
        {
            std::erase_if(group.components, [&](const ExportComponent& component)
            {
                return component.id == first;
            });
        }
        if (exportSelectedComponentId_ == first)
        {
            exportSelectedComponentId_.clear();
        }
        exportSourceMenuComponentId_.clear();
    }
    else if ((verb == "close-panel" || verb == "done-panel") && fromClick)
    {
        exportSelectedComponentId_.clear();
        exportSourceMenuComponentId_.clear();
    }
    else if (verb == "open-sources" && fromClick)
    {
        exportSourceMenuComponentId_ = exportSourceMenuComponentId_ == first ? std::string() : first;
        exportComponentMenuGroupId_.clear();
    }
    else if (verb == "add-source" && fromClick)
    {
        ExportComponent* component = findComponent(first);
        if (component != nullptr)
        {
            if (const ImageryCatalogItem* item = FindImagery(second))
            {
                if (std::string_view(item->id) == "tianditu.multidate" ||
                    !ComponentHasSource(*component, item->id, {}))
                {
                    ExportSourceEntry source;
                    source.id = item->id;
                    source.name = item->name;
                    source.kind = ExportSourceKind::Imagery;
                    component->sources.push_back(std::move(source));
                    if (second == "tianditu.multidate")
                    {
                        exportMapSourceIds_.insert(second);
                        exportSourceMenuComponentId_.clear();
                        EnsureExportTianDiTuMultidateYears(runtime);
                        requestRedraw();
                        return true;
                    }
                }
            }
            else if (const GisCatalogItem* item = FindGis(second))
            {
                if (AllowsGis(component->kind) && !ComponentHasSource(*component, item->id, {}))
                {
                    ExportSourceEntry source;
                    source.id = item->id;
                    source.name = item->name;
                    source.kind = ExportSourceKind::Gis;
                    source.gisCategory = item->category;
                    const auto saved = exportGisColors_.find(item->category);
                    source.color = saved == exportGisColors_.end() ?
                        renderer_.GetGisOverlayColor(item->preview) : saved->second;
                    component->sources.push_back(std::move(source));
                }
            }
            else
            {
                const auto imported = std::ranges::find_if(
                    importedShapefileSources_, [&second](const auto& source)
                    { return source.id == second; });
                if (imported != importedShapefileSources_.end() &&
                    AllowsGis(component->kind) && !ComponentHasSource(*component, second, {}))
                {
                    component->sources.push_back(*imported);
                }
            }
        }
        exportSourceMenuComponentId_.clear();
    }
    else if (verb == "remove-source" && fromClick)
    {
        ExportComponent* component = findComponent(first);
        const std::optional<uint32_t> index = ParseUnsigned(second);
        if (component != nullptr && index.has_value() && *index < component->sources.size())
        {
            component->sources.erase(component->sources.begin() + static_cast<std::ptrdiff_t>(*index));
        }
    }
    else if ((verb == "shp-category" || verb == "shp-height") && fromChange)
    {
        ExportComponent* component = findComponent(first);
        const auto index = ParseUnsigned(second);
        if (component != nullptr && index && *index < component->sources.size())
        {
            auto& source = component->sources[*index];
            if (!source.GetShapefile().GetPath().empty())
            {
                if (verb == "shp-category")
                {
                    if (const auto* category = FindGis(value))
                    {
                        source.gisCategory = category->category;
                    }
                }
                else
                {
                    auto options = source.GetShapefile();
                    options.SetHeightField(std::string(value));
                    source.SetShapefile(options);
                }
            }
        }
    }
    else if (verb == "set-format" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->imageFormat = value == "jpg" ? "jpg" : "png";
        }
    }
    else if (verb == "set-edge" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<uint32_t> edge = ParseUnsigned(value))
            {
                component->maxEdge = *edge;
            }
        }
    }
    else if (verb == "set-xyz" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<uint32_t> tileSize = ParseUnsigned(value))
            {
                component->xyzTileSize = *tileSize;
            }
        }
    }
    else if (verb == "set-texture-size" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<uint32_t> textureSize = ParseUnsigned(value))
            {
                component->textureSize = *textureSize;
            }
        }
    }
    else if (verb == "set-texture-format" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->textureFormat = value == "jpg" ? "jpg" : "png";
        }
    }
    else if (verb == "set-shape" && fromClick)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->shape = second == "sphere" ? DownTool::FbxPlanetShape::Sphere :
                second == "both" ? DownTool::FbxPlanetShape::Both : DownTool::FbxPlanetShape::Plane;
        }
    }
    else if (verb == "toggle-elevation")
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->exportElevation = checked;
        }
    }
    else if (verb == "toggle-fbx-per-tile")
    {
        if (ExportComponent* component = findComponent(first);
            component && component->kind == ExportComponentKind::BasePlate)
        {
            component->SetFbxPerTile(checked);
        }
    }
    else if (verb == "set-sample")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<double> sample = ParsePositive(value))
            {
                component->elevationSampleMeters = *sample;
                runtime.setTextById("export-summary__" + component->id, ComponentSummary(*component));
            }
        }
        refresh = false;
    }
    else if (verb == "toggle-cesium-mipmaps")
    {
        if (ExportComponent* component = findComponent(first);
            component && component->kind == ExportComponentKind::CesiumBuildings)
        {
            component->SetCesiumTextureMipmaps(checked);
        }
    }
    else if (verb == "set-cesium-error" || verb == "set-cesium-offset" ||
             verb == "set-cesium-levels" || verb == "set-cesium-geoid")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (verb == "set-cesium-geoid")
            {
                component->SetCesiumGeoidPath(std::filesystem::u8path(std::string(value)));
            }
            else if (verb == "set-cesium-levels")
            {
                if (const auto levels = ParseUnsigned(value))
                {
                    component->SetCesiumLodLevels(*levels);
                }
            }
            else
            {
                try
                {
                    size_t consumed = 0;
                    const double parsed = std::stod(std::string(value), &consumed);
                    if (consumed == value.size() && std::isfinite(parsed))
                    {
                        if (verb == "set-cesium-error" && parsed > 0.)
                        {
                            component->SetCesiumErrorMeters(parsed);
                        }
                        else if (verb == "set-cesium-offset")
                        {
                            component->SetCesiumHeightOffsetMeters(parsed);
                        }
                    }
                }
                catch (const std::exception&)
                {
                    // 不完整输入保留上一次有效值。
                }
            }
        }
        refresh = false;
    }
    else if (verb == "set-terrain-budget")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const auto budget = ParseUnsigned(value))
            {
                component->SetTerrainMaximumTriangles(*budget);
            }
        }
        refresh = false;
    }
    else if (verb == "set-subdivision")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<uint32_t> subdivision = ParseUnsigned(value))
            {
                component->meshSubdivision = *subdivision;
            }
        }
        refresh = false;
    }
    else if (verb == "set-year" && fromChange)
    {
        ExportComponent* component = findComponent(first);
        const std::optional<uint32_t> index = ParseUnsigned(second);
        if (component != nullptr && index.has_value() && *index < component->sources.size())
        {
            ExportSourceEntry& source = component->sources[*index];
            if (value.empty())
            {
                source.multidateKey.clear();
                source.multidateYear.clear();
                source.name = "天地图多时相";
            }
            else
            {
                const auto timeline = std::ranges::find_if(
                    tianDiTuMultidateTimelines_,
                    [&value](const auto& item) { return item.key == value; });
                if (timeline != tianDiTuMultidateTimelines_.end())
                {
                    const std::string year = timeline->year.empty() ? timeline->time : timeline->year;
                    source.multidateKey = timeline->key;
                    source.multidateYear = year;
                    source.multidateGbCode = tianDiTuMultidateRegionGbCode_;
                    source.multidateRegionName = tianDiTuMultidateRegionName_;
                    source.name = "天地图多时相 " + year;
                }
            }
        }
    }
    else if (verb == "set-color")
    {
        ExportComponent* component = findComponent(first);
        const std::optional<uint32_t> index = ParseUnsigned(second);
        const std::optional<DownTool::AmapVectorColor> color = ParseColorHex(value);
        if (component != nullptr && index.has_value() && color.has_value() &&
            *index < component->sources.size())
        {
            component->sources[*index].color = *color;
            exportGisColors_[component->sources[*index].gisCategory] = *color;
        }
        refresh = false;
    }
    else if ((verb == "browse-wall-path" || verb == "browse-roof-path") &&
             fromClick)
    {
        ExportComponent* component = findComponent(first);
        const std::optional<std::filesystem::path> selectedPath =
            SelectExportDirectory();
        if (component != nullptr && selectedPath.has_value())
        {
            if (verb == "browse-wall-path")
            {
                component->wallTexturePath = *selectedPath;
            }
            else
            {
                component->roofTexturePath = *selectedPath;
            }
        }
        else
        {
            refresh = false;
        }
    }
    else if (verb == "set-wall-path")
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->wallTexturePath = std::filesystem::u8path(std::string(value));
        }
        refresh = false;
    }
    else if (verb == "set-roof-path")
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->roofTexturePath = std::filesystem::u8path(std::string(value));
        }
        refresh = false;
    }
    else if (verb == "set-wall-mode" && fromClick)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->wallTextureMode = second == "merged-atlas" ?
                DownTool::AmapBuildingFbxTextureMode::MergedAtlas :
                DownTool::AmapBuildingFbxTextureMode::Scattered;
        }
    }
    else if (verb == "set-roof-mode" && fromClick)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->roofTextureMode = second == "merged-atlas" ?
                DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas :
                second == "satellite" ? DownTool::AmapBuildingFbxRoofTextureMode::Satellite :
                DownTool::AmapBuildingFbxRoofTextureMode::Scattered;
        }
    }
    else if (verb == "set-base-plate" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->basePlateComponentId = std::string(value);
        }
    }
    else if (verb == "set-atlas" && fromChange)
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->atlasImageFormat = value == "jpg" ?
                DownTool::AmapBuildingFbxAtlasImageFormat::Jpg :
                DownTool::AmapBuildingFbxAtlasImageFormat::Png;
        }
    }
    else if (verb == "set-layer-height")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<double> height = ParsePositive(value))
            {
                component->layerHeightCentimeters = *height;
            }
        }
        refresh = false;
    }
    else if (verb == "set-building-chunk-size")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<double> size = ParsePositive(value))
            {
                component->SetBuildingChunkSizeMeters(*size);
            }
        }
        refresh = false;
    }
    else if (verb == "set-wall-materials")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<uint32_t> limit = ParseUnsigned(value); limit && *limit > 0)
            {
                component->maxWallMaterialsPerBucket = *limit;
            }
        }
        refresh = false;
    }
    else if (verb == "set-seed")
    {
        if (ExportComponent* component = findComponent(first))
        {
            if (const std::optional<uint32_t> seed = ParseUnsigned(value))
            {
                component->textureRandomSeed = *seed;
            }
        }
        refresh = false;
    }
    else if (verb == "toggle-follow-elevation")
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->followBaseElevation = checked;
        }
    }
    else if (verb == "toggle-base-layers")
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->includeBaseLayers = checked;
        }
    }
    else if (verb == "toggle-labels")
    {
        if (ExportComponent* component = findComponent(first))
        {
            component->includeLabels = checked;
        }
    }
    else
    {
        refresh = false;
    }
    if (refresh)
    {
        exportValidationMessage_.clear();
        UpdateExportUi(runtime);
        requestRedraw();
    }
    return true;
}

bool DomDownToolPreview::PreviewController::ImportShapefile(const std::filesystem::path& path,
                                                            skui::Runtime& runtime,
                                                            std::string& error)
{
    DownTool::ShapefileInfo info;
    if (!DownTool::Wgs84ShapefileReader::Inspect(path, info, error))
    {
        return false;
    }
    if (page_ != Page::Preview)
    {
        OpenPreviewPage(runtime);
    }
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    auto found = std::ranges::find_if(importedShapefileSources_, [&absolute](const auto& source) {
        return source.GetShapefile().GetPath() == absolute;
    });
    if (found == importedShapefileSources_.end())
    {
        ExportSourceEntry source;
        source.id = "shp." + std::to_string(importedShapefileSources_.size() + 1);
        while (std::ranges::any_of(importedShapefileSources_,
                                   [&source](const auto& item) { return item.id == source.id; }))
        {
            source.id += "x";
        }
        source.name = PathToUtf8(absolute.stem()) + " (SHP)";
        source.kind = ExportSourceKind::Gis;
        source.gisCategory = info.GetGeometryType() == DownTool::AmapVectorGeometryType::Polygon
                                 ? DownTool::AmapVectorRenderCategory::Building
                             : info.GetGeometryType() == DownTool::AmapVectorGeometryType::Point
                                 ? DownTool::AmapVectorRenderCategory::Point
                                 : DownTool::AmapVectorRenderCategory::Road;
        source.color = DownTool::GetDefaultAmapVectorRenderColor(source.gisCategory);
        DownTool::ShapefileImportOptions options;
        options.SetPath(absolute);
        source.SetShapefile(options);
        importedShapefileSources_.push_back(std::move(source));
        found = std::prev(importedShapefileSources_.end());
    }
    importedShapefileInfo_[absolute] = info;
    hiddenShapefileSourceIds_.erase(found->id);
    FocusShapefile(*found);
    if (previewRangeMode_ == ExportRangeMode::CenterRadius &&
        renderer_.GetExportRadiusMeters() <= 0. && info.GetFeatureCount() > 0)
    {
        const auto center = DownTool::GeographicBoundsCenter(info.GetBounds());
        previewRadiusValue_ = fmt::format(
            "{:.3f}",
            std::max(1., DownTool::GeographicBoundsRadiusMeters(info.GetBounds(), center)));
        renderer_.SetExportRadiusMeters(std::stod(previewRadiusValue_));
    }
    shapefilePreviewError_.clear();
    SyncShapefilePreview();
    updateUi(runtime);
    requestRedraw();
    return true;
}

void DomDownToolPreview::PreviewController::SyncShapefilePreview()
{
    std::vector<ExportSourceEntry> visible;
    for (const auto& source : importedShapefileSources_)
    {
        if (!hiddenShapefileSourceIds_.contains(source.id))
        {
            visible.push_back(source);
        }
    }
    DownTool::AmapVectorDownloadOptions options;
    ConfigureGisVectorSources(options, visible);
    renderer_.SetLocalShapefiles(options.GetShapefiles());
}

void DomDownToolPreview::PreviewController::FocusShapefile(const ExportSourceEntry& source)
{
    const auto info = importedShapefileInfo_.find(source.GetShapefile().GetPath());
    if (info == importedShapefileInfo_.end() || info->second.GetFeatureCount() == 0)
    {
        return;
    }
    const auto& bounds = info->second.GetBounds();
    const auto center = DownTool::GeographicBoundsCenter(bounds);
    const double span = std::max(bounds.east - bounds.west, (bounds.north - bounds.south) * 1.5);
    renderer_.setView(center, std::clamp(std::log2(180. / std::max(span, 0.0001)), 1., 20.));
}

void DomDownToolPreview::PreviewController::UpdateShapefilePreviewUi(skui::Runtime& runtime)
{
    std::string message = shapefilePreviewError_;
    if (message.empty())
    {
        message = renderer_.GetLocalShapefileError();
    }
    if (message.empty() && renderer_.IsLocalShapefileLoading())
    {
        message = "正在读取 SHP…";
    }
    runtime.setVisibleById("shp-preview-panel",
                           !importedShapefileSources_.empty() || !message.empty());
    runtime.setTextById("shp-preview-count",
                        fmt::format("本地图层 ({})", importedShapefileSources_.size()));
    runtime.setVisibleById("shp-preview-export", !importedShapefileSources_.empty());
    runtime.setVisibleById("shp-preview-message", !message.empty());
    runtime.setTextById("shp-preview-message", message);
    std::string html = "<div id=\"shp-preview-list\" class=\"shp-preview-list\">";
    for (const auto& source : importedShapefileSources_)
    {
        const bool visible = !hiddenShapefileSourceIds_.contains(source.id);
        const std::string id = EscapeHtml(source.id);
        html += "<div class=\"shp-preview-row\"><input type=\"checkbox\" id=\"shp-visible-" + id +
                "\" title=\"显示图层\" aria-label=\"显示图层\"" +
                (visible ? std::string(" checked") : std::string{}) +
                "><span class=\"shp-preview-name\" title=\"" +
                EscapeHtml(PathToUtf8(source.GetShapefile().GetPath())) + "\">" +
                EscapeHtml(source.name) + "</span><button type=\"button\" id=\"shp-focus-" + id +
                "\" data-action=\"shapefile-preview\" title=\"定位图层\" "
                "aria-label=\"定位图层\">&#x2299;</button>"
                "<button type=\"button\" id=\"shp-remove-" +
                id + "\" data-action=\"shapefile-preview\" title=\"移除图层\" "
                "aria-label=\"移除图层\">&times;</button></div>";
    }
    html += "</div>";
    if (html != lastShapefilePreviewHtml_ && runtime.replaceHtmlById("shp-preview-list", html))
    {
        lastShapefilePreviewHtml_ = std::move(html);
    }
}

bool DomDownToolPreview::PreviewController::HandleShapefilePreviewEvent(
    skui::Runtime& runtime, const skui::ElementEvent& event)
{
    const bool visibility = event.id.starts_with("shp-visible-");
    const bool focus = event.id.starts_with("shp-focus-");
    const bool remove = event.id.starts_with("shp-remove-");
    if ((!visibility && !focus && !remove) ||
        (visibility ? event.type != skui::ElementEventType::Change
                    : event.type != skui::ElementEventType::Click))
    {
        return false;
    }
    const std::string id = event.id.substr(visibility ? 12 : focus ? 10 : 11);
    const auto source = std::ranges::find(importedShapefileSources_, id, &ExportSourceEntry::id);
    if (source == importedShapefileSources_.end())
    {
        return true;
    }
    if (focus)
    {
        FocusShapefile(*source);
    }
    else
    {
        if (remove)
        {
            importedShapefileInfo_.erase(source->GetShapefile().GetPath());
            importedShapefileSources_.erase(source);
            hiddenShapefileSourceIds_.erase(id);
            for (auto& group : exportContentGroups_)
            {
                for (auto& component : group.components)
                {
                    std::erase_if(component.sources,
                                  [&id](const auto& item) { return item.id == id; });
                }
            }
        }
        else if (event.checked)
        {
            hiddenShapefileSourceIds_.erase(id);
        }
        else
        {
            hiddenShapefileSourceIds_.insert(id);
        }
        shapefilePreviewError_.clear();
        SyncShapefilePreview();
    }
    updateUi(runtime);
    requestRedraw();
    return true;
}
