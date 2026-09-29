#pragma once

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

[[nodiscard]] FbxTerrainTinResult BuildFbxTerrainTin(
    uint32_t gridWidth,
    uint32_t gridHeight,
    const std::vector<double>& heights,
    const std::vector<FbxTerrainGridPoint>& boundaryPoints,
    double geometricErrorMeters,
    uint32_t maximumTriangles = 0);
} // namespace DownTool
