#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace DownTool
{
struct FbxTerrainGridPoint
{
    uint32_t x = 0;
    uint32_t y = 0;
};

struct FbxTerrainTriangle
{
    uint32_t first = 0;
    uint32_t second = 0;
    uint32_t third = 0;
};

struct FbxTerrainTinResult
{
    std::vector<FbxTerrainGridPoint> points;
    std::vector<FbxTerrainTriangle> triangles;
    double maximumErrorMeters = 0.;
};

// 可选的三维位置与高度栅格同序；提供后按空间位置误差细化，包含球面曲率。
[[nodiscard]] FbxTerrainTinResult BuildFbxTerrainTin(
    uint32_t gridWidth, uint32_t gridHeight, const std::vector<double>& heights,
    const std::vector<FbxTerrainGridPoint>& boundaryPoints, double geometricErrorMeters,
    uint32_t maximumTriangles = 0, const std::vector<std::array<double, 3>>& positionsMeters = {});
} // namespace DownTool
