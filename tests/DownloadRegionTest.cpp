#include "PostProcessTestSupport.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
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

bool TestCopyTerrainExclusion()
{
    DomDownToolPreview::DownloadRangeEditor editor;
    std::string error;
    const auto ring = Rectangle(116.1234567890123, 39.1234567890123,
                                116.2234567890123, 39.2234567890123);
    if (!Require(editor.AddRegion(ring, DownloadRegionRole::Include, error),
                 "无法准备底板排除复制样例"))
    {
        return false;
    }
    const auto original = DownloadRegionsToJson(editor.GetRegions());
    const auto selected = editor.GetSelectedId();
    if (!Require(editor.CopySelectedAs(DownloadRegionRole::ExcludeTerrain, error) &&
                     editor.GetRegions().size() == 2 && editor.GetSelectedId() == selected,
                 "复制底板排除区失败或改变了当前选区：" + error))
    {
        return false;
    }
    const auto copied = DownloadRegionsToJson(editor.GetRegions());
    if (!Require(copied[0] == original[0] &&
                     copied[1]["verticesWgs84"] == original[0]["verticesWgs84"] &&
                     copied[1]["id"] != copied[0]["id"] &&
                     copied[1]["role"] == "exclude-terrain" &&
                     GetRegionPolygons(editor.GetRegions(), DownloadRegionRole::ExcludeGis).empty(),
                 "复制改写了原区域、损失坐标精度或影响了 GIS 排除"))
    {
        return false;
    }
    DownloadRegions restored;
    if (!Require(ReadDownloadRegions(copied, restored, error) &&
                     DownloadRegionsToJson(restored) == copied,
                 "底板排除区无法保存恢复"))
    {
        return false;
    }
    editor.Undo();
    if (!Require(DownloadRegionsToJson(editor.GetRegions()) == original, "复制无法一次撤销"))
    {
        return false;
    }
    editor.Redo();
    if (!Require(DownloadRegionsToJson(editor.GetRegions()) == copied, "复制无法重做"))
    {
        return false;
    }
    editor.SetSelectedId(editor.GetRegions().back().GetId());
    return Require(editor.SetSelectedEnabled(false, error) &&
                       GetRegionPolygons(editor.GetRegions(),
                                         DownloadRegionRole::ExcludeTerrain).empty(),
                   "停用底板排除区没有生效");
}

bool TestCopyRegionRoles()
{
    const auto ring =
        Rectangle(116.1234567890123, 39.1234567890123, 116.2234567890123, 39.2234567890123);
    for (const auto sourceRole : {DownloadRegionRole::Include, DownloadRegionRole::ExcludeGis,
                                  DownloadRegionRole::ExcludeTerrain})
    {
        for (const auto targetRole : {DownloadRegionRole::Include, DownloadRegionRole::ExcludeGis,
                                      DownloadRegionRole::ExcludeTerrain})
        {
            DomDownToolPreview::DownloadRangeEditor editor;
            std::string error;
            if (!Require(!editor.CopySelectedAs(targetRole, error) && editor.GetRegions().empty() &&
                             !editor.GetCanUndo(),
                         "未选中区域时复制改动了区域或撤销记录"))
            {
                return false;
            }
            DownloadRegion source;
            source.SetId("copy-source");
            source.SetName("原区域");
            source.SetSource("原任务范围");
            source.SetRole(sourceRole);
            source.SetEnabled(false);
            source.SetVertices(ring);
            editor.SetRegions({source});
            editor.SetSelectedId(source.GetId());
            const auto original = DownloadRegionsToJson(editor.GetRegions());
            if (!Require(editor.CopySelectedAs(targetRole, error),
                         std::string(DownloadRegionRoleName(sourceRole)) + "无法复制：" + error))
            {
                return false;
            }
            const auto copied = DownloadRegionsToJson(editor.GetRegions());
            if (!Require(copied.size() == 2 && copied[0] == original[0] &&
                             copied[1]["id"] != copied[0]["id"] &&
                             copied[1]["verticesWgs84"] == original[0]["verticesWgs84"] &&
                             editor.GetRegions().back().GetSource() == source.GetSource() &&
                             editor.GetRegions().back().GetEnabled() &&
                             editor.GetRegions().back().GetRole() == targetRole &&
                             editor.GetSelectedId() == source.GetId(),
                         "复制未使用指定类型或改变了原始边界、来源、选区"))
            {
                return false;
            }
            editor.Undo();
            if (!Require(DownloadRegionsToJson(editor.GetRegions()) == original,
                         "复制区域无法一次撤销"))
            {
                return false;
            }
            editor.Redo();
            DownloadRegions restored;
            if (!Require(DownloadRegionsToJson(editor.GetRegions()) == copied &&
                             ReadDownloadRegions(copied, restored, error) &&
                             DownloadRegionsToJson(restored) == copied,
                         "复制区域无法重做或保存恢复"))
            {
                return false;
            }
        }
    }
    return true;
}

bool TestSquareRadiusEditing()
{
    DomDownToolPreview::DownloadRangeEditor editor;
    std::string error;
    const LonLat center{116.4, 39.9};
    const double latitudeOffset = 1000. / 6378137. * 180. / std::numbers::pi;
    const double longitudeOffset = latitudeOffset / std::cos(center.lat * std::numbers::pi / 180.);
    const auto square = Rectangle(center.lon - longitudeOffset, center.lat - latitudeOffset,
                                  center.lon + longitudeOffset, center.lat + latitudeOffset);
    if (!Require(editor.AddRegion(Rectangle(116., 39., 116.01, 39.01),
                                  DownloadRegionRole::ExcludeGis, error) &&
                     editor.AddRegion(square, DownloadRegionRole::Include, error) &&
                     editor.GetSelectedSquareRadiusMeters().has_value() &&
                     std::abs(*editor.GetSelectedSquareRadiusMeters() - 1000.) < 1.e-6,
                 "旧半径方框没有恢复为正方形半径"))
    {
        return false;
    }
    const auto original = DownloadRegionsToJson(editor.GetRegions());
    if (!Require(editor.SetSelectedSquareRadiusMeters(2000., error) &&
                     std::abs(*editor.GetSelectedSquareRadiusMeters() - 2000.) < 1.e-6,
                 "修改正方形半径失败：" + error))
    {
        return false;
    }
    const auto resized = DownloadRegionsToJson(editor.GetRegions());
    const auto resizedCenter = GeographicBoundsCenter(
        *CalculateGeographicBounds({editor.GetSelectedRegion()->GetVertices()}));
    if (!Require(std::abs(resizedCenter.lon - center.lon) < 1.e-12 &&
                     std::abs(resizedCenter.lat - center.lat) < 1.e-12 &&
                     resized[0] == original[0] && resized[1]["id"] == original[1]["id"],
                 "修改半径移动了中心、排除区或区域身份"))
    {
        return false;
    }
    for (const double invalidRadius : {0., -1., std::numeric_limits<double>::infinity(),
                                       std::numeric_limits<double>::quiet_NaN(), 1.e10})
    {
        if (!Require(!editor.SetSelectedSquareRadiusMeters(invalidRadius, error) &&
                         DownloadRegionsToJson(editor.GetRegions()) == resized,
                     "无效半径改写了已有区域"))
        {
            return false;
        }
    }
    editor.Undo();
    if (!Require(DownloadRegionsToJson(editor.GetRegions()) == original, "修改半径无法一次撤销"))
    {
        return false;
    }
    editor.Redo();
    DownloadRegions restored;
    if (!Require(ReadDownloadRegions(resized, restored, error), "半径修改后的区域无法保存恢复"))
    {
        return false;
    }
    editor.SetRegions(restored);
    editor.SetSelectedId(restored.back().GetId());
    if (!Require(editor.GetSelectedSquareRadiusMeters().has_value() &&
                     std::abs(*editor.GetSelectedSquareRadiusMeters() - 2000.) < 1.e-6 &&
                     editor.SetSelectedSquareRadiusMeters(500., error),
                 "任务恢复后无法继续编辑正方形半径"))
    {
        return false;
    }
    auto closedSquare = square;
    std::reverse(closedSquare.begin(), closedSquare.end());
    closedSquare.push_back(closedSquare.front());
    if (!Require(editor.SetSelectedVertices(closedSquare, error) &&
                     editor.GetSelectedSquareRadiusMeters().has_value(),
                 "闭合外环或反向外环不能识别为正方形"))
    {
        return false;
    }
    auto rectangle = square;
    for (auto& vertex : rectangle)
    {
        vertex.lon = center.lon + (vertex.lon - center.lon) * 2.;
    }
    if (!Require(editor.SetSelectedVertices(rectangle, error) &&
                     !editor.GetSelectedSquareRadiusMeters() &&
                     !editor.SetSelectedSquareRadiusMeters(1000., error),
                 "非正方形长方形错误启用了半径参数"))
    {
        return false;
    }
    auto skewed = square;
    skewed.front().lon += longitudeOffset * 0.2;
    if (!Require(editor.SetSelectedVertices(skewed, error) &&
                     !editor.GetSelectedSquareRadiusMeters(),
                 "任意四边形错误启用了半径参数"))
    {
        return false;
    }
    editor.SetSelectedId(restored.front().GetId());
    return Require(!editor.GetSelectedSquareRadiusMeters(), "GIS 排除区域错误启用了下载区半径参数");
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
    if (!TestRegionPersistenceAndEditing() || !TestCopyTerrainExclusion() ||
        !TestCopyRegionRoles() ||
        !TestSquareRadiusEditing() ||
        !TestRoadClippingAndBuildingExclusion() || !TestPolygonHolesAndOverlappingIncludes() ||
        !TestGcj02CoordinatesAndCompleteExclusion() || !TestDirectPngRendering())
    {
        return 1;
    }
    std::cout << "DownloadRegionTest passed\n";
    return 0;
}
