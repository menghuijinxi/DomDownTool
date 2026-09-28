#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
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
        0.01, 64);
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
        width, height, heights, squareBoundary(width, height), 1., 64);
    require(result.points.size() > 4, "terrain peak should add an interior point");
    require(result.triangles.size() > 2, "terrain peak should add triangles");
    requireValidTriangles(result);
}

void verifyInvalidInputIsRejected()
{
    const std::vector<FbxTerrainGridPoint> boundary = squareBoundary(4, 4);
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, std::vector<double>(4), boundary, 1., 32)
                .points.empty(),
            "height buffer with an invalid size should be rejected");
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, flatHeights(4, 4, 0.), {{0, 0}, {5, 0}, {4, 4}}, 1., 32)
                .points.empty(),
            "boundary point outside the grid should be rejected");
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, flatHeights(4, 4, 0.), boundary, 1., 0)
                .points.empty(),
            "zero triangle budget should be rejected");
    require(DownTool::BuildFbxTerrainTin(
                 4, 4, flatHeights(4, 4, 0.), boundary, -1., 32)
                .points.empty(),
            "negative geometric error should be rejected");
}

void verifyTriangleBudgetIsHard()
{
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 16;
    std::vector<double> heights = flatHeights(width, height, 0.);
    for (uint32_t y = 2; y < height; y += 4)
    {
        for (uint32_t x = 2; x < width; x += 4)
        {
            heights[static_cast<std::size_t>(y) * (width + 1) + x] =
                static_cast<double>((x + y) * 10);
        }
    }
    constexpr std::size_t maximumTriangleCount = 6;
    const FbxTerrainTinResult result = DownTool::BuildFbxTerrainTin(
        width, height, heights, squareBoundary(width, height), 1.,
        maximumTriangleCount);
    require(!result.triangles.empty(), "terrain budget test should produce a mesh");
    require(result.triangles.size() <= maximumTriangleCount,
            "TIN exceeded the configured triangle budget");
    requireValidTriangles(result);
}

void verifyRefinementBatchKeepsRequestedAccuracy()
{
    constexpr uint32_t width = 64;
    constexpr uint32_t height = 64;
    constexpr double maximumErrorMeters = 0.5;
    constexpr std::size_t maximumTriangleCount = 8192;
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
        maximumErrorMeters, maximumTriangleCount);
    require(!result.triangles.empty(), "batched refinement should produce a mesh");
    require(result.maximumErrorMeters <= maximumErrorMeters,
            "batched refinement should keep the requested geometric error");
    require(result.triangles.size() <= maximumTriangleCount,
            "batched refinement exceeded the configured triangle budget");
    requireValidTriangles(result);
}
} // namespace

int main()
{
    try
    {
        verifyPlaneUsesBoundaryOnly();
        verifyPeakAddsDetail();
        verifyInvalidInputIsRejected();
        verifyTriangleBudgetIsHard();
        verifyRefinementBatchKeepsRequestedAccuracy();
        std::cout << "FbxTerrainTriangulatorTest passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FbxTerrainTriangulatorTest failed: " << exception.what() << '\n';
        return 1;
    }
}
