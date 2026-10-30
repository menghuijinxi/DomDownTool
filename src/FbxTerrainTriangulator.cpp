// 渐进细化、局部换边及索引堆改编自 Michael Fogleman 的 hmm (MIT)。
// 上游版本与完整许可见 third_party/hmm/README.md 和 LICENSE.md。
#include "FbxTerrainTriangulator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CDT.h"

namespace
{
using DownTool::FbxTerrainGridPoint;
using DownTool::FbxTerrainTinResult;

uint64_t PointKey(const FbxTerrainGridPoint& point)
{
    return (static_cast<uint64_t>(point.x) << 32) | point.y;
}

// hmm 使用网格坐标中的顺时针三角形，此边函数在三角形内部为正。
int64_t Edge(FbxTerrainGridPoint a, FbxTerrainGridPoint b, FbxTerrainGridPoint c)
{
    return (int64_t(b.x) - c.x) * (int64_t(a.y) - c.y) -
        (int64_t(b.y) - c.y) * (int64_t(a.x) - c.x);
}

bool InCircle(FbxTerrainGridPoint a, FbxTerrainGridPoint b,
              FbxTerrainGridPoint c, FbxTerrainGridPoint p)
{
    const int64_t dx = int64_t(a.x) - p.x;
    const int64_t dy = int64_t(a.y) - p.y;
    const int64_t ex = int64_t(b.x) - p.x;
    const int64_t ey = int64_t(b.y) - p.y;
    const int64_t fx = int64_t(c.x) - p.x;
    const int64_t fy = int64_t(c.y) - p.y;
    const int64_t ap = dx * dx + dy * dy;
    const int64_t bp = ex * ex + ey * ey;
    const int64_t cp = fx * fx + fy * fy;
    return dx * (ey * cp - bp * fy) - dy * (ex * cp - bp * fx) +
        ap * (ex * fy - ey * fx) < 0;
}

class ProgressiveTerrain
{
public:
    ProgressiveTerrain(uint32_t width, uint32_t height, const std::vector<double>& heights,
                       const std::vector<std::array<double, 3>>& positions)
        : width_(width), height_(height), heights_(heights), positions_(positions)
    {
    }

    bool Initialize(const std::vector<FbxTerrainGridPoint>& boundary)
    {
        // 只在初始化时调用 CDT；之后逐点局部细化，不重建整个三角网。
        CDT::Triangulation<double> cdt;
        std::vector<CDT::V2d<double>> vertices;
        std::vector<CDT::Edge> edges;
        for (uint32_t i = 0; i < boundary.size(); ++i)
        {
            vertices.push_back(CDT::V2d<double>::make(boundary[i].x, boundary[i].y));
            edges.emplace_back(i, static_cast<uint32_t>((i + 1) % boundary.size()));
        }
        cdt.insertVertices(vertices);
        cdt.insertEdges(edges);
        cdt.eraseOuterTrianglesAndHoles();
        if (cdt.triangles.empty())
        {
            return false;
        }
        points_ = boundary;
        for (const auto& triangle : cdt.triangles)
        {
            AddTriangle(triangle.vertices[0], triangle.vertices[2], triangle.vertices[1],
                        -1, -1, -1, -1);
        }
        std::unordered_map<uint64_t, int> edgeIndices;
        for (int i = 0; i < static_cast<int>(triangles_.size()); ++i)
        {
            const int next = i - i % 3 + (i + 1) % 3;
            const uint32_t a = triangles_[i];
            const uint32_t b = triangles_[next];
            const auto found = edgeIndices.find((uint64_t(b) << 32) | a);
            if (found != edgeIndices.end())
            {
                halfedges_[i] = found->second;
                halfedges_[found->second] = i;
            }
            edgeIndices.emplace((uint64_t(a) << 32) | b, i);
        }
        Flush();
        return true;
    }

    FbxTerrainTinResult Run(double tolerance, uint32_t maximumTriangles)
    {
        if (maximumTriangles != 0 && triangles_.size() / 3 > maximumTriangles)
        {
            return {};
        }
        while (errors_[queue_.front()] > std::max(tolerance, 1e-9))
        {
            // 固定边界后每个内部插点增加两个面；预算不够则保留当前完整网格。
            if (maximumTriangles != 0 && triangles_.size() / 3 + 2 > maximumTriangles)
            {
                break;
            }
            Step();
        }
        FbxTerrainTinResult result;
        result.maximumErrorMeters = std::max(errors_[queue_.front()], BoundaryError());
        result.points = std::move(points_);
        result.triangles.reserve(triangles_.size() / 3);
        for (std::size_t i = 0; i < triangles_.size(); i += 3)
        {
            result.triangles.push_back({static_cast<uint32_t>(triangles_[i]),
                                       static_cast<uint32_t>(triangles_[i + 2]),
                                       static_cast<uint32_t>(triangles_[i + 1])});
        }
        return result;
    }

private:
    double Height(FbxTerrainGridPoint p) const
    {
        return heights_[static_cast<std::size_t>(p.y) * (width_ + 1) + p.x];
    }

    double PositionError(FbxTerrainGridPoint p, FbxTerrainGridPoint a, FbxTerrainGridPoint b,
                         FbxTerrainGridPoint c, const std::array<double, 3>& weights) const
    {
        if (positions_.empty())
        {
            return std::abs(Height(p) - Height(a) * weights[0] - Height(b) * weights[1] -
                            Height(c) * weights[2]);
        }
        const auto at = [&](FbxTerrainGridPoint point) -> const auto& {
            return positions_[static_cast<std::size_t>(point.y) * (width_ + 1) + point.x];
        };
        double squaredError = 0.;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const double difference = at(p)[axis] - at(a)[axis] * weights[0] -
                                      at(b)[axis] * weights[1] - at(c)[axis] * weights[2];
            squaredError += difference * difference;
        }
        return std::sqrt(squaredError);
    }

    std::pair<FbxTerrainGridPoint, double> FindCandidate(int triangle) const
    {
        const auto a = points_[triangles_[triangle * 3]];
        const auto b = points_[triangles_[triangle * 3 + 1]];
        const auto c = points_[triangles_[triangle * 3 + 2]];
        const int64_t area = Edge(a, b, c);
        if (area <= 0)
        {
            throw std::runtime_error("Terrain refinement produced a reversed or degenerate face");
        }
        const uint32_t minX = std::max(1u, std::min({a.x, b.x, c.x}));
        const uint32_t maxX = std::min(width_ - 1, std::max({a.x, b.x, c.x}));
        const uint32_t minY = std::max(1u, std::min({a.y, b.y, c.y}));
        const uint32_t maxY = std::min(height_ - 1, std::max({a.y, b.y, c.y}));
        const double za = Height(a) / area;
        const double zb = Height(b) / area;
        const double zc = Height(c) / area;
        FbxTerrainGridPoint candidate{};
        double maximum = 0.;
        for (uint32_t y = minY; y <= maxY; ++y)
        {
            const FbxTerrainGridPoint origin{minX, y};
            const std::array<int64_t, 3> weights{Edge(b, c, origin), Edge(c, a, origin),
                                                Edge(a, b, origin)};
            const std::array<int64_t, 3> steps{int64_t(c.y) - b.y,
                                              int64_t(a.y) - c.y,
                                              int64_t(b.y) - a.y};
            // 求本扫描线的整数区间，避免扫描细长三角形外部的大包围盒。
            int64_t start = 0;
            int64_t end = int64_t(maxX) - minX;
            for (std::size_t i = 0; i < weights.size(); ++i)
            {
                if (steps[i] > 0 && weights[i] < 0)
                {
                    start = std::max(start, (-weights[i] + steps[i] - 1) / steps[i]);
                }
                else if (steps[i] < 0)
                {
                    end = std::min(end, weights[i] < 0 ? int64_t(-1) :
                        weights[i] / -steps[i]);
                }
                else if (steps[i] == 0 && weights[i] < 0)
                {
                    end = -1;
                }
            }
            for (int64_t offset = start; offset <= end; ++offset)
            {
                const FbxTerrainGridPoint p{static_cast<uint32_t>(minX + offset), y};
                if (PointKey(p) == PointKey(a) || PointKey(p) == PointKey(b) ||
                    PointKey(p) == PointKey(c))
                {
                    continue;
                }
                const double z = za * (weights[0] + steps[0] * offset) +
                    zb * (weights[1] + steps[1] * offset) +
                    zc * (weights[2] + steps[2] * offset);
                const double error =
                    positions_.empty()
                        ? std::abs(z - Height(p))
                        : PositionError(p, a, b, c,
                                        {double(weights[0] + steps[0] * offset) / area,
                                         double(weights[1] + steps[1] * offset) / area,
                                         double(weights[2] + steps[2] * offset) / area});
                if (error > maximum)
                {
                    maximum = error;
                    candidate = p;
                }
            }
        }
        return {candidate, maximum};
    }

    double BoundaryError() const
    {
        double maximum = 0.;
        for (std::size_t e = 0; e < halfedges_.size(); ++e)
        {
            if (halfedges_[e] >= 0)
            {
                continue;
            }
            const auto a = points_[triangles_[e]];
            const auto b = points_[triangles_[e - e % 3 + (e + 1) % 3]];
            const uint32_t length = std::max(a.x, b.x) - std::min(a.x, b.x) +
                std::max(a.y, b.y) - std::min(a.y, b.y);
            for (uint32_t i = 1; i < length; ++i)
            {
                const double t = double(i) / length;
                const FbxTerrainGridPoint p{
                    static_cast<uint32_t>(int64_t(a.x) +
                        (int64_t(b.x) - a.x) * i / length),
                    static_cast<uint32_t>(int64_t(a.y) +
                        (int64_t(b.y) - a.y) * i / length)};
                maximum = std::max(maximum, PositionError(p, a, b, b, {1. - t, t, 0.}));
            }
        }
        return maximum;
    }

    void Flush()
    {
        for (int t : pending_)
        {
            const auto [candidate, error] = FindCandidate(t);
            candidates_[t] = candidate;
            errors_[t] = error;
            queueIndices_[t] = static_cast<int>(queue_.size());
            queue_.push_back(t);
            QueueUp(static_cast<int>(queue_.size()) - 1);
        }
        pending_.clear();
    }

    void SplitEdge(int pn, int a)
    {
        const int a0 = a - a % 3;
        const int al = a0 + (a + 1) % 3;
        const int ar = a0 + (a + 2) % 3;
        const int p0 = triangles_[ar];
        const int pr = triangles_[a];
        const int pl = triangles_[al];
        const int hal = halfedges_[al];
        const int har = halfedges_[ar];
        const int b = halfedges_[a];
        // 候选点只来自内部；不可改变相邻地块共同确定的外边界。
        if (b < 0)
        {
            throw std::runtime_error("Terrain refinement attempted to split a fixed boundary");
        }
        const int b0 = b - b % 3;
        const int bl = b0 + (b + 2) % 3;
        const int br = b0 + (b + 1) % 3;
        const int p1 = triangles_[bl];
        const int hbl = halfedges_[bl];
        const int hbr = halfedges_[br];
        QueueRemove(b / 3);
        const int t0 = AddTriangle(p0, pr, pn, har, -1, -1, a0);
        const int t1 = AddTriangle(pr, p1, pn, hbr, -1, t0 + 1, b0);
        const int t2 = AddTriangle(p1, pl, pn, hbl, -1, t1 + 1, -1);
        const int t3 = AddTriangle(pl, p0, pn, hal, t0 + 2, t2 + 1, -1);
        Legalize(t0);
        Legalize(t1);
        Legalize(t2);
        Legalize(t3);
    }

    void Step()
    {
        const int t = queue_.front();
        QueueRemove(t);
        const int e0 = t * 3;
        const int p0 = triangles_[e0];
        const int p1 = triangles_[e0 + 1];
        const int p2 = triangles_[e0 + 2];
        const auto a = points_[p0];
        const auto b = points_[p1];
        const auto c = points_[p2];
        const auto p = candidates_[t];
        const int pn = static_cast<int>(points_.size());
        points_.push_back(p);
        if (Edge(a, b, p) == 0)
        {
            SplitEdge(pn, e0);
        }
        else if (Edge(b, c, p) == 0)
        {
            SplitEdge(pn, e0 + 1);
        }
        else if (Edge(c, a, p) == 0)
        {
            SplitEdge(pn, e0 + 2);
        }
        else
        {
            const int h0 = halfedges_[e0];
            const int h1 = halfedges_[e0 + 1];
            const int h2 = halfedges_[e0 + 2];
            const int t0 = AddTriangle(p0, p1, pn, h0, -1, -1, e0);
            const int t1 = AddTriangle(p1, p2, pn, h1, -1, t0 + 1, -1);
            const int t2 = AddTriangle(p2, p0, pn, h2, t0 + 2, t1 + 1, -1);
            Legalize(t0);
            Legalize(t1);
            Legalize(t2);
        }
        Flush();
    }

    int AddTriangle(int a, int b, int c, int ab, int bc, int ca, int e)
    {
        if (e < 0)
        {
            e = static_cast<int>(triangles_.size());
            triangles_.insert(triangles_.end(), {a, b, c});
            halfedges_.insert(halfedges_.end(), {ab, bc, ca});
            candidates_.push_back({});
            errors_.push_back(0.);
            queueIndices_.push_back(-1);
        }
        else
        {
            triangles_[e] = a;
            triangles_[e + 1] = b;
            triangles_[e + 2] = c;
            halfedges_[e] = ab;
            halfedges_[e + 1] = bc;
            halfedges_[e + 2] = ca;
        }
        if (ab >= 0)
        {
            halfedges_[ab] = e;
        }
        if (bc >= 0)
        {
            halfedges_[bc] = e + 1;
        }
        if (ca >= 0)
        {
            halfedges_[ca] = e + 2;
        }
        pending_.push_back(e / 3);
        return e;
    }

    void Legalize(int firstEdge)
    {
        // 显式栈代替上游递归，避免长换边链耗尽线程栈。
        std::vector<int> pending{firstEdge};
        while (!pending.empty())
        {
            const int a = pending.back();
            pending.pop_back();
            const int b = halfedges_[a];
            if (b < 0)
            {
                continue;
            }
            const int a0 = a - a % 3;
            const int b0 = b - b % 3;
            const int al = a0 + (a + 1) % 3;
            const int ar = a0 + (a + 2) % 3;
            const int bl = b0 + (b + 2) % 3;
            const int br = b0 + (b + 1) % 3;
            const int p0 = triangles_[ar];
            const int pr = triangles_[a];
            const int pl = triangles_[al];
            const int p1 = triangles_[bl];
            if (!InCircle(points_[p0], points_[pr], points_[pl], points_[p1]))
            {
                continue;
            }
            const int hal = halfedges_[al];
            const int har = halfedges_[ar];
            const int hbl = halfedges_[bl];
            const int hbr = halfedges_[br];
            QueueRemove(a / 3);
            QueueRemove(b / 3);
            const int t0 = AddTriangle(p0, p1, pl, -1, hbl, hal, a0);
            const int t1 = AddTriangle(p1, p0, pr, t0, har, hbr, b0);
            pending.push_back(t1 + 2);
            pending.push_back(t0 + 1);
        }
    }

    bool QueueLess(int i, int j) const
    {
        return errors_[queue_[i]] > errors_[queue_[j]];
    }

    void QueueSwap(int i, int j)
    {
        std::swap(queue_[i], queue_[j]);
        queueIndices_[queue_[i]] = i;
        queueIndices_[queue_[j]] = j;
    }

    void QueueUp(int j)
    {
        while (j > 0)
        {
            const int i = (j - 1) / 2;
            if (!QueueLess(j, i))
            {
                break;
            }
            QueueSwap(i, j);
            j = i;
        }
    }

    bool QueueDown(int i, int n)
    {
        const int initial = i;
        while (2 * i + 1 < n)
        {
            int j = 2 * i + 1;
            if (j + 1 < n && QueueLess(j + 1, j))
            {
                ++j;
            }
            if (!QueueLess(j, i))
            {
                break;
            }
            QueueSwap(i, j);
            i = j;
        }
        return i > initial;
    }

    void QueueRemove(int t)
    {
        const int i = queueIndices_[t];
        if (i < 0)
        {
            const auto found = std::find(pending_.begin(), pending_.end(), t);
            if (found == pending_.end())
            {
                throw std::runtime_error("Terrain refinement queue is inconsistent");
            }
            std::iter_swap(found, pending_.end() - 1);
            pending_.pop_back();
            return;
        }
        const int n = static_cast<int>(queue_.size()) - 1;
        if (i != n)
        {
            QueueSwap(i, n);
            if (!QueueDown(i, n))
            {
                QueueUp(i);
            }
        }
        queue_.pop_back();
        queueIndices_[t] = -1;
    }

    uint32_t width_;
    uint32_t height_;
    const std::vector<double>& heights_;
    const std::vector<std::array<double, 3>>& positions_;
    std::vector<FbxTerrainGridPoint> points_;
    std::vector<int> triangles_;
    std::vector<int> halfedges_;
    std::vector<FbxTerrainGridPoint> candidates_;
    std::vector<double> errors_;
    std::vector<int> queueIndices_;
    std::vector<int> queue_;
    std::vector<int> pending_;
};

bool ValidBoundary(uint32_t width, uint32_t height,
                   const std::vector<FbxTerrainGridPoint>& boundary)
{
    // 仅接受沿矩形周界顺序排列的唯一点，这是共享边约束的前置条件。
    std::set<uint64_t> keys;
    int64_t twiceArea = 0;
    uint64_t length = 0;
    for (std::size_t i = 0; i < boundary.size(); ++i)
    {
        const auto p = boundary[i];
        const auto q = boundary[(i + 1) % boundary.size()];
        if (p.x > width || p.y > height ||
            (p.x != 0 && p.x != width && p.y != 0 && p.y != height) ||
            !keys.insert(PointKey(p)).second ||
            !((p.x == q.x && (p.x == 0 || p.x == width)) ||
              (p.y == q.y && (p.y == 0 || p.y == height))))
        {
            return false;
        }
        twiceArea += int64_t(p.x) * q.y - int64_t(p.y) * q.x;
        length += std::max(p.x, q.x) - std::min(p.x, q.x) +
            std::max(p.y, q.y) - std::min(p.y, q.y);
    }
    return std::abs(twiceArea) == int64_t(width) * height * 2 &&
        length == 2 * (uint64_t(width) + height);
}
} // namespace

namespace DownTool
{
FbxTerrainTinResult BuildFbxTerrainTin(uint32_t gridWidth, uint32_t gridHeight,
                                       const std::vector<double>& heights,
                                       const std::vector<FbxTerrainGridPoint>& boundaryPoints,
                                       double geometricErrorMeters, uint32_t maximumTriangles,
                                       const std::vector<std::array<double, 3>>& positionsMeters)
{
    // 整数内圆谓词的四次乘积在该范围内不会溢出；正式导出最多使用 1024 段。
    constexpr uint32_t maximumGridExtent = 4096;
    if (gridWidth < 1 || gridHeight < 1 || gridWidth > maximumGridExtent ||
        gridHeight > maximumGridExtent ||
        heights.size() != (std::size_t(gridWidth) + 1) * (gridHeight + 1) ||
        boundaryPoints.size() < 4 || !std::isfinite(geometricErrorMeters) ||
        geometricErrorMeters < 0. ||
        (!positionsMeters.empty() && (positionsMeters.size() != heights.size() ||
                                      !std::ranges::all_of(
                                          positionsMeters,
                                          [](const auto& position) {
                                              return std::ranges::all_of(position, [](double v) {
                                                  return std::isfinite(v);
                                              });
                                          }))) ||
        (maximumTriangles != 0 && maximumTriangles < 2) ||
        !std::ranges::all_of(
            heights, [](double z) { return std::isfinite(z); }) ||
        !ValidBoundary(gridWidth, gridHeight, boundaryPoints))
    {
        return {};
    }
    ProgressiveTerrain terrain(gridWidth, gridHeight, heights, positionsMeters);
    if (!terrain.Initialize(boundaryPoints))
    {
        return {};
    }
    return terrain.Run(geometricErrorMeters, maximumTriangles);
}
} // namespace DownTool
