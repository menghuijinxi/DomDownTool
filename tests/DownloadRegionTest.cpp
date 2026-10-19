#include "PostProcessTestSupport.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#include "nlohmann/json.hpp"

#include "AmapVectorPngDownloader.h"
#include "DownloadRegion.h"
#include "GCJ02GoogleDownBase.h"
#include "GisSpatialFilter.h"
#include "preview/DownloadRangeEditor.h"

namespace
{
using namespace DownTool;
using postprocess_test::Require;

Wgs84Ring Rectangle(double west, double south, double east, double north)
{
    return {{west, south}, {east, south}, {east, north}, {west, north}};
}

AmapVectorFeature Polygon(const Wgs84Ring& ring, const std::string& layer)
{
    AmapVectorFeature feature;
    feature.geometryType = AmapVectorGeometryType::Polygon;
    feature.layerName = layer;
    for (const auto& point : ring)
    {
        const auto gcj = GCJ02GoogleDownBase::WGS84ToGCJ02(point);
        if (feature.geometryParts.empty())
        {
            feature.geometryParts.emplace_back();
        }
        feature.geometryParts.front().push_back({gcj.lon, gcj.lat});
    }
    feature.geometryParts.front().push_back(feature.geometryParts.front().front());
    return feature;
}

bool TestRegionPersistenceAndEditing()
{
    DomDownToolPreview::DownloadRangeEditor editor;
    std::string error;
    const auto include = Rectangle(0., 0., 10., 10.);
    const auto exclude = Rectangle(4., 4., 6., 6.);
    if (!Require(editor.AddRegion(include, DownloadRegionRole::Include, error) &&
                     editor.AddRegion(exclude, DownloadRegionRole::ExcludeGis, error) &&
                     editor.SetSelectedName("保留编辑状态的排除区", error),
                 "区域添加失败：" + error))
    {
        return false;
    }
    const auto saved = DownloadRegionsToJson(editor.GetRegions());
    DownloadRegions restored;
    if (!Require(ReadDownloadRegions(saved, restored, error) &&
                     saved == DownloadRegionsToJson(restored),
                 "区域元数据和坐标恢复不完整"))
    {
        return false;
    }
    DomDownToolPreview::DownloadRangeEditor reopened;
    reopened.SetRegions(restored);
    reopened.SetSelectedId(restored.back().GetId());
    const auto edited = Rectangle(3., 3., 7., 7.);
    if (!Require(
            reopened.SetSelectedVertices(edited, error) &&
                reopened.SetSelectedEnabled(false, error) &&
                GetRegionPolygons(reopened.GetRegions(), DownloadRegionRole::ExcludeGis).empty(),
            "恢复后的区域无法继续编辑或停用"))
    {
        return false;
    }
    reopened.Undo();
    if (!Require(GetRegionPolygons(reopened.GetRegions(), DownloadRegionRole::ExcludeGis).size() ==
                     1,
                 "撤销停用区域失败"))
    {
        return false;
    }
    reopened.Undo();
    const auto original = saved;
    if (!Require(DownloadRegionsToJson(reopened.GetRegions()) == original &&
                     DownloadRegionsToJson(restored) == original,
                 "撤销没有恢复原坐标，或编辑覆盖了旧配置"))
    {
        return false;
    }
    reopened.Redo();
    if (!Require(std::abs(reopened.GetSelectedRegion()->GetVertices().front().lon - 3.) < 1.e-9,
                 "重做顶点编辑失败"))
    {
        return false;
    }
    const auto beforeInvalidEdit = DownloadRegionsToJson(reopened.GetRegions());
    if (!Require(!reopened.SetSelectedVertices({{0., 0.}, {2., 2.}, {0., 2.}, {2., 0.}}, error) &&
                     DownloadRegionsToJson(reopened.GetRegions()) == beforeInvalidEdit,
                 "自交区域被接受或失败编辑覆盖了有效区域"))
    {
        return false;
    }
    auto invalid = saved;
    invalid[1]["id"] = invalid[0]["id"];
    return Require(!ReadDownloadRegions(invalid, restored, error), "重复区域 ID 没有被拒绝");
}

bool TestRoadClippingAndBuildingExclusion()
{
    GisSpatialRange range;
    range.SetIncludedPolygons({Rectangle(0., 0., 10., 10.)});
    range.SetExcludedPolygons({Rectangle(4., 4., 6., 6.)});
    AmapVectorDecodedData data;
    AmapVectorFeature road;
    road.geometryType = AmapVectorGeometryType::LineString;
    road.layerName = "road";
    road.uid = 123;
    road.geometryParts = {{{-1., 5.}, {11., 5.}}};
    data.features.push_back(road);
    auto degenerateRoad = road;
    degenerateRoad.uid = 999;
    degenerateRoad.geometryParts = {{{2., 2.}, {2., 2.}}};
    data.features.push_back(degenerateRoad);
    auto excludedBuilding = Polygon(Rectangle(3.5, 4.5, 4.5, 5.5), "building");
    excludedBuilding.uid = 456;
    excludedBuilding.hasHeight = true;
    excludedBuilding.height = 20;
    data.features.push_back(excludedBuilding);
    auto touchingBuilding = Polygon(Rectangle(3., 4., 4., 5.), "building");
    touchingBuilding.uid = 457;
    data.features.push_back(touchingBuilding);
    auto includedBuilding = Polygon(Rectangle(1., 1., 2., 2.), "building");
    includedBuilding.uid = 789;
    includedBuilding.hasHeight = true;
    includedBuilding.height = 20;
    data.features.push_back(includedBuilding);
    AmapVectorFeature point;
    point.geometryType = AmapVectorGeometryType::Point;
    point.geometryParts = {{{5., 5.}}};
    data.features.push_back(point);
    point.geometryParts = {{{4., 5.}}};
    data.features.push_back(point);
    std::string error;
    if (!Require(ApplyGisSpatialFilter(data, range, error), error))
    {
        return false;
    }
    size_t roadCount = 0;
    size_t buildingCount = 0;
    for (const auto& feature : data.features)
    {
        if (feature.uid == road.uid)
        {
            ++roadCount;
            for (const auto& coordinate : feature.geometryParts.front())
            {
                if (!Require(coordinate.longitude >= 0. && coordinate.longitude <= 10. &&
                                 !(coordinate.longitude > 4. && coordinate.longitude < 6.),
                             "道路没有沿下载区和排除区边界裁切"))
                {
                    return false;
                }
            }
        }
        else if (feature.uid == includedBuilding.uid)
        {
            ++buildingCount;
        }
        else
        {
            return Require(false, "命中排除区的建筑或点仍被保留");
        }
    }
    return Require(roadCount == 2 && buildingCount == 1,
                   "穿过排除区的道路未分成两段，或整栋建筑筛选不正确");
}

bool TestPolygonHolesAndOverlappingIncludes()
{
    GisSpatialRange range;
    range.SetIncludedPolygons({Rectangle(0., 0., 8., 10.), Rectangle(2., 0., 10., 10.)});
    range.SetExcludedPolygons({Rectangle(4., 4., 6., 6.)});
    AmapVectorDecodedData data;
    data.features = {Polygon(Rectangle(-1., -1., 11., 11.), "region")};
    std::string error;
    if (!Require(ApplyGisSpatialFilter(data, range, error) && data.features.size() == 1 &&
                     data.features.front().geometryParts.size() == 2,
                 "重叠下载区未取并集，或面数据未形成孔洞：" + error))
    {
        return false;
    }
    double area = 0.;
    for (const auto& ring : data.features.front().geometryParts)
    {
        double ringArea = 0.;
        for (size_t index = 1; index < ring.size(); ++index)
        {
            ringArea += ring[index - 1].longitude * ring[index].latitude -
                        ring[index].longitude * ring[index - 1].latitude;
        }
        area += ringArea / 2.;
    }
    if (!Require(std::abs(std::abs(area) - 96.) < 1.e-5, "孔洞面积或并集面积错误"))
    {
        return false;
    }
    AmapVectorDecodedData cancelled;
    cancelled.features = {Polygon(Rectangle(0., 0., 10., 10.), "region")};
    const auto before = cancelled.features.front().geometryParts;
    return Require(!ApplyGisSpatialFilter(cancelled, range, error, [] { return true; }) &&
                       cancelled.features.size() == 1 &&
                       cancelled.features.front().geometryParts.size() == before.size(),
                   "取消空间处理后仍替换了解码数据");
}

bool TestGcj02CoordinatesAndCompleteExclusion()
{
    GisSpatialRange range;
    const auto include = Rectangle(116.1, 39.9, 116.11, 39.91);
    range.SetIncludedPolygons({include});
    range.SetExcludedPolygons({Rectangle(116.102, 39.902, 116.108, 39.908)});
    AmapVectorDecodedData data;
    AmapVectorFeature point;
    point.geometryType = AmapVectorGeometryType::Point;
    const auto coordinate = GCJ02GoogleDownBase::WGS84ToGCJ02({116.105, 39.905});
    point.geometryParts = {{{coordinate.lon, coordinate.lat}}};
    data.features = {point};
    std::string error;
    if (!Require(ApplyGisSpatialFilter(data, range, error) && data.features.empty(),
                 "WGS84 排除区没有正确匹配 GCJ-02 GIS 数据"))
    {
        return false;
    }
    range.SetExcludedPolygons({Rectangle(116., 39., 117., 40.)});
    data.features = {Polygon(include, "region")};
    return Require(ApplyGisSpatialFilter(data, range, error) && data.features.empty(),
                   "完全排除的范围仍输出 GIS 数据");
}

bool TestDirectPngRendering()
{
    const auto outputRoot =
        std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "tmp/download-region-test";
    AmapVectorDownloadOptions options;
    options.centerWgs84 = {116.105, 39.905};
    options.radiusMeters = 50.;
    options.zoom = 16;
    const auto coordinate = GCJ02GoogleDownBase::WGS84ToGCJ02(options.centerWgs84);
    AmapVectorFeature point;
    point.geometryType = AmapVectorGeometryType::Point;
    point.layerName = "poi";
    point.minZoom = 1;
    point.maxZoom = 20;
    point.geometryParts = {{{coordinate.lon, coordinate.lat}}};
    AmapVectorDecodedData data;
    data.status = true;
    data.features = {point};
    AmapVectorRenderOptions render;
    render.maxConcurrency = 1;
    render.pngTileCachePath = outputRoot / "original";
    AmapVectorPngDownloader downloader;
    const auto original = downloader.Render(data, options, render);
    const auto tile = GoogleDownBase::LonLatToTile(options.centerWgs84, options.zoom);
    const auto relative = std::filesystem::path(std::to_string(tile.Zoom)) /
                          std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
    const auto visible =
        cv::imread((render.pngTileCachePath / relative).string(), cv::IMREAD_UNCHANGED);
    cv::Mat originalAlpha;
    if (!Require(original.success && !visible.empty() && visible.channels() == 4,
                 "GIS PNG 范围回归无法生成基准瓦片：" + original.error))
    {
        return false;
    }
    cv::extractChannel(visible, originalAlpha, 3);
    GisSpatialRange range;
    range.SetExcludedPolygons({Rectangle(116.1, 39.9, 116.11, 39.91)});
    options.SetGisSpatialRange(range);
    render.pngTileCachePath = outputRoot / "excluded";
    const auto excluded = downloader.Render(data, options, render);
    const auto masked =
        cv::imread((render.pngTileCachePath / relative).string(), cv::IMREAD_UNCHANGED);
    if (!Require(excluded.success && !masked.empty() && masked.channels() == 4,
                 "直接渲染 GIS PNG 时排除区处理失败：" + excluded.error))
    {
        return false;
    }
    cv::Mat excludedAlpha;
    cv::extractChannel(masked, excludedAlpha, 3);
    return Require(cv::countNonZero(originalAlpha) > 0 && cv::countNonZero(excludedAlpha) == 0 &&
                       data.features.size() == 1,
                   "GIS PNG 未变透明，或裁切改写了原解码数据");
}
} // namespace

int main()
{
    if (!TestRegionPersistenceAndEditing() || !TestRoadClippingAndBuildingExclusion() ||
        !TestPolygonHolesAndOverlappingIncludes() || !TestGcj02CoordinatesAndCompleteExclusion() ||
        !TestDirectPngRendering())
    {
        return 1;
    }
    std::cout << "DownloadRegionTest passed\n";
    return 0;
}
