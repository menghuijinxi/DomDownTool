#include "GisSpatialFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

#include "polyclipping/clipper.hpp"

#include "AmapVectorPngDownloader.h"
#include "GCJ02GoogleDownBase.h"
#include "GisSpatialIndex.h"

namespace
{
using namespace ClipperLib;
using DownTool::AmapVectorCoordinate;
using DownTool::AmapVectorFeature;
using DownTool::GisProcessingStage;
using DownTool::GisSpatialBounds;
using DownTool::GisSpatialIndex;

constexpr double kCoordinateScale = 1.e8;
constexpr uint8_t kIncludedCandidate = 1;
constexpr uint8_t kIncludedBoundary = 2;
constexpr uint8_t kExcludedCandidate = 4;
constexpr uint8_t kExcludedBoundary = 8;

class FilterCanceled : public std::exception
{
};

class ProcessingContext
{
  public:
    ProcessingContext(const std::function<bool()>& cancellation,
                      const DownTool::GisProcessingProgressCallback& progress)
        : cancellation_(cancellation), progress_(progress)
    {
    }

    void Check() const
    {
        if (cancellation_ && cancellation_())
        {
            throw FilterCanceled{};
        }
    }

    void Require(bool completed) const
    {
        if (!completed)
        {
            throw FilterCanceled{};
        }
    }

    void Report(GisProcessingStage stage, size_t completed, size_t total) const
    {
        if (progress_ && (completed % 256 == 0 || completed == total))
        {
            progress_({stage, completed, total});
        }
        Check();
    }

    const std::function<bool()>& GetCancellation() const { return cancellation_; }

  private:
    const std::function<bool()>& cancellation_;
    const DownTool::GisProcessingProgressCallback& progress_;
};

IntPoint toPoint(double longitude, double latitude)
{
    return {static_cast<cInt>(std::llround(longitude * kCoordinateScale)),
            static_cast<cInt>(std::llround(latitude * kCoordinateScale))};
}

Paths toClipPaths(const DownTool::Wgs84Polygons& polygons, const ProcessingContext& context)
{
    Paths paths;
    for (const auto& ring : polygons)
    {
        Path path;
        path.reserve(ring.size());
        for (const auto& coordinate : ring)
        {
            if (path.size() % 256 == 0)
            {
                context.Check();
            }
            const auto gcj = DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(coordinate);
            path.push_back(toPoint(gcj.lon, gcj.lat));
        }
        if (!Orientation(path))
        {
            std::reverse(path.begin(), path.end());
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

Paths toFeaturePaths(const AmapVectorFeature& feature, const ProcessingContext& context)
{
    Paths paths;
    for (const auto& part : feature.geometryParts)
    {
        Path path;
        path.reserve(part.size());
        for (const auto& coordinate : part)
        {
            if (path.size() % 256 == 0)
            {
                context.Check();
            }
            path.push_back(toPoint(coordinate.longitude, coordinate.latitude));
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

GisSpatialBounds pathBounds(const Path& path)
{
    GisSpatialBounds bounds;
    for (const auto& point : path)
    {
        bounds.Add(static_cast<double>(point.X), static_cast<double>(point.Y));
    }
    return bounds;
}

GisSpatialBounds featureBounds(const AmapVectorFeature& feature, const ProcessingContext& context)
{
    GisSpatialBounds bounds;
    size_t visited = 0;
    for (const auto& part : feature.geometryParts)
    {
        for (const auto& coordinate : part)
        {
            if (visited++ % 256 == 0)
            {
                context.Check();
            }
            const auto point = toPoint(coordinate.longitude, coordinate.latitude);
            bounds.Add(static_cast<double>(point.X), static_cast<double>(point.Y));
        }
    }
    return bounds;
}

Paths clipClosed(const Paths& subject, const Paths& clip, ClipType operation,
                 const ProcessingContext& context)
{
    context.Check();
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
    context.Check();
    return result;
}

bool contains(const Paths& polygons, const IntPoint& point)
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

bool touchesBoundary(const Paths& first, const Paths& second, const ProcessingContext& context)
{
    for (const Path& left : first)
    {
        for (const Path& right : second)
        {
            context.Check();
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

bool hasSimpleBuildingFootprint(const AmapVectorFeature& feature)
{
    if (feature.geometryType != DownTool::AmapVectorGeometryType::Polygon ||
        feature.geometryParts.size() != 1 || feature.geometryParts.front().size() > 16)
    {
        return false;
    }
    DownTool::Wgs84Ring quantized;
    for (const auto& coordinate : feature.geometryParts.front())
    {
        const auto point = toPoint(coordinate.longitude, coordinate.latitude);
        quantized.emplace_back(static_cast<double>(point.X), static_cast<double>(point.Y));
    }
    // 只让已确认简单的小轮廓走整栋保留路径，退化环和复杂环仍由 Clipper 验证。
    return DownTool::IsSimpleWgs84Ring(quantized);
}

std::vector<AmapVectorCoordinate> toCoordinates(const Path& path, bool closed,
                                                const ProcessingContext& context)
{
    std::vector<AmapVectorCoordinate> coordinates;
    coordinates.reserve(path.size() + (closed ? 1 : 0));
    for (const auto& point : path)
    {
        if (coordinates.size() % 256 == 0)
        {
            context.Check();
        }
        coordinates.push_back({point.X / kCoordinateScale, point.Y / kCoordinateScale});
    }
    if (closed && !coordinates.empty())
    {
        coordinates.push_back(coordinates.front());
    }
    return coordinates;
}

std::vector<AmapVectorCoordinate> polygonRingCoordinates(const Path& path, bool isHole,
                                                         const ProcessingContext& context)
{
    auto coordinates = toCoordinates(path, true, context);
    if (Orientation(path) == isHole)
    {
        std::reverse(coordinates.begin(), coordinates.end());
    }
    return coordinates;
}

size_t findRoot(std::vector<size_t>& parents, size_t index)
{
    while (parents[index] != index)
    {
        parents[index] = parents[parents[index]];
        index = parents[index];
    }
    return index;
}

std::vector<Paths> groupIntersectingPaths(Paths paths, const ProcessingContext& context)
{
    if (paths.size() <= 1)
    {
        return {std::move(paths)};
    }
    std::vector<GisSpatialBounds> bounds;
    bounds.reserve(paths.size());
    for (const auto& path : paths)
    {
        context.Check();
        bounds.push_back(pathBounds(path));
    }
    GisSpatialIndex index;
    context.Require(index.Build(std::move(bounds), context.GetCancellation()));
    std::vector<size_t> parents(paths.size());
    std::iota(parents.begin(), parents.end(), 0);
    std::vector<size_t> matches;
    for (size_t ring = 0; ring < paths.size(); ++ring)
    {
        context.Require(index.Query(index.GetBounds(ring), matches, context.GetCancellation()));
        for (const size_t match : matches)
        {
            const size_t firstRoot = findRoot(parents, ring);
            const size_t secondRoot = findRoot(parents, match);
            parents[std::max(firstRoot, secondRoot)] = std::min(firstRoot, secondRoot);
        }
    }
    // 包围盒不相交的环不能互相形成孔洞或相交，分组不改变奇偶填充。
    std::vector<Paths> groups(paths.size());
    for (size_t ring = 0; ring < paths.size(); ++ring)
    {
        groups[findRoot(parents, ring)].push_back(std::move(paths[ring]));
    }
    return groups;
}

bool pathContainsPath(const Path& outer, const Path& inner, const ProcessingContext& context)
{
    for (size_t index = 0; index < inner.size(); ++index)
    {
        if (index % 256 == 0)
        {
            context.Check();
        }
        const int containment = PointInPolygon(inner[index], outer);
        if (containment != -1)
        {
            return containment == 1;
        }
    }
    return true;
}

void appendClosedPaths(const Paths& paths, const AmapVectorFeature& source,
                       std::vector<AmapVectorFeature>& output, const ProcessingContext& context)
{
    std::vector<double> areas;
    std::vector<GisSpatialBounds> contourBounds;
    for (const auto& path : paths)
    {
        context.Check();
        areas.push_back(std::abs(Area(path)));
        contourBounds.push_back(pathBounds(path));
    }
    GisSpatialIndex index;
    context.Require(index.Build(std::move(contourBounds), context.GetCancellation()));
    std::vector<size_t> parents(paths.size(), paths.size());
    std::vector<std::vector<size_t>> holes(paths.size());
    std::vector<size_t> matches;
    for (size_t contour = 0; contour < paths.size(); ++contour)
    {
        const auto& bounds = index.GetBounds(contour);
        context.Require(index.Query(bounds, matches, context.GetCancellation()));
        double parentArea = std::numeric_limits<double>::infinity();
        for (const size_t candidate : matches)
        {
            context.Check();
            if (areas[candidate] > areas[contour] && areas[candidate] < parentArea &&
                index.GetBounds(candidate).Contains(bounds) &&
                pathContainsPath(paths[candidate], paths[contour], context))
            {
                parents[contour] = candidate;
                parentArea = areas[candidate];
            }
        }
    }
    // 严格简单化拆出的独立外环可能沿用负绕向，孔洞身份必须由包含层级决定。
    std::vector<bool> isHole(paths.size());
    for (size_t contour = 0; contour < paths.size(); ++contour)
    {
        context.Check();
        size_t ancestor = parents[contour];
        while (ancestor != paths.size())
        {
            isHole[contour] = !isHole[contour];
            ancestor = parents[ancestor];
        }
        if (isHole[contour])
        {
            holes[parents[contour]].push_back(contour);
        }
    }
    for (size_t outer = 0; outer < paths.size(); ++outer)
    {
        context.Check();
        if (isHole[outer] || areas[outer] == 0. || paths[outer].size() < 3)
        {
            continue;
        }
        AmapVectorFeature clipped = source;
        clipped.geometryParts.clear();
        clipped.geometryParts.push_back(polygonRingCoordinates(paths[outer], false, context));
        for (const size_t hole : holes[outer])
        {
            clipped.geometryParts.push_back(polygonRingCoordinates(paths[hole], true, context));
        }
        output.push_back(std::move(clipped));
    }
}

void appendClippedFeature(const AmapVectorFeature& feature, const Paths& mask,
                          std::vector<AmapVectorFeature>& output, const ProcessingContext& context,
                          bool fullyInside)
{
    Paths subject = toFeaturePaths(feature, context);
    if (feature.geometryType == DownTool::AmapVectorGeometryType::Polygon)
    {
        for (const auto& group : groupIntersectingPaths(std::move(subject), context))
        {
            context.Check();
            if (!group.empty())
            {
                // 保留严格简单化，但不让 PolyTree 在每次拆环时反复修复全局父子关系。
                const auto clipped = fullyInside ? clipClosed(group, {}, ctUnion, context)
                                                 : clipClosed(group, mask, ctIntersection, context);
                appendClosedPaths(clipped, feature, output, context);
            }
        }
        return;
    }
    Clipper clipper;
    if (!clipper.AddPaths(subject, ptSubject, false))
    {
        return;
    }
    clipper.AddPaths(mask, ptClip, true);
    PolyTree result;
    if (!clipper.Execute(ctIntersection, result, pftEvenOdd, pftNonZero))
    {
        throw std::runtime_error("GIS 线范围裁切失败。");
    }
    context.Check();
    Paths lines;
    OpenPathsFromPolyTree(result, lines);
    for (const auto& line : lines)
    {
        if (line.size() >= 2)
        {
            AmapVectorFeature clipped = feature;
            clipped.geometryParts = {toCoordinates(line, false, context)};
            output.push_back(std::move(clipped));
        }
    }
}

void markCandidates(const Paths& paths, const GisSpatialIndex& index, std::vector<uint8_t>& flags,
                    uint8_t candidateFlag, const ProcessingContext& context)
{
    const uint8_t boundaryFlag =
        candidateFlag == kIncludedCandidate ? kIncludedBoundary : kExcludedBoundary;
    std::vector<size_t> matches;
    for (const auto& path : paths)
    {
        context.Check();
        context.Require(index.Query(pathBounds(path), matches, context.GetCancellation()));
        for (const size_t match : matches)
        {
            flags[match] |= candidateFlag;
        }
        for (size_t edge = 0; edge < path.size(); ++edge)
        {
            const auto& first = path[edge];
            const auto& second = path[(edge + 1) % path.size()];
            GisSpatialBounds edgeBounds;
            edgeBounds.Add(static_cast<double>(first.X), static_cast<double>(first.Y));
            edgeBounds.Add(static_cast<double>(second.X), static_cast<double>(second.Y));
            context.Require(index.Query(edgeBounds, matches, context.GetCancellation()));
            for (const size_t match : matches)
            {
                flags[match] |= boundaryFlag;
            }
        }
    }
}
} // namespace

const char* DownTool::GisProcessingStageName(GisProcessingStage stage)
{
    switch (stage)
    {
    case GisProcessingStage::Merge:
        return "整理 GIS 数据";
    case GisProcessingStage::BuildIndex:
        return "准备 GIS 范围";
    case GisProcessingStage::Query:
        return "查询 GIS 范围";
    case GisProcessingStage::Filter:
        return "处理 GIS 范围";
    }
    return "处理 GIS 数据";
}

bool DownTool::ApplyGisSpatialFilter(AmapVectorDecodedData& decodedData,
                                     const GisSpatialRange& range, std::string& error,
                                     const std::function<bool()>& cancellationRequested,
                                     const GisProcessingProgressCallback& progressCallback)
{
    const ProcessingContext context(cancellationRequested, progressCallback);
    try
    {
        context.Check();
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
        Paths included = toClipPaths(range.GetIncludedPolygons(), context);
        const Paths excluded = toClipPaths(range.GetExcludedPolygons(), context);
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
            throw std::runtime_error("GIS 下载区合并失败。");
        }
        const Paths mask = excluded.empty()
                               ? unionIncluded
                               : clipClosed(unionIncluded, excluded, ctDifference, context);

        context.Report(GisProcessingStage::BuildIndex, 0, decodedData.features.size());
        std::vector<GisSpatialBounds> bounds;
        bounds.reserve(decodedData.features.size());
        for (const auto& feature : decodedData.features)
        {
            bounds.push_back(featureBounds(feature, context));
        }
        GisSpatialIndex index;
        context.Require(index.Build(std::move(bounds), cancellationRequested));
        context.Report(GisProcessingStage::BuildIndex, index.GetSize(), index.GetSize());

        context.Report(GisProcessingStage::Query, 0, 1);
        std::vector<uint8_t> flags(index.GetSize());
        markCandidates(unionIncluded, index, flags, kIncludedCandidate, context);
        markCandidates(excluded, index, flags, kExcludedCandidate, context);
        context.Report(GisProcessingStage::Query, 1, 1);

        std::vector<AmapVectorFeature> selected;
        selected.reserve(decodedData.features.size());
        for (size_t featureIndex = 0; featureIndex < decodedData.features.size(); ++featureIndex)
        {
            context.Report(GisProcessingStage::Filter, featureIndex, decodedData.features.size());
            const auto& feature = decodedData.features[featureIndex];
            const uint8_t candidates = flags[featureIndex];
            const auto& featureBox = index.GetBounds(featureIndex);
            if (mask.empty() || !featureBox.IsValid() || !(candidates & kIncludedCandidate))
            {
                continue;
            }
            const IntPoint center{
                static_cast<cInt>(featureBox.GetWest() +
                                  (featureBox.GetEast() - featureBox.GetWest()) / 2.),
                static_cast<cInt>(featureBox.GetSouth() +
                                  (featureBox.GetNorth() - featureBox.GetSouth()) / 2.)};
            const bool includedBoundary = candidates & kIncludedBoundary;
            const bool fullyIncluded = !includedBoundary && contains(unionIncluded, center);
            if (!includedBoundary && !fullyIncluded)
            {
                continue;
            }
            bool mayIntersectExclusion = candidates & kExcludedCandidate;
            if (mayIntersectExclusion && !(candidates & kExcludedBoundary))
            {
                if (contains(excluded, center))
                {
                    continue;
                }
                mayIntersectExclusion = false;
            }
            if (feature.geometryType == AmapVectorGeometryType::Point)
            {
                for (const auto& part : feature.geometryParts)
                {
                    for (const auto& coordinate : part)
                    {
                        context.Check();
                        const auto point = toPoint(coordinate.longitude, coordinate.latitude);
                        if (contains(unionIncluded, point) && !contains(excluded, point))
                        {
                            AmapVectorFeature selectedPoint = feature;
                            selectedPoint.geometryParts = {{coordinate}};
                            selected.push_back(std::move(selectedPoint));
                        }
                    }
                }
            }
            else if (ClassifyAmapVectorFeature(feature) == AmapVectorRenderCategory::Building)
            {
                if (fullyIncluded && !mayIntersectExclusion && hasSimpleBuildingFootprint(feature))
                {
                    selected.push_back(feature);
                    continue;
                }
                const Paths footprint = toFeaturePaths(feature, context);
                if (!clipClosed(footprint, unionIncluded, ctIntersection, context).empty() &&
                    (!mayIntersectExclusion ||
                     (clipClosed(footprint, excluded, ctIntersection, context).empty() &&
                      !touchesBoundary(footprint, excluded, context))))
                {
                    selected.push_back(feature);
                }
            }
            else
            {
                appendClippedFeature(feature, mask, selected, context,
                                     fullyIncluded && !mayIntersectExclusion);
            }
        }
        context.Report(GisProcessingStage::Filter, decodedData.features.size(),
                       decodedData.features.size());
        decodedData.features = std::move(selected);
        return true;
    }
    catch (const FilterCanceled&)
    {
        error = "GIS 范围处理已取消。";
        return false;
    }
    catch (const std::exception& exception)
    {
        error = "GIS 范围处理失败：" + std::string(exception.what());
        return false;
    }
}
