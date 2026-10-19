#include "GisSpatialFilter.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

#include "polyclipping/clipper.hpp"

#include "AmapVectorPngDownloader.h"
#include "GCJ02GoogleDownBase.h"

namespace
{
using namespace ClipperLib;
using DownTool::AmapVectorCoordinate;
using DownTool::AmapVectorFeature;

// 经纬度量化至约毫米，避免使用浮点布尔运算时丢失小范围孔洞。
constexpr double kCoordinateScale = 1.e8;

IntPoint ToPoint(double longitude, double latitude)
{
    return {static_cast<cInt>(std::llround(longitude * kCoordinateScale)),
            static_cast<cInt>(std::llround(latitude * kCoordinateScale))};
}

Paths ToClipPaths(const DownTool::Wgs84Polygons& polygons)
{
    Paths paths;
    for (const auto& ring : polygons)
    {
        Path path;
        for (const auto& coordinate : ring)
        {
            const auto gcj = DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(coordinate);
            path.push_back(ToPoint(gcj.lon, gcj.lat));
        }
        if (!Orientation(path))
        {
            std::reverse(path.begin(), path.end());
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

Paths ToFeaturePaths(const AmapVectorFeature& feature)
{
    Paths paths;
    for (const auto& part : feature.geometryParts)
    {
        Path path;
        for (const auto& coordinate : part)
        {
            path.push_back(ToPoint(coordinate.longitude, coordinate.latitude));
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

Paths ClipClosed(const Paths& subject, const Paths& clip, ClipType operation)
{
    Clipper clipper;
    clipper.StrictlySimple(true);
    if (!clipper.AddPaths(subject, ptSubject, true))
    {
        return {};
    }
    clipper.AddPaths(clip, ptClip, true);
    Paths result;
    if (!clipper.Execute(operation, result, pftEvenOdd, pftNonZero))
    {
        throw std::runtime_error("GIS 面范围裁切失败。");
    }
    return result;
}

bool Contains(const Paths& polygons, const IntPoint& point)
{
    int winding = 0;
    for (const Path& path : polygons)
    {
        const int containment = PointInPolygon(point, path);
        if (containment == -1)
        {
            return true;
        }
        if (containment == 1)
        {
            winding += Orientation(path) ? 1 : -1;
        }
    }
    return winding != 0;
}

bool TouchesBoundary(const Paths& first, const Paths& second)
{
    for (const Path& left : first)
    {
        for (const Path& right : second)
        {
            for (const auto& point : left)
            {
                if (PointInPolygon(point, right) == -1)
                {
                    return true;
                }
            }
            for (const auto& point : right)
            {
                if (PointInPolygon(point, left) == -1)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<AmapVectorCoordinate> ToCoordinates(const Path& path, bool closed)
{
    std::vector<AmapVectorCoordinate> coordinates;
    for (const auto& point : path)
    {
        coordinates.push_back({point.X / kCoordinateScale, point.Y / kCoordinateScale});
    }
    if (closed && !coordinates.empty())
    {
        coordinates.push_back(coordinates.front());
    }
    return coordinates;
}

void AppendPolygons(const PolyNode& node, const AmapVectorFeature& source,
                    std::vector<AmapVectorFeature>& output)
{
    for (const PolyNode* child : node.Childs)
    {
        if (!child->IsHole() && !child->IsOpen() && child->Contour.size() >= 3)
        {
            AmapVectorFeature clipped = source;
            clipped.geometryParts.clear();
            clipped.geometryParts.push_back(ToCoordinates(child->Contour, true));
            for (const PolyNode* hole : child->Childs)
            {
                if (hole->IsHole())
                {
                    clipped.geometryParts.push_back(ToCoordinates(hole->Contour, true));
                }
            }
            output.push_back(std::move(clipped));
        }
        AppendPolygons(*child, source, output);
    }
}

void AppendClippedFeature(const AmapVectorFeature& feature, const Paths& mask,
                          std::vector<AmapVectorFeature>& output)
{
    const bool closed = feature.geometryType == DownTool::AmapVectorGeometryType::Polygon;
    Clipper clipper;
    clipper.StrictlySimple(true);
    if (!clipper.AddPaths(ToFeaturePaths(feature), ptSubject, closed))
    {
        return;
    }
    clipper.AddPaths(mask, ptClip, true);
    PolyTree result;
    if (!clipper.Execute(ctIntersection, result, pftEvenOdd, pftNonZero))
    {
        throw std::runtime_error("GIS 范围裁切失败。");
    }
    if (closed)
    {
        AppendPolygons(result, feature, output);
    }
    else
    {
        Paths lines;
        OpenPathsFromPolyTree(result, lines);
        for (const Path& line : lines)
        {
            if (line.size() >= 2)
            {
                AmapVectorFeature clipped = feature;
                clipped.geometryParts = {ToCoordinates(line, false)};
                output.push_back(std::move(clipped));
            }
        }
    }
}
} // namespace

bool DownTool::ApplyGisSpatialFilter(AmapVectorDecodedData& decodedData,
                                     const GisSpatialRange& range, std::string& error,
                                     const std::function<bool()>& cancellationRequested)
{
    if (range.IsEmpty())
    {
        return true;
    }
    if ((!range.GetIncludedPolygons().empty() &&
         !ValidateWgs84Polygons(range.GetIncludedPolygons(), error)) ||
        (!range.GetExcludedPolygons().empty() &&
         !ValidateWgs84Polygons(range.GetExcludedPolygons(), error)))
    {
        return false;
    }
    try
    {
        Paths included = ToClipPaths(range.GetIncludedPolygons());
        const Paths excluded = ToClipPaths(range.GetExcludedPolygons());
        if (included.empty())
        {
            included = {{{-18000000000LL, -9000000000LL},
                         {18000000000LL, -9000000000LL},
                         {18000000000LL, 9000000000LL},
                         {-18000000000LL, 9000000000LL}}};
        }
        Clipper unionClipper;
        unionClipper.AddPaths(included, ptSubject, true);
        Paths unionIncluded;
        if (!unionClipper.Execute(ctUnion, unionIncluded, pftNonZero, pftNonZero))
        {
            error = "GIS 下载区合并失败。";
            return false;
        }
        const Paths mask =
            excluded.empty() ? unionIncluded : ClipClosed(unionIncluded, excluded, ctDifference);
        std::vector<AmapVectorFeature> selected;
        for (const AmapVectorFeature& feature : decodedData.features)
        {
            if (cancellationRequested && cancellationRequested())
            {
                error = "GIS 范围处理已取消。";
                return false;
            }
            if (mask.empty() || feature.geometryParts.empty())
            {
                continue;
            }
            if (feature.geometryType == AmapVectorGeometryType::Point)
            {
                for (const auto& part : feature.geometryParts)
                {
                    for (const auto& coordinate : part)
                    {
                        const auto pointCoordinate =
                            ToPoint(coordinate.longitude, coordinate.latitude);
                        if (Contains(unionIncluded, pointCoordinate) &&
                            !Contains(excluded, pointCoordinate))
                        {
                            AmapVectorFeature point = feature;
                            point.geometryParts = {{coordinate}};
                            selected.push_back(std::move(point));
                        }
                    }
                }
            }
            else if (ClassifyAmapVectorFeature(feature) == AmapVectorRenderCategory::Building)
            {
                const Paths footprint = ToFeaturePaths(feature);
                if (!ClipClosed(footprint, unionIncluded, ctIntersection).empty() &&
                    (excluded.empty() || (ClipClosed(footprint, excluded, ctIntersection).empty() &&
                                          !TouchesBoundary(footprint, excluded))))
                {
                    selected.push_back(feature);
                }
            }
            else
            {
                AppendClippedFeature(feature, mask, selected);
            }
        }
        decodedData.features = std::move(selected);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "GIS 范围处理失败：" + std::string(exception.what());
        return false;
    }
}
