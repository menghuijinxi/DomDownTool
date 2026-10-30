#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

#include "FbxTerrainTriangulator.h"

namespace
{
using DownTool::FbxTerrainGridPoint;
using DownTool::FbxTerrainTinResult;

std::vector<FbxTerrainGridPoint> squareBoundary(uint32_t width, uint32_t height)
{
    return {{0, 0}, {width, 0}, {width, height}, {0, height}};
}

std::vector<double> flatHeights(uint32_t width, uint32_t height, double value)
{
    return std::vector<double>(
        static_cast<std::size_t>(width + 1) * (height + 1), value);
}

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void requireValidTriangles(const FbxTerrainTinResult& result)
{
    for (const auto& triangle : result.triangles)
    {
        require(triangle.first < result.points.size(), "triangle index is out of range");
        require(triangle.second < result.points.size(), "triangle index is out of range");
        require(triangle.third < result.points.size(), "triangle index is out of range");
    }
}

void verifyPlaneUsesBoundaryOnly()
{
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 8;
    const FbxTerrainTinResult result = DownTool::BuildFbxTerrainTin(
        width, height, flatHeights(width, height, 42.), squareBoundary(width, height),
        0.01);
    require(result.points.size() == 4, "flat terrain should not add interior points");
    require(result.triangles.size() == 2, "flat terrain should be two triangles");
    require(std::abs(result.maximumErrorMeters) <= 1e-9,
            "flat terrain should have zero interpolation error");
    requireValidTriangles(result);
}

void verifyPeakAddsDetail()
{
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 8;
    std::vector<double> heights = flatHeights(width, height, 0.);
    heights[static_cast<std::size_t>(4) * (width + 1) + 4] = 100.;
    const FbxTerrainTinResult result = DownTool::BuildFbxTerrainTin(
        width, height, heights, squareBoundary(width, height), 1.);
    require(result.points.size() > 4, "terrain peak should add an interior point");
    require(result.triangles.size() > 2, "terrain peak should add triangles");
    requireValidTriangles(result);
}

void verifyCurvatureRefinesConstantHeights()
{
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 16;
    const auto heights = flatHeights(width, height, 0.);
    std::vector<std::array<double, 3>> positions(heights.size());
    for (uint32_t y = 0; y <= height; ++y)
    {
        for (uint32_t x = 0; x <= width; ++x)
        {
            positions[std::size_t(y) * (width + 1) + x] = {
                double(x), double(y),
                10. * std::sin(x * 3.141592653589793 / width) *
                    std::sin(y * 3.141592653589793 / height)};
        }
    }
    const auto boundary = squareBoundary(width, height);
    const auto result =
        DownTool::BuildFbxTerrainTin(width, height, heights, boundary, .1, 0, positions);
    require(result.triangles.size() > 2, "constant heights erased three-dimensional curvature");
    require(result.maximumErrorMeters <= .1, "three-dimensional error exceeds tolerance");
    const auto limited =
        DownTool::BuildFbxTerrainTin(width, height, heights, boundary, .1, 2, positions);
    require(limited.triangles.size() == 2 && limited.maximumErrorMeters > 9.,
            "triangle budget hid the remaining three-dimensional error");
    positions.pop_back();
    require(DownTool::BuildFbxTerrainTin(width, height, heights, boundary, .1, 0, positions)
                .triangles.empty(),
            "three-dimensional position buffer with incorrect size was accepted");
}

void verifyInvalidInputIsRejected()
{
    const std::vector<FbxTerrainGridPoint> boundary = squareBoundary(4, 4);
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, std::vector<double>(4), boundary, 1.)
                .points.empty(),
            "height buffer with an invalid size should be rejected");
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, flatHeights(4, 4, 0.), {{0, 0}, {5, 0}, {4, 4}}, 1.)
                .points.empty(),
            "boundary point outside the grid should be rejected");
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, flatHeights(4, 4, 0.), boundary, -1.)
                .points.empty(),
            "negative geometric error should be rejected");
    require(DownTool::BuildFbxTerrainTin(4, 4, flatHeights(4, 4, 0.),
                {{0, 0}, {3, 0}, {1, 0}, {4, 0}, {4, 4}, {0, 4}}, 1.)
                .points.empty(), "overlapping boundary segments should be rejected");
    require(DownTool::BuildFbxTerrainTin(
                4, 4, flatHeights(4, 4, 0.), boundary, 1., 1).points.empty(),
            "impossible one-face rectangle should be rejected");
}

void verifyRefinementBatchKeepsRequestedAccuracy()
{
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    constexpr double maximumErrorMeters = 0.5;
    std::vector<double> heights = flatHeights(width, height, 0.);
    for (uint32_t y = 1; y < height; ++y)
    {
        for (uint32_t x = 1; x < width; ++x)
        {
            heights[static_cast<std::size_t>(y) * (width + 1) + x] =
                10. * std::sin(static_cast<double>(x) * 0.3) *
                std::cos(static_cast<double>(y) * 0.2);
        }
    }

    const FbxTerrainTinResult result = DownTool::BuildFbxTerrainTin(
        width, height, heights, squareBoundary(width, height),
        maximumErrorMeters);
    require(!result.triangles.empty(), "batched refinement should produce a mesh");
    require(result.maximumErrorMeters <= maximumErrorMeters,
            "batched refinement should keep the requested geometric error");
    requireValidTriangles(result);
}

void verifyRefinementCanExceedLegacyTriangleLimit()
{
    constexpr uint32_t width = 92;
    constexpr uint32_t height = 92;
    constexpr double maximumErrorMeters = 0.01;
    constexpr std::size_t legacyTriangleLimit = 16384;
    std::vector<double> heights = flatHeights(width, height, 0.);
    for (uint32_t y = 1; y < height; ++y)
    {
        for (uint32_t x = 1; x < width; ++x)
        {
            heights[static_cast<std::size_t>(y) * (width + 1) + x] =
                (x + y) % 2 == 0 ? 100. : -100.;
        }
    }

    const FbxTerrainTinResult result = DownTool::BuildFbxTerrainTin(
        width, height, heights, squareBoundary(width, height),
        maximumErrorMeters);
    require(result.triangles.size() > legacyTriangleLimit,
            "TIN refinement still stops at the legacy triangle limit");
    require(result.maximumErrorMeters <= maximumErrorMeters,
            "TIN exceeding the legacy limit did not reach the requested accuracy");
    requireValidTriangles(result);
}

double verifyCoverageAndError(
    const FbxTerrainTinResult& result, uint32_t width, uint32_t height,
    const std::vector<double>& heights, const std::vector<FbxTerrainGridPoint>& boundary)
{
    require(!result.triangles.empty(), "empty budgeted mesh");
    requireValidTriangles(result);
    std::set<std::pair<uint32_t, uint32_t>> unique;
    for (const auto p : result.points)
    {
        require(unique.emplace(p.x, p.y).second, "duplicate grid vertex");
    }
    for (const auto p : boundary)
    {
        require(unique.contains({p.x, p.y}), "fixed boundary vertex was removed");
    }
    std::map<std::pair<uint32_t, uint32_t>, unsigned> edges;
    std::vector<double> raster(heights.size(), std::numeric_limits<double>::quiet_NaN());
    double area = 0.;
    const auto cross = [](FbxTerrainGridPoint a, FbxTerrainGridPoint b,
                          FbxTerrainGridPoint p)
    {
        return (double(b.x) - a.x) * (double(p.y) - a.y) -
            (double(b.y) - a.y) * (double(p.x) - a.x);
    };
    const auto z = [&](FbxTerrainGridPoint p)
    {
        return heights[std::size_t(p.y) * (width + 1) + p.x];
    };
    for (const auto t : result.triangles)
    {
        const auto a = result.points[t.first];
        const auto b = result.points[t.second];
        const auto c = result.points[t.third];
        const double twiceArea = cross(a, b, c);
        require(twiceArea > 0., "reversed or degenerate triangle");
        area += twiceArea / 2.;
        ++edges[std::minmax(t.first, t.second)];
        ++edges[std::minmax(t.second, t.third)];
        ++edges[std::minmax(t.third, t.first)];
        for (uint32_t y = std::min({a.y, b.y, c.y}); y <= std::max({a.y, b.y, c.y}); ++y)
        {
            for (uint32_t x = std::min({a.x, b.x, c.x}); x <= std::max({a.x, b.x, c.x}); ++x)
            {
                const double wa = cross(b, c, {x, y}) / twiceArea;
                const double wb = cross(c, a, {x, y}) / twiceArea;
                const double wc = cross(a, b, {x, y}) / twiceArea;
                if (wa < 0. || wb < 0. || wc < 0.)
                {
                    continue;
                }
                double& sample = raster[std::size_t(y) * (width + 1) + x];
                const double value = wa * z(a) + wb * z(b) + wc * z(c);
                require(!std::isfinite(sample) || std::abs(sample - value) < 1e-7,
                        "overlapping faces have inconsistent heights");
                sample = value;
            }
        }
    }
    require(area == double(width) * height, "mesh does not cover the rectangle");
    std::size_t boundaryEdges = 0;
    for (const auto& [edge, count] : edges)
    {
        require(count <= 2, "non-manifold edge");
        if (count == 1)
        {
            ++boundaryEdges;
            const auto a = result.points[edge.first];
            const auto b = result.points[edge.second];
            require((a.x == b.x && (a.x == 0 || a.x == width)) ||
                        (a.y == b.y && (a.y == 0 || a.y == height)),
                    "internal open edge");
        }
    }
    require(boundaryEdges == boundary.size(), "boundary changed during refinement");
    double maximum = 0.;
    for (std::size_t i = 0; i < raster.size(); ++i)
    {
        require(std::isfinite(raster[i]), "uncovered grid sample");
        maximum = std::max(maximum, std::abs(raster[i] - heights[i]));
    }
    require(std::abs(maximum - result.maximumErrorMeters) < 1e-7,
            "reported error does not cover all source samples");
    return maximum;
}

void verifyBudgetsRetainShapeAndTopology()
{
    constexpr uint32_t width = 63;
    constexpr uint32_t height = 47;
    auto heights = flatHeights(width, height, 0.);
    for (uint32_t y = 0; y <= height; ++y)
    {
        for (uint32_t x = 0; x <= width; ++x)
        {
            const double nx = double(x) / width;
            const double ny = double(y) / height;
            heights[std::size_t(y) * (width + 1) + x] =
                180. * std::sin(nx * 3.141592653589793) *
                std::sin(ny * 3.141592653589793);
        }
    }
    const auto boundary = squareBoundary(width, height);
    for (uint32_t budget : {2u, 3u, 4u, 31u, 128u, 255u})
    {
        const auto result = DownTool::BuildFbxTerrainTin(
            width, height, heights, boundary, 0., budget);
        require(result.triangles.size() <= budget, "triangle budget exceeded");
        const double error = verifyCoverageAndError(result, width, height, heights, boundary);
        if (budget >= 128)
        {
            require(error < 12., "low-poly mountain lost its overall shape");
        }
    }
}

void verifyRandomAndThinGrids()
{
    std::mt19937 random(20260929);
    for (const auto [width, height] :
         {std::pair{1u, 33u}, {33u, 1u}, {13u, 37u}, {48u, 32u}})
    {
        auto heights = flatHeights(width, height, 0.);
        for (double& z : heights)
        {
            z = double(random() % 10000) / 10.;
        }
        std::vector<FbxTerrainGridPoint> boundary;
        for (uint32_t x = 0; x < width; ++x)
        {
            boundary.push_back({x, 0});
        }
        for (uint32_t y = 0; y < height; ++y)
        {
            boundary.push_back({width, y});
        }
        for (uint32_t x = width; x > 0; --x)
        {
            boundary.push_back({x, height});
        }
        for (uint32_t y = height; y > 0; --y)
        {
            boundary.push_back({0, y});
        }
        for (uint32_t budget : {200u, 601u, 4096u})
        {
            const auto result = DownTool::BuildFbxTerrainTin(
                width, height, heights, boundary, 0., budget);
            require(result.triangles.size() <= budget, "random mesh exceeds budget");
            verifyCoverageAndError(result, width, height, heights, boundary);
        }
    }
    auto invalid = flatHeights(4, 4, 0.);
    invalid[3] = std::numeric_limits<double>::quiet_NaN();
    require(DownTool::BuildFbxTerrainTin(4, 4, invalid, squareBoundary(4, 4), 1.)
                .triangles.empty(), "non-finite input accepted");
}
} // namespace

int main()
{
    try
    {
        verifyPlaneUsesBoundaryOnly();
        verifyPeakAddsDetail();
        verifyCurvatureRefinesConstantHeights();
        verifyInvalidInputIsRejected();
        verifyRefinementBatchKeepsRequestedAccuracy();
        verifyRefinementCanExceedLegacyTriangleLimit();
        verifyBudgetsRetainShapeAndTopology();
        verifyRandomAndThinGrids();
        std::cout << "FbxTerrainTriangulatorTest passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FbxTerrainTriangulatorTest failed: " << exception.what() << '\n';
        return 1;
    }
}
