#include "GisSpatialFilter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "polyclipping/clipper.hpp"

#include "GCJ02GoogleDownBase.h"

namespace
{
using namespace DownTool;

bool require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

Wgs84Ring rectangle(double west, double south, double east, double north)
{
    return {{west, south}, {east, south}, {east, north}, {west, north}};
}

std::vector<AmapVectorCoordinate> coordinates(const Wgs84Ring& ring)
{
    std::vector<AmapVectorCoordinate> result;
    for (const auto& point : ring)
    {
        result.push_back({point.lon, point.lat});
    }
    result.push_back(result.front());
    return result;
}

AmapVectorFeature polygon(const Wgs84Ring& ring, uint64_t uid = 1)
{
    AmapVectorFeature feature;
    feature.layerName = "region";
    feature.uid = uid;
    feature.geometryType = AmapVectorGeometryType::Polygon;
    feature.geometryParts.push_back(coordinates(ring));
    return feature;
}

double area(const AmapVectorDecodedData& decoded)
{
    double result = 0.;
    for (const auto& feature : decoded.features)
    {
        for (const auto& ring : feature.geometryParts)
        {
            double signedArea = 0.;
            for (size_t index = 1; index < ring.size(); ++index)
            {
                signedArea += ring[index - 1].longitude * ring[index].latitude -
                              ring[index].longitude * ring[index - 1].latitude;
            }
            result += signedArea / 2.;
        }
    }
    return result;
}

bool cancellationInsideOneFeature()
{
    Wgs84Ring ring;
    for (size_t index = 0; index < 10000; ++index)
    {
        const double angle = index * 2. * std::numbers::pi / 10000.;
        ring.emplace_back(5. + std::cos(angle), 5. + std::sin(angle));
    }
    AmapVectorDecodedData decoded;
    decoded.features.push_back(polygon(ring));
    GisSpatialRange range;
    range.SetIncludedPolygons({rectangle(0., 0., 10., 10.)});
    size_t checks = 0;
    std::string error;
    const bool success =
        ApplyGisSpatialFilter(decoded, range, error, [&] { return ++checks >= 8; });
    return require(!success && error == "GIS 范围处理已取消。" && decoded.features.size() == 1 &&
                       decoded.features.front().geometryParts.front().size() == 10001,
                   "单个大要素处理没有及时检查取消，或取消覆盖了原始数据");
}

bool nestedHolesAndIndependentIslands()
{
    AmapVectorDecodedData decoded;
    auto feature = polygon(rectangle(0., 0., 10., 10.));
    feature.geometryParts.push_back(coordinates(rectangle(2., 2., 8., 8.)));
    feature.geometryParts.push_back(coordinates(rectangle(3., 3., 7., 7.)));
    feature.geometryParts.push_back(coordinates(rectangle(4., 4., 6., 6.)));
    feature.geometryParts.push_back(coordinates(rectangle(12., 0., 14., 2.)));
    decoded.features.push_back(feature);
    GisSpatialRange range;
    range.SetIncludedPolygons({rectangle(-1., -1., 15., 11.)});
    std::string error;
    if (!require(ApplyGisSpatialFilter(decoded, range, error), error) ||
        !require(decoded.features.size() == 3 && std::abs(area(decoded) - 80.) < 1.e-6,
                 "嵌套孔洞、孔内岛或独立面丢失"))
    {
        return false;
    }
    size_t holes = 0;
    for (const auto& output : decoded.features)
    {
        holes += output.geometryParts.size() - 1;
        if (!require(output.uid == feature.uid && output.layerName == feature.layerName,
                     "裁切没有保留要素属性"))
        {
            return false;
        }
    }
    return require(holes == 2, "孔洞没有归属于直接包围它的外环");
}

bool overlappingAndTouchingContours()
{
    AmapVectorDecodedData decoded;
    auto feature = polygon(rectangle(0., 0., 4., 4.));
    feature.geometryParts.push_back(coordinates(rectangle(2., 0., 6., 4.)));
    feature.geometryParts.push_back(coordinates(rectangle(6., 4., 8., 6.)));
    decoded.features.push_back(feature);
    GisSpatialRange range;
    range.SetIncludedPolygons({rectangle(-1., -1., 9., 7.)});
    std::string error;
    return require(ApplyGisSpatialFilter(decoded, range, error) &&
                       std::abs(area(decoded) - 20.) < 1.e-6,
                   "重叠环的奇偶填充或顶点接触后的面积不正确：" + error);
}

bool touchingLobesWithOppositeWinding()
{
    AmapVectorDecodedData decoded;
    decoded.features.push_back(polygon({{0., 0.},
                                        {4., 0.},
                                        {4., 4.},
                                        {0., 4.},
                                        {0., 0.},
                                        {0., -2.},
                                        {-2., -2.},
                                        {-2., 0.},
                                        {0., 0.}}));
    GisSpatialRange range;
    range.SetIncludedPolygons({rectangle(-3., -3., 5., 5.)});
    std::string error;
    return require(ApplyGisSpatialFilter(decoded, range, error) && decoded.features.size() == 2 &&
                       std::abs(area(decoded) - 20.) < 1.e-6 &&
                       decoded.features.front().geometryParts.size() == 1 &&
                       decoded.features.back().geometryParts.size() == 1,
                   "反向自接触轮廓拆出的独立外环被误判为孔洞：" + error);
}

bool crossingBuildingAndExclusionContact()
{
    AmapVectorDecodedData decoded;
    auto crossing = polygon(rectangle(-1., 4., 11., 6.), 1);
    crossing.layerName = "building";
    auto touching = polygon(rectangle(2., 2., 4., 4.), 2);
    touching.layerName = "building";
    auto retained = polygon(rectangle(1., 7., 2., 8.), 3);
    retained.layerName = "building";
    decoded.features = {crossing, touching, retained};
    GisSpatialRange range;
    range.SetIncludedPolygons({rectangle(0., 0., 10., 10.)});
    range.SetExcludedPolygons({rectangle(4., 4., 6., 6.)});
    std::string error;
    return require(ApplyGisSpatialFilter(decoded, range, error) && decoded.features.size() == 1 &&
                       decoded.features.front().uid == 3,
                   "跨空间节点建筑漏检，或边界接触建筑没有整栋剔除：" + error);
}

bool degenerateBuildingsAreNotKept()
{
    AmapVectorDecodedData decoded;
    auto feature = polygon({{1., 1.}, {1., 1.}, {1., 1.}}, 1);
    feature.layerName = "building";
    decoded.features.push_back(feature);
    GisSpatialRange range;
    range.SetIncludedPolygons({rectangle(0., 0., 10., 10.)});
    std::string error;
    return require(ApplyGisSpatialFilter(decoded, range, error) && decoded.features.empty(),
                   "包围盒快速路径保留了退化建筑：" + error);
}

Wgs84Polygons readRings(const nlohmann::json& value)
{
    Wgs84Polygons polygons;
    for (const auto& ring : value)
    {
        Wgs84Ring vertices;
        for (const auto& vertex : ring)
        {
            vertices.emplace_back(vertex.at(0).get<double>(), vertex.at(1).get<double>());
        }
        polygons.push_back(std::move(vertices));
    }
    return polygons;
}

ClipperLib::Paths clipPaths(const Wgs84Polygons& polygons)
{
    ClipperLib::Paths paths;
    for (const auto& ring : polygons)
    {
        ClipperLib::Path path;
        for (const auto& vertex : ring)
        {
            const auto gcj = GCJ02GoogleDownBase::WGS84ToGCJ02(vertex);
            path.emplace_back(static_cast<ClipperLib::cInt>(std::llround(gcj.lon * 1.e8)),
                              static_cast<ClipperLib::cInt>(std::llround(gcj.lat * 1.e8)));
        }
        if (!ClipperLib::Orientation(path))
        {
            std::reverse(path.begin(), path.end());
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

ClipperLib::Paths featurePaths(const AmapVectorDecodedData& decoded)
{
    ClipperLib::Paths paths;
    for (const auto& feature : decoded.features)
    {
        for (const auto& ring : feature.geometryParts)
        {
            ClipperLib::Path path;
            for (const auto& vertex : ring)
            {
                path.emplace_back(
                    static_cast<ClipperLib::cInt>(std::llround(vertex.longitude * 1.e8)),
                    static_cast<ClipperLib::cInt>(std::llround(vertex.latitude * 1.e8)));
            }
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

bool realFixture(const std::filesystem::path& path)
{
    using namespace ClipperLib;
    std::ifstream input(path);
    const auto fixture = nlohmann::json::parse(input);
    const auto& source = fixture.at("feature");
    AmapVectorFeature feature;
    feature.geometryType = AmapVectorGeometryType::Polygon;
    feature.layerName = source.at("layerName").get<std::string>();
    feature.mainKey = source.at("mainKey").get<uint32_t>();
    feature.subKey = source.at("subKey").get<uint32_t>();
    for (const auto& ring : source.at("geometryParts"))
    {
        std::vector<AmapVectorCoordinate> vertices;
        for (const auto& point : ring)
        {
            vertices.push_back({point.at(0).get<double>(), point.at(1).get<double>()});
        }
        feature.geometryParts.push_back(std::move(vertices));
    }
    GisSpatialRange range;
    range.SetIncludedPolygons(readRings(fixture.at("includedWgs84")));
    range.SetExcludedPolygons(readRings(fixture.at("excludedWgs84")));
    AmapVectorDecodedData decoded;
    decoded.features.push_back(feature);
    const auto original = decoded;
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    if (!require(ApplyGisSpatialFilter(decoded, range, error), error))
    {
        return false;
    }
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "fixture_seconds=" << seconds << " features=" << decoded.features.size()
              << std::endl;

    Clipper maskClipper;
    maskClipper.AddPaths(clipPaths(range.GetIncludedPolygons()), ptSubject, true);
    maskClipper.AddPaths(clipPaths(range.GetExcludedPolygons()), ptClip, true);
    Paths mask;
    if (!require(maskClipper.Execute(ctDifference, mask, pftNonZero, pftNonZero),
                 "无法建立样例基准掩膜"))
    {
        return false;
    }
    Clipper legacy;
    legacy.StrictlySimple(true);
    legacy.AddPaths(featurePaths(original), ptSubject, true);
    legacy.AddPaths(mask, ptClip, true);
    PolyTree reference;
    const auto referenceStart = std::chrono::steady_clock::now();
    if (!require(legacy.Execute(ctIntersection, reference, pftEvenOdd, pftNonZero),
                 "旧实现基准裁切失败"))
    {
        return false;
    }
    std::cout
        << "legacy_seconds="
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - referenceStart).count()
        << std::endl;
    Paths expected;
    ClosedPathsFromPolyTree(reference, expected);
    Clipper difference;
    difference.AddPaths(expected, ptSubject, true);
    difference.AddPaths(featurePaths(decoded), ptClip, true);
    Paths changed;
    if (!require(difference.Execute(ctXor, changed, pftNonZero, pftNonZero) && changed.empty(),
                 "新空间处理与旧严格简单化结果存在几何差异"))
    {
        return false;
    }
    auto canceled = original;
    size_t checks = 0;
    const bool success =
        ApplyGisSpatialFilter(canceled, range, error, [&] { return ++checks >= 64; });
    return require(seconds < 5. && !success && canceled.features.size() == 1 &&
                       canceled.features.front().geometryParts.size() ==
                           feature.geometryParts.size(),
                   "真实慢样例仍耗时过长，或取消改写了原始数据");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--fixture")
    {
        return realFixture(std::filesystem::u8path(argv[2])) ? 0 : 1;
    }
    if (!cancellationInsideOneFeature() || !nestedHolesAndIndependentIslands() ||
        !overlappingAndTouchingContours() || !touchingLobesWithOppositeWinding() ||
        !crossingBuildingAndExclusionContact() || !degenerateBuildingsAreNotKept())
    {
        return 1;
    }
    std::cout << "GisSpatialFilterTest passed\n";
    return 0;
}
