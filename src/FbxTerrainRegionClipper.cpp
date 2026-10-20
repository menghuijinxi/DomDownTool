#include "FbxTerrainRegionClipper.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <numbers>
#include <utility>

#include "mapbox/earcut.hpp"
#include "polyclipping/clipper.hpp"

#include "GisSpatialIndex.h"

namespace
{
using namespace ClipperLib;
using DownTool::FbxTerrainClipPoint;
using DownTool::FbxTerrainClipTriangle;

// 固定全局精度使相邻三角面和地块生成完全相同的裁切交点，单位为 10 微米。
constexpr double kClipScale = 100000.;
constexpr double kEarthRadius = 6378137.;

double MercatorX(double longitude)
{
    return kEarthRadius * longitude * std::numbers::pi / 180.;
}

double MercatorY(double latitude)
{
    return kEarthRadius *
           std::log(std::tan(std::numbers::pi / 4. + latitude * std::numbers::pi / 360.));
}

IntPoint Quantize(const FbxTerrainClipPoint& point)
{
    return {static_cast<cInt>(std::llround(point.GetX() * kClipScale)),
            static_cast<cInt>(std::llround(point.GetY() * kClipScale))};
}

DownTool::GisSpatialBounds PathBounds(const Path& path)
{
    DownTool::GisSpatialBounds bounds;
    for (const auto& point : path)
    {
        bounds.Add(static_cast<double>(point.X), static_cast<double>(point.Y));
    }
    return bounds;
}

bool ToPaths(const DownTool::Wgs84Polygons& polygons, Paths& paths, std::string& error)
{
    if (polygons.empty())
    {
        return true;
    }
    if (!DownTool::ValidateWgs84Polygons(polygons, error))
    {
        return false;
    }
    for (const auto& ring : polygons)
    {
        if (!DownTool::IsSimpleWgs84Ring(ring))
        {
            error = "底板范围边界不能自交。";
            return false;
        }
        Path path;
        for (const auto& point : ring)
        {
            if (std::abs(point.lat) >= 85.05112878)
            {
                error = "底板范围超出 Web Mercator 支持的纬度。";
                return false;
            }
            const auto quantized = Quantize({MercatorX(point.lon), MercatorY(point.lat)});
            if (path.empty() || path.back() != quantized)
            {
                path.push_back(quantized);
            }
        }
        if (path.size() > 1 && path.front() == path.back())
        {
            path.pop_back();
        }
        if (path.size() < 3 || Area(path) == 0.)
        {
            error = "底板范围在裁切精度内没有有效面积。";
            return false;
        }
        if (!Orientation(path))
        {
            ReversePath(path);
        }
        paths.push_back(std::move(path));
    }
    return true;
}

bool UnionPaths(const Paths& input, Paths& output)
{
    if (input.empty())
    {
        return true;
    }
    Clipper clipper;
    clipper.AddPaths(input, ptSubject, true);
    return clipper.Execute(ctUnion, output, pftNonZero, pftNonZero);
}

double SignedArea(const FbxTerrainClipTriangle& triangle)
{
    return (triangle[1].GetX() - triangle[0].GetX()) * (triangle[2].GetY() - triangle[0].GetY()) -
           (triangle[1].GetY() - triangle[0].GetY()) * (triangle[2].GetX() - triangle[0].GetX());
}

void TriangulateNode(const PolyNode& node, double orientation,
                     std::vector<FbxTerrainClipTriangle>& output)
{
    using Ring = std::vector<std::array<double, 2>>;
    std::vector<Ring> polygon;
    std::vector<FbxTerrainClipPoint> points;
    const auto origin = node.Contour.front();
    const auto appendRing = [&](const Path& path) {
        Ring ring;
        for (const auto& point : path)
        {
            ring.push_back({static_cast<double>(point.X - origin.X) / kClipScale,
                            static_cast<double>(point.Y - origin.Y) / kClipScale});
            points.emplace_back(static_cast<double>(point.X) / kClipScale,
                                static_cast<double>(point.Y) / kClipScale);
        }
        polygon.push_back(std::move(ring));
    };
    appendRing(node.Contour);
    for (const auto* child : node.Childs)
    {
        if (child->IsHole())
        {
            appendRing(child->Contour);
        }
    }
    const auto indices = mapbox::earcut<uint32_t>(polygon);
    for (size_t index = 0; index < indices.size(); index += 3)
    {
        FbxTerrainClipTriangle triangle = {points[indices[index]], points[indices[index + 1]],
                                           points[indices[index + 2]]};
        const double area = SignedArea(triangle);
        if (area == 0.)
        {
            continue;
        }
        if ((area > 0.) != (orientation > 0.))
        {
            std::swap(triangle[1], triangle[2]);
        }
        output.push_back(triangle);
    }
}
} // namespace

class DownTool::FbxTerrainRegionClipper::Impl
{
  public:
    Paths mask;
    bool active = false;
    bool intersection = false;
    GisSpatialIndex polygonIndex;
    GisSpatialIndex boundaryIndex;
    std::function<bool()> cancellationRequested;

    bool CheckCancellation(std::string& error) const
    {
        if (cancellationRequested && cancellationRequested())
        {
            error = "底板范围裁切已取消。";
            return false;
        }
        return true;
    }

    bool Execute(const Path& subject, PolyTree& output, std::string& error) const
    {
        if (!CheckCancellation(error))
        {
            return false;
        }
        std::vector<size_t> candidates;
        if (!polygonIndex.Query(PathBounds(subject), candidates, cancellationRequested))
        {
            error = "底板范围查询已取消。";
            return false;
        }
        Clipper clipper;
        clipper.AddPath(subject, ptSubject, true);
        for (const auto index : candidates)
        {
            clipper.AddPath(mask[index], ptClip, true);
        }
        if (!clipper.Execute(intersection ? ctIntersection : ctDifference, output, pftNonZero,
                             pftNonZero))
        {
            error = "底板范围布尔运算失败。";
            return false;
        }
        return CheckCancellation(error);
    }

    bool Contains(const IntPoint& point, bool& contained, std::string& error) const
    {
        std::vector<size_t> candidates;
        if (!polygonIndex.Query({static_cast<double>(point.X), static_cast<double>(point.Y),
                                 static_cast<double>(point.X), static_cast<double>(point.Y)},
                                candidates, cancellationRequested))
        {
            error = "底板范围查询已取消。";
            return false;
        }
        int winding = 0;
        for (const auto index : candidates)
        {
            const int result = PointInPolygon(point, mask[index]);
            if (result == -1)
            {
                contained = true;
                return true;
            }
            if (result == 1)
            {
                winding += Orientation(mask[index]) ? 1 : -1;
            }
        }
        contained = winding != 0;
        return true;
    }
};

DownTool::FbxTerrainRegionClipper::FbxTerrainRegionClipper() : impl_(std::make_unique<Impl>())
{
}

DownTool::FbxTerrainRegionClipper::~FbxTerrainRegionClipper() = default;

bool DownTool::FbxTerrainRegionClipper::Initialize(
    const Wgs84Polygons& included, const Wgs84Polygons& excluded, std::string& error,
    const std::function<bool()>& cancellationRequested)
{
    auto next = std::make_unique<Impl>();
    next->cancellationRequested = cancellationRequested;
    next->active = !included.empty() || !excluded.empty();
    next->intersection = !included.empty();
    try
    {
        Paths includePaths;
        Paths excludePaths;
        Paths unionIncluded;
        Paths unionExcluded;
        if (!next->CheckCancellation(error) || !ToPaths(included, includePaths, error) ||
            !ToPaths(excluded, excludePaths, error))
        {
            return false;
        }
        if (!UnionPaths(includePaths, unionIncluded) || !UnionPaths(excludePaths, unionExcluded))
        {
            error = "底板范围合并失败。";
            return false;
        }
        if (included.empty())
        {
            next->mask = std::move(unionExcluded);
        }
        else if (unionExcluded.empty())
        {
            next->mask = std::move(unionIncluded);
        }
        else
        {
            Clipper clipper;
            clipper.AddPaths(unionIncluded, ptSubject, true);
            clipper.AddPaths(unionExcluded, ptClip, true);
            if (!clipper.Execute(ctDifference, next->mask, pftNonZero, pftNonZero))
            {
                error = "底板排除区域计算失败。";
                return false;
            }
        }
        std::vector<GisSpatialBounds> polygons;
        std::vector<GisSpatialBounds> edges;
        for (const auto& path : next->mask)
        {
            polygons.push_back(PathBounds(path));
            for (size_t index = 0; index < path.size(); ++index)
            {
                const auto& first = path[index];
                const auto& second = path[(index + 1) % path.size()];
                GisSpatialBounds edge;
                edge.Add(static_cast<double>(first.X), static_cast<double>(first.Y));
                edge.Add(static_cast<double>(second.X), static_cast<double>(second.Y));
                edges.push_back(edge);
            }
        }
        if (!next->polygonIndex.Build(std::move(polygons), cancellationRequested) ||
            !next->boundaryIndex.Build(std::move(edges), cancellationRequested) ||
            !next->CheckCancellation(error))
        {
            error = "底板范围索引已取消。";
            return false;
        }
        impl_ = std::move(next);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "底板范围初始化失败：" + std::string(exception.what());
        return false;
    }
}

bool DownTool::FbxTerrainRegionClipper::IsActive() const
{
    return impl_->active;
}

bool DownTool::FbxTerrainRegionClipper::HasVisibleArea(const GeographicBounds& bounds,
                                                       bool& visible, std::string& error) const
{
    visible = false;
    if (!impl_->CheckCancellation(error))
    {
        return false;
    }
    if (!impl_->active)
    {
        visible = true;
        return true;
    }
    const Path rectangle = {Quantize({MercatorX(bounds.west), MercatorY(bounds.south)}),
                            Quantize({MercatorX(bounds.east), MercatorY(bounds.south)}),
                            Quantize({MercatorX(bounds.east), MercatorY(bounds.north)}),
                            Quantize({MercatorX(bounds.west), MercatorY(bounds.north)})};
    PolyTree result;
    if (!impl_->Execute(rectangle, result, error))
    {
        return false;
    }
    visible = !result.Childs.empty();
    return true;
}

bool DownTool::FbxTerrainRegionClipper::ClipTriangle(const FbxTerrainClipTriangle& triangle,
                                                     std::vector<FbxTerrainClipTriangle>& output,
                                                     std::string& error) const
{
    output.clear();
    if (!impl_->CheckCancellation(error))
    {
        return false;
    }
    for (const auto& point : triangle)
    {
        if (!std::isfinite(point.GetX()) || !std::isfinite(point.GetY()) ||
            std::abs(point.GetX()) > 3.e7 || std::abs(point.GetY()) > 3.e7)
        {
            error = "底板三角面包含无效坐标。";
            return false;
        }
    }
    const double orientation = SignedArea(triangle);
    if (orientation == 0.)
    {
        return true;
    }
    if (!impl_->active)
    {
        output.push_back(triangle);
        return true;
    }
    const Path subject = {Quantize(triangle[0]), Quantize(triangle[1]), Quantize(triangle[2])};
    std::vector<size_t> edges;
    if (!impl_->boundaryIndex.Query(PathBounds(subject), edges, impl_->cancellationRequested))
    {
        error = "底板边界查询已取消。";
        return false;
    }
    if (edges.empty())
    {
        const FbxTerrainClipPoint center(
            triangle[0].GetX() + (triangle[1].GetX() - triangle[0].GetX()) / 3. +
                (triangle[2].GetX() - triangle[0].GetX()) / 3.,
            triangle[0].GetY() + (triangle[1].GetY() - triangle[0].GetY()) / 3. +
                (triangle[2].GetY() - triangle[0].GetY()) / 3.);
        bool contained = false;
        if (!impl_->Contains(Quantize(center), contained, error))
        {
            return false;
        }
        if (contained == impl_->intersection)
        {
            output.push_back(triangle);
        }
        return true;
    }
    try
    {
        PolyTree result;
        if (!impl_->Execute(subject, result, error))
        {
            return false;
        }
        for (auto* node = result.GetFirst(); node; node = node->GetNext())
        {
            if (!node->IsHole() && node->Contour.size() >= 3)
            {
                TriangulateNode(*node, orientation, output);
                if (!impl_->CheckCancellation(error))
                {
                    output.clear();
                    return false;
                }
            }
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        output.clear();
        error = "底板三角面裁切失败：" + std::string(exception.what());
        return false;
    }
}
