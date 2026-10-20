#include "GisSpatialIndex.h"

#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <vector>

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

bool matchesLinearScan()
{
    std::mt19937 random(20261020);
    std::uniform_real_distribution<double> coordinate(-100., 100.);
    std::uniform_real_distribution<double> width(0., 8.);
    std::vector<GisSpatialBounds> bounds;
    for (size_t index = 0; index < 20000; ++index)
    {
        const double x = coordinate(random);
        const double y = coordinate(random);
        bounds.emplace_back(x, y, x + width(random), y + width(random));
    }
    bounds.emplace_back(-200., -0.1, 200., 0.1);
    bounds.emplace_back(-0.1, -200., 0.1, 200.);
    bounds.emplace_back(0., 0., 0., 0.);
    bounds.emplace_back();
    GisSpatialIndex tree;
    if (!require(tree.Build(bounds) && tree.GetSize() == bounds.size(), "空间索引构建失败"))
    {
        return false;
    }
    std::vector<GisSpatialBounds> queries = {{-300., -300., 300., 300.}, {-1., -1., 1., 1.},
                                             {0., 0., 0., 0.},           {200., 0., 200., 0.},
                                             {500., 500., 501., 501.},   {}};
    for (size_t index = 0; index < 100; ++index)
    {
        const double x = coordinate(random);
        const double y = coordinate(random);
        queries.emplace_back(x, y, x + width(random), y + width(random));
    }
    for (const auto& query : queries)
    {
        std::vector<size_t> actual;
        std::vector<size_t> expected;
        for (size_t index = 0; index < bounds.size(); ++index)
        {
            if (query.IsValid() && bounds[index].IsValid() && bounds[index].Intersects(query))
            {
                expected.push_back(index);
            }
        }
        if (!require(tree.Query(query, actual), "空间查询意外取消"))
        {
            return false;
        }
        std::sort(actual.begin(), actual.end());
        if (!require(actual == expected, "空间查询漏选、重复，或边界接触语义与线性扫描不同"))
        {
            return false;
        }
    }
    size_t checks = 0;
    std::vector<size_t> canceledMatches;
    if (!require(!tree.Query(queries.front(), canceledMatches, [&] { return ++checks >= 5; }) &&
                     canceledMatches.empty(),
                 "查询中取消仍返回不完整候选"))
    {
        return false;
    }
    checks = 0;
    if (!require(!tree.Build(bounds, [&] { return ++checks >= 5; }) && tree.GetSize() == 0,
                 "构建中取消未清理未完成索引"))
    {
        return false;
    }
    return require(tree.Build({{1., 1., 1., 1.}, {1., 1., 1., 1.}}) &&
                       tree.Query({1., 1., 1., 1.}, canceledMatches) &&
                       canceledMatches.size() == 2 && tree.Build({}) &&
                       tree.Query(queries.front(), canceledMatches) && canceledMatches.empty(),
                   "取消后重用索引、重合点或空索引处理失败");
}
} // namespace

int main()
{
    if (!matchesLinearScan())
    {
        return 1;
    }
    std::cout << "GisSpatialIndexTest passed\n";
    return 0;
}
