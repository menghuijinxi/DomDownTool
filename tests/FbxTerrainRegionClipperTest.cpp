#include "FbxTerrainRegionClipper.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

#include "PostProcessTestSupport.h"

namespace
{
using namespace DownTool;
using postprocess_test::Require;

LonLat Coordinate(double x, double y)
{
    constexpr double radius = 6378137.;
    return {x / radius * 180. / std::numbers::pi,
            (2. * std::atan(std::exp(y / radius)) - std::numbers::pi / 2.) * 180. /
                std::numbers::pi};
}

Wgs84Ring Rectangle(double west, double south, double east, double north)
{
    return {Coordinate(west, south), Coordinate(east, south), Coordinate(east, north),
            Coordinate(west, north)};
}

double SignedArea(const FbxTerrainClipTriangle& triangle)
{
    return ((triangle[1].GetX() - triangle[0].GetX()) * (triangle[2].GetY() - triangle[0].GetY()) -
            (triangle[1].GetY() - triangle[0].GetY()) * (triangle[2].GetX() - triangle[0].GetX())) *
           0.5;
}

bool CheckArea(const Wgs84Polygons& included, const Wgs84Polygons& excluded, double expectedArea,
               double offsetX = 0., double offsetY = 0.)
{
    FbxTerrainRegionClipper clipper;
    std::string error;
    if (!Require(clipper.Initialize(included, excluded, error), "初始化底板范围失败：" + error))
    {
        return false;
    }
    for (bool reverse : {false, true})
    {
        FbxTerrainClipTriangle source = {FbxTerrainClipPoint(offsetX, offsetY),
                                         FbxTerrainClipPoint(offsetX + 20., offsetY),
                                         FbxTerrainClipPoint(offsetX, offsetY + 20.)};
        if (reverse)
        {
            std::swap(source[1], source[2]);
        }
        std::vector<FbxTerrainClipTriangle> result;
        if (!Require(clipper.ClipTriangle(source, result, error), "底板三角面裁切失败：" + error))
        {
            return false;
        }
        double area = 0.;
        for (const auto& triangle : result)
        {
            const double signedArea = SignedArea(triangle);
            if (!Require(reverse ? signedArea < 0. : signedArea > 0.,
                         "裁切改变了三角面绕向或输出了退化面"))
            {
                return false;
            }
            area += std::abs(signedArea);
        }
        if (!Require(std::abs(area - expectedArea) < 0.001, "裁切面积错误：" +
                                                                std::to_string(area) + "，预期 " +
                                                                std::to_string(expectedArea)))
        {
            return false;
        }
    }
    return true;
}

bool RunTest()
{
    if (!CheckArea({}, {}, 200.) || !CheckArea({}, {Rectangle(2., 2., 4., 4.)}, 196.) ||
        !CheckArea({}, {Rectangle(2., 2., 6., 6.), Rectangle(4., 4., 8., 8.)}, 172.) ||
        !CheckArea({}, {Rectangle(-1., -1., 21., 21.)}, 0.) ||
        !CheckArea({}, {Rectangle(20., 0., 25., 5.)}, 200.) ||
        !CheckArea({}, {Rectangle(30., 30., 40., 40.)}, 200.) ||
        !CheckArea({Rectangle(0., 0., 10., 10.)}, {}, 100.) ||
        !CheckArea({Rectangle(0., 0., 10., 10.)}, {Rectangle(2., 2., 4., 4.)}, 96.) ||
        !CheckArea({Rectangle(0., 0., 10., 10.)}, {Rectangle(-1., -1., 21., 21.)}, 0.) ||
        !CheckArea({Rectangle(0., 0., 4., 4.), Rectangle(6., 0., 10., 4.)}, {}, 32.) ||
        !CheckArea({}, {Rectangle(12960002., 4850002., 12960004., 4850004.)}, 196., 12960000.,
                   4850000.))
    {
        return false;
    }
    const Wgs84Ring concave = {Coordinate(0., 0.), Coordinate(10., 0.), Coordinate(10., 2.),
                               Coordinate(2., 2.), Coordinate(2., 10.), Coordinate(0., 10.)};
    if (!CheckArea({concave}, {}, 36.))
    {
        return false;
    }

    FbxTerrainRegionClipper clipper;
    std::string error;
    bool cancelled = false;
    if (!Require(
            clipper.Initialize({}, {Rectangle(2., 2., 4., 4.)}, error, [&] { return cancelled; }),
            "无法准备取消测试"))
    {
        return false;
    }
    bool visible = true;
    const auto minimum = Coordinate(2.5, 2.5);
    const auto maximum = Coordinate(3.5, 3.5);
    if (!Require(clipper.HasVisibleArea({minimum.lon, maximum.lon, minimum.lat, maximum.lat},
                                        visible, error) &&
                     !visible,
                 "完全排除的地块仍被保留"))
    {
        return false;
    }
    const FbxTerrainClipTriangle triangle = {
        FbxTerrainClipPoint(0., 0.), FbxTerrainClipPoint(20., 0.), FbxTerrainClipPoint(0., 20.)};
    std::vector<FbxTerrainClipTriangle> result;
    cancelled = true;
    if (!Require(!clipper.ClipTriangle(triangle, result, error) && result.empty() && !error.empty(),
                 "取消后仍发布了部分裁切结果"))
    {
        return false;
    }
    cancelled = false;
    auto invalid = triangle;
    invalid[0] = FbxTerrainClipPoint(std::numeric_limits<double>::quiet_NaN(), 0.);
    if (!Require(!clipper.ClipTriangle(invalid, result, error) && result.empty(),
                 "裁切接受了无效顶点") ||
        !Require(!clipper.Initialize({{Coordinate(0., 0.), Coordinate(2., 2.), Coordinate(0., 2.),
                                       Coordinate(2., 0.)}},
                                     {}, error),
                 "裁切接受了自交范围"))
    {
        return false;
    }
    std::cout << "FbxTerrainRegionClipperTest passed\n";
    return true;
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
