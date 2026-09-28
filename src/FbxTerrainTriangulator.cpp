#include "FbxTerrainTriangulator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CDT.h"

namespace
{
using DownTool::FbxTerrainGridPoint;
using DownTool::FbxTerrainTinResult;
using DownTool::FbxTerrainTriangle;

struct GridCandidate
{
    FbxTerrainGridPoint point;
    double height = 0.;
};

struct TriangleGeometry
{
    std::array<uint32_t, 3> vertices{};
    double minX = 0.;
    double maxX = 0.;
    double minY = 0.;
    double maxY = 0.;
};

struct CandidateError
{
    std::size_t candidateIndex = 0;
    double error = 0.;
};

// 候选点会在下一轮重新校验；扩大批次可避免每 16 个点全量重建一次 CDT。
constexpr std::size_t kMaximumRefinementBatchSize = 256;

uint64_t GridPointKey(const FbxTerrainGridPoint& point)
{
    return (static_cast<uint64_t>(point.x) << 32) | point.y;
}

bool IsInteriorPoint(const FbxTerrainGridPoint& point, uint32_t width, uint32_t height)
{
    return point.x > 0 && point.x < width && point.y > 0 && point.y < height;
}

double HeightAt(
    const std::vector<double>& heights,
    uint32_t width,
    const FbxTerrainGridPoint& point)
{
    return heights[static_cast<std::size_t>(point.y) * (width + 1) + point.x];
}

bool IsLocalExtremum(
    const std::vector<double>& heights,
    uint32_t width,
    uint32_t height,
    const FbxTerrainGridPoint& point)
{
    const double center = HeightAt(heights, width, point);
    bool hasLower = false;
    bool hasHigher = false;
    for (int32_t y = -1; y <= 1; ++y)
    {
        for (int32_t x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
            {
                continue;
            }
            const uint32_t neighborX = static_cast<uint32_t>(
                static_cast<int32_t>(point.x) + x);
            const uint32_t neighborY = static_cast<uint32_t>(
                static_cast<int32_t>(point.y) + y);
            if (neighborX > width || neighborY > height)
            {
                continue;
            }
            const double neighbor = HeightAt(
                heights, width, {neighborX, neighborY});
            hasLower = hasLower || center > neighbor;
            hasHigher = hasHigher || center < neighbor;
        }
    }
    return hasLower != hasHigher;
}

std::vector<GridCandidate> BuildCandidates(
    uint32_t width,
    uint32_t height,
    const std::vector<double>& heights,
    const std::set<uint64_t>& selected)
{
    const uint64_t area = static_cast<uint64_t>(width) * height;
    const uint32_t stride = area > 65536 ? static_cast<uint32_t>(std::ceil(
        std::sqrt(static_cast<double>(area) / 65536.))) : 1;
    std::unordered_map<uint64_t, GridCandidate> candidates;
    candidates.reserve(static_cast<std::size_t>(std::min<uint64_t>(area, 65536)));
    for (uint32_t y = 1; y < height; y += stride)
    {
        for (uint32_t x = 1; x < width; x += stride)
        {
            const FbxTerrainGridPoint point{x, y};
            if (selected.contains(GridPointKey(point)))
            {
                continue;
            }
            candidates.emplace(GridPointKey(point), GridCandidate{
                point, HeightAt(heights, width, point)});
        }
    }

    // 粗采样之外始终保留局部峰谷，避免窄山脊或孤立尖峰恰好落在步长之间。
    for (uint32_t y = 1; y < height; ++y)
    {
        for (uint32_t x = 1; x < width; ++x)
        {
            const FbxTerrainGridPoint point{x, y};
            if (!selected.contains(GridPointKey(point)) &&
                IsLocalExtremum(heights, width, height, point))
            {
                candidates.emplace(GridPointKey(point), GridCandidate{
                    point, HeightAt(heights, width, point)});
            }
        }
    }

    std::vector<GridCandidate> result;
    result.reserve(candidates.size());
    for (const auto& [key, candidate] : candidates)
    {
        (void)key;
        result.push_back(candidate);
    }
    return result;
}

std::vector<CDT::V2d<double>> ToCdtVertices(
    const std::vector<FbxTerrainGridPoint>& points)
{
    std::vector<CDT::V2d<double>> vertices;
    vertices.reserve(points.size());
    for (const FbxTerrainGridPoint& point : points)
    {
        vertices.push_back(CDT::V2d<double>::make(
            static_cast<double>(point.x), static_cast<double>(point.y)));
    }
    return vertices;
}

std::vector<TriangleGeometry> BuildTriangleGeometry(
    const CDT::TriangleVec& triangles,
    const std::vector<CDT::V2d<double>>& vertices)
{
    std::vector<TriangleGeometry> geometry;
    geometry.reserve(triangles.size());
    for (const CDT::Triangle& triangle : triangles)
    {
        TriangleGeometry item;
        for (std::size_t index = 0; index < 3; ++index)
        {
            item.vertices[index] = triangle.vertices[index];
            const auto& vertex = vertices[item.vertices[index]];
            if (index == 0)
            {
                item.minX = item.maxX = vertex.x;
                item.minY = item.maxY = vertex.y;
            }
            else
            {
                item.minX = std::min(item.minX, vertex.x);
                item.maxX = std::max(item.maxX, vertex.x);
                item.minY = std::min(item.minY, vertex.y);
                item.maxY = std::max(item.maxY, vertex.y);
            }
        }
        geometry.push_back(item);
    }
    return geometry;
}

bool TryInterpolateTriangleHeight(
    const TriangleGeometry& triangle,
    const std::vector<CDT::V2d<double>>& vertices,
    const std::vector<double>& pointHeights,
    const FbxTerrainGridPoint& point,
    double& interpolatedHeight)
{
    const auto& a = vertices[triangle.vertices[0]];
    const auto& b = vertices[triangle.vertices[1]];
    const auto& c = vertices[triangle.vertices[2]];
    const double denominator = (b.y - c.y) * (a.x - c.x) +
        (c.x - b.x) * (a.y - c.y);
    if (std::abs(denominator) <= std::numeric_limits<double>::epsilon())
    {
        return false;
    }

    const double x = static_cast<double>(point.x);
    const double y = static_cast<double>(point.y);
    const double weightA = ((b.y - c.y) * (x - c.x) +
        (c.x - b.x) * (y - c.y)) / denominator;
    const double weightB = ((c.y - a.y) * (x - c.x) +
        (a.x - c.x) * (y - c.y)) / denominator;
    const double weightC = 1. - weightA - weightB;
    constexpr double tolerance = 1e-9;
    if (weightA < -tolerance || weightB < -tolerance || weightC < -tolerance)
    {
        return false;
    }
    interpolatedHeight = weightA * pointHeights[triangle.vertices[0]] +
        weightB * pointHeights[triangle.vertices[1]] +
        weightC * pointHeights[triangle.vertices[2]];
    return true;
}

std::vector<CandidateError> CalculateCandidateErrors(
    const std::vector<GridCandidate>& candidates,
    const std::vector<FbxTerrainGridPoint>& points,
    const std::vector<double>& pointHeights,
    const CDT::TriangleVec& triangles,
    const std::vector<CDT::V2d<double>>& vertices,
    uint32_t width,
    uint32_t height,
    double& maximumError)
{
    const std::vector<TriangleGeometry> geometry = BuildTriangleGeometry(
        triangles, vertices);
    const uint32_t binCountX = std::clamp(width / 32, 1u, 64u);
    const uint32_t binCountY = std::clamp(height / 32, 1u, 64u);
    std::vector<std::vector<std::size_t>> bins(
        static_cast<std::size_t>(binCountX) * binCountY);
    const auto binIndex = [=](double x, double y)
    {
        const uint32_t binX = std::min(
            binCountX - 1,
            static_cast<uint32_t>(std::clamp(
                x / std::max(1., static_cast<double>(width)) * binCountX,
                0., static_cast<double>(binCountX - 1))));
        const uint32_t binY = std::min(
            binCountY - 1,
            static_cast<uint32_t>(std::clamp(
                y / std::max(1., static_cast<double>(height)) * binCountY,
                0., static_cast<double>(binCountY - 1))));
        return static_cast<std::size_t>(binY) * binCountX + binX;
    };
    for (std::size_t index = 0; index < geometry.size(); ++index)
    {
        const TriangleGeometry& triangle = geometry[index];
        const uint32_t minBinX = static_cast<uint32_t>(binIndex(
            triangle.minX, triangle.minY) % binCountX);
        const uint32_t maxBinX = static_cast<uint32_t>(binIndex(
            triangle.maxX, triangle.minY) % binCountX);
        const uint32_t minBinY = static_cast<uint32_t>(binIndex(
            triangle.minX, triangle.minY) / binCountX);
        const uint32_t maxBinY = static_cast<uint32_t>(binIndex(
            triangle.minX, triangle.maxY) / binCountX);
        for (uint32_t y = minBinY; y <= maxBinY; ++y)
        {
            for (uint32_t x = minBinX; x <= maxBinX; ++x)
            {
                bins[static_cast<std::size_t>(y) * binCountX + x].push_back(index);
            }
        }
    }

    std::vector<CandidateError> errors;
    errors.reserve(candidates.size());
    maximumError = 0.;
    for (std::size_t candidateIndex = 0; candidateIndex < candidates.size();
         ++candidateIndex)
    {
        const GridCandidate& candidate = candidates[candidateIndex];
        const auto& possibleTriangles = bins[binIndex(
            candidate.point.x, candidate.point.y)];
        double largestError = 0.;
        for (std::size_t triangleIndex : possibleTriangles)
        {
            const TriangleGeometry& triangle = geometry[triangleIndex];
            double interpolatedHeight = 0.;
            if (TryInterpolateTriangleHeight(
                    triangle, vertices, pointHeights, candidate.point,
                    interpolatedHeight))
            {
                largestError = std::max(
                    largestError,
                    std::abs(candidate.height - interpolatedHeight));
            }
        }
        maximumError = std::max(maximumError, largestError);
        errors.push_back({candidateIndex, largestError});
    }
    std::ranges::sort(errors, [](const CandidateError& left, const CandidateError& right)
                      { return left.error > right.error; });
    return errors;
}

bool BuildCdt(
    const std::vector<FbxTerrainGridPoint>& points,
    const std::vector<FbxTerrainGridPoint>& boundaryPoints,
    CDT::Triangulation<double>& cdt)
{
    cdt.insertVertices(ToCdtVertices(points));
    std::unordered_map<uint64_t, uint32_t> pointIndices;
    pointIndices.reserve(points.size());
    for (uint32_t index = 0; index < points.size(); ++index)
    {
        pointIndices.emplace(GridPointKey(points[index]), index);
    }
    std::vector<CDT::Edge> edges;
    edges.reserve(boundaryPoints.size());
    for (std::size_t index = 0; index < boundaryPoints.size(); ++index)
    {
        const auto first = pointIndices.find(GridPointKey(boundaryPoints[index]));
        const auto second = pointIndices.find(GridPointKey(
            boundaryPoints[(index + 1) % boundaryPoints.size()]));
        if (first == pointIndices.end() || second == pointIndices.end() ||
            first->second == second->second)
        {
            return false;
        }
        edges.emplace_back(first->second, second->second);
    }
    if (edges.size() < 3)
    {
        return false;
    }
    cdt.insertEdges(edges);
    cdt.eraseOuterTrianglesAndHoles();
    return !cdt.triangles.empty();
}

} // namespace

namespace DownTool
{
FbxTerrainTinResult BuildFbxTerrainTin(
    const uint32_t gridWidth,
    const uint32_t gridHeight,
    const std::vector<double>& heights,
    const std::vector<FbxTerrainGridPoint>& boundaryPoints,
    const double geometricErrorMeters,
    const std::size_t maximumTriangleCount)
{
    FbxTerrainTinResult result;
    if (gridWidth < 1 || gridHeight < 1 ||
        heights.size() != static_cast<std::size_t>(gridWidth + 1) *
            (gridHeight + 1) || boundaryPoints.size() < 3 ||
        maximumTriangleCount == 0 || !std::isfinite(geometricErrorMeters) ||
        geometricErrorMeters < 0.)
    {
        return result;
    }

    std::vector<FbxTerrainGridPoint> points = boundaryPoints;
    std::set<uint64_t> selected;
    for (const FbxTerrainGridPoint& point : boundaryPoints)
    {
        if (point.x > gridWidth || point.y > gridHeight ||
            (!IsInteriorPoint(point, gridWidth, gridHeight) &&
             point.x != 0 && point.x != gridWidth &&
             point.y != 0 && point.y != gridHeight))
        {
            return result;
        }
        selected.insert(GridPointKey(point));
    }

    const std::vector<GridCandidate> candidates = BuildCandidates(
        gridWidth, gridHeight, heights, selected);
    std::vector<uint8_t> candidateSelected(candidates.size(), 0);
    std::vector<double> pointHeights;
    pointHeights.reserve(points.size());
    for (const FbxTerrainGridPoint& point : points)
    {
        pointHeights.push_back(HeightAt(heights, gridWidth, point));
    }

    double maximumError = std::numeric_limits<double>::infinity();
    while (points.size() >= 3 && maximumError > geometricErrorMeters)
    {
        CDT::Triangulation<double> cdt;
        if (!BuildCdt(points, boundaryPoints, cdt))
        {
            return {};
        }
        if (cdt.triangles.size() > maximumTriangleCount)
        {
            return {};
        }

        const std::vector<CandidateError> errors = CalculateCandidateErrors(
            candidates, points, pointHeights, cdt.triangles, cdt.vertices,
            gridWidth, gridHeight, maximumError);
        const std::size_t availableTriangleCount = maximumTriangleCount -
            cdt.triangles.size();
        const std::size_t maximumPendingCount = availableTriangleCount / 2;
        const std::size_t pendingLimit = std::min(
            maximumPendingCount, kMaximumRefinementBatchSize);
        std::vector<CandidateError> pending;
        pending.reserve(pendingLimit);
        for (const CandidateError& error : errors)
        {
            if (error.error <= geometricErrorMeters ||
                candidateSelected[error.candidateIndex] != 0)
            {
                continue;
            }
            pending.push_back(error);
            if (pending.size() >= pendingLimit)
            {
                break;
            }
        }
        if (pending.empty() || pendingLimit == 0)
        {
            result.triangles.reserve(cdt.triangles.size());
            for (const CDT::Triangle& triangle : cdt.triangles)
            {
                result.triangles.push_back({
                    triangle.vertices[0], triangle.vertices[1],
                    triangle.vertices[2]});
            }
            break;
        }
        for (const CandidateError& error : pending)
        {
            const GridCandidate& candidate = candidates[error.candidateIndex];
            points.push_back(candidate.point);
            pointHeights.push_back(candidate.height);
            candidateSelected[error.candidateIndex] = 1;
            selected.insert(GridPointKey(candidate.point));
        }
        result.triangles.clear();
    }

    if (result.triangles.empty())
    {
        CDT::Triangulation<double> cdt;
        if (!BuildCdt(points, boundaryPoints, cdt))
        {
            return {};
        }
        result.triangles.reserve(cdt.triangles.size());
        for (const CDT::Triangle& triangle : cdt.triangles)
        {
            result.triangles.push_back({
                triangle.vertices[0], triangle.vertices[1],
                triangle.vertices[2]});
        }
        maximumError = 0.;
        (void)CalculateCandidateErrors(
            candidates, points, pointHeights, cdt.triangles, cdt.vertices,
            gridWidth, gridHeight, maximumError);
    }

    result.points = std::move(points);
    result.maximumErrorMeters = std::isfinite(maximumError) ? maximumError : 0.;
    return result;
}
} // namespace DownTool
