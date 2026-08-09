#include "preview/GisMeshBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <mapbox/earcut.hpp>

namespace
{
using DomDownToolPreview::GisMeshFeature;
using DomDownToolPreview::GisGeometryMesh;
using DomDownToolPreview::ViewportSourcePoint;
using DownTool::AmapVectorGeometryType;
using DownTool::LonLat;

struct Point
{
    double x = 0.;
    double y = 0.;
};

using Ring = std::vector<Point>;
using Polygon = std::vector<Ring>;

bool isFinite(const Point& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

double signedArea(const Ring& ring)
{
    if (ring.size() < 3)
    {
        return 0.;
    }

    double area = 0.;
    for (size_t index = 0; index < ring.size(); ++index)
    {
        const Point& current = ring[index];
        const Point& next = ring[(index + 1) % ring.size()];
        area += current.x * next.y - next.x * current.y;
    }
    return area * 0.5;
}

bool containsPoint(const Ring& ring, const Point& point)
{
    bool inside = false;
    for (size_t index = 0, previous = ring.size() - 1; index < ring.size();
         previous = index++)
    {
        const Point& current = ring[index];
        const Point& last = ring[previous];
        const bool intersects = ((current.y > point.y) != (last.y > point.y)) &&
            point.x < (last.x - current.x) * (point.y - current.y) /
                    (last.y - current.y) + current.x;
        if (intersects)
        {
            inside = !inside;
        }
    }
    return inside;
}

Ring projectRing(const std::vector<LonLat>& sourceRing,
                 const DomDownToolPreview::ViewportTransform& transform)
{
    Ring ring;
    ring.reserve(sourceRing.size());
    for (const LonLat& coordinate : sourceRing)
    {
        const std::optional<ViewportSourcePoint> point =
            transform.sourcePointForLonLat(coordinate);
        if (!point.has_value())
        {
            continue;
        }
        const Point projected{point->x - transform.centerSourceX,
                              point->y - transform.centerSourceY};
        if (isFinite(projected))
        {
            ring.push_back(projected);
        }
    }

    while (ring.size() > 1 && ring.front().x == ring.back().x &&
           ring.front().y == ring.back().y)
    {
        ring.pop_back();
    }
    ring.erase(std::unique(ring.begin(), ring.end(), [](const Point& left, const Point& right) {
        return left.x == right.x && left.y == right.y;
    }), ring.end());
    return ring;
}

std::vector<Polygon> classifyRings(const std::vector<Ring>& rings)
{
    struct RingInfo
    {
        Ring ring;
        double area = 0.;
        size_t parent = std::numeric_limits<size_t>::max();
        int depth = 0;
    };

    std::vector<RingInfo> sorted;
    sorted.reserve(rings.size());
    for (const Ring& ring : rings)
    {
        const double area = signedArea(ring);
        if (ring.size() >= 3 && std::abs(area) > 1e-12)
        {
            sorted.push_back({ring, area});
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const RingInfo& left, const RingInfo& right) {
        return std::abs(left.area) > std::abs(right.area);
    });

    std::vector<Polygon> polygons;
    std::vector<size_t> outerIndex;
    for (size_t index = 0; index < sorted.size(); ++index)
    {
        RingInfo& current = sorted[index];
        for (size_t candidate = 0; candidate < index; ++candidate)
        {
            if (!containsPoint(sorted[candidate].ring, current.ring.front()))
            {
                continue;
            }
            if (current.parent == std::numeric_limits<size_t>::max() ||
                std::abs(sorted[candidate].area) <
                    std::abs(sorted[current.parent].area))
            {
                current.parent = candidate;
            }
        }
        current.depth = current.parent == std::numeric_limits<size_t>::max()
            ? 0
            : sorted[current.parent].depth + 1;
        if ((current.depth & 1) == 0)
        {
            polygons.push_back({current.ring});
            outerIndex.push_back(index);
        }
        else
        {
            size_t parent = current.parent;
            while (parent != std::numeric_limits<size_t>::max() &&
                   (sorted[parent].depth & 1) != 0)
            {
                parent = sorted[parent].parent;
            }
            const auto outer = std::find(outerIndex.begin(), outerIndex.end(), parent);
            if (outer != outerIndex.end())
            {
                polygons[static_cast<size_t>(std::distance(outerIndex.begin(), outer))].push_back(
                    current.ring);
            }
        }
    }
    return polygons;
}

void appendFillGeometry(const std::vector<std::vector<LonLat>>& sourceRings,
                        const DomDownToolPreview::ViewportTransform& transform,
                        GisGeometryMesh& mesh)
{
    std::vector<Ring> rings;
    rings.reserve(sourceRings.size());
    for (const std::vector<LonLat>& sourceRing : sourceRings)
    {
        Ring ring = projectRing(sourceRing, transform);
        if (!ring.empty())
        {
            rings.push_back(std::move(ring));
        }
    }

    for (const Polygon& polygon : classifyRings(rings))
    {
        const size_t vertexOffset = mesh.fillVertices.size();
        for (const Ring& ring : polygon)
        {
            for (const Point& point : ring)
            {
                mesh.fillVertices.push_back(
                    {static_cast<float>(point.x), static_cast<float>(point.y)});
            }
        }
        std::vector<std::vector<std::array<double, 2>>> earcutPolygon;
        earcutPolygon.reserve(polygon.size());
        for (const Ring& ring : polygon)
        {
            std::vector<std::array<double, 2>> earcutRing;
            earcutRing.reserve(ring.size());
            for (const Point& point : ring)
            {
                earcutRing.push_back({point.x, point.y});
            }
            earcutPolygon.push_back(std::move(earcutRing));
        }
        const std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(earcutPolygon);
        for (const uint32_t index : indices)
        {
            mesh.fillIndices.push_back(static_cast<uint32_t>(vertexOffset) + index);
        }
    }
}

void appendLineGeometry(const std::vector<std::vector<LonLat>>& sourceParts,
                        const DomDownToolPreview::ViewportTransform& transform,
                        GisGeometryMesh& mesh)
{
    for (const std::vector<LonLat>& sourcePart : sourceParts)
    {
        if (sourcePart.size() < 2)
        {
            continue;
        }
        std::vector<ViewportSourcePoint> points;
        points.reserve(sourcePart.size());
        for (const LonLat& coordinate : sourcePart)
        {
            const std::optional<ViewportSourcePoint> point =
                transform.sourcePointForLonLat(coordinate);
            if (point.has_value())
            {
                points.push_back(*point);
            }
        }
        for (size_t index = 1; index < points.size(); ++index)
        {
            const uint32_t vertexOffset = static_cast<uint32_t>(mesh.lineVertices.size());
            const ViewportSourcePoint& start = points[index - 1];
            const ViewportSourcePoint& end = points[index];
            const float startX = static_cast<float>(start.x - transform.centerSourceX);
            const float startY = static_cast<float>(start.y - transform.centerSourceY);
            const float endX = static_cast<float>(end.x - transform.centerSourceX);
            const float endY = static_cast<float>(end.y - transform.centerSourceY);
            mesh.lineVertices.push_back({startX, startY, endX, endY, 0.F, -1.F});
            mesh.lineVertices.push_back({startX, startY, endX, endY, 0.F, 1.F});
            mesh.lineVertices.push_back({startX, startY, endX, endY, 1.F, -1.F});
            mesh.lineVertices.push_back({startX, startY, endX, endY, 1.F, 1.F});
            mesh.lineIndices.insert(mesh.lineIndices.end(), {vertexOffset, vertexOffset + 1,
                                                              vertexOffset + 2, vertexOffset + 2,
                                                              vertexOffset + 1, vertexOffset + 3});
        }
    }
}

void appendPointGeometry(const std::vector<std::vector<LonLat>>& sourceParts,
                         const DomDownToolPreview::ViewportTransform& transform,
                         GisGeometryMesh& mesh)
{
    for (const std::vector<LonLat>& sourcePart : sourceParts)
    {
        for (const LonLat& coordinate : sourcePart)
        {
            const std::optional<ViewportSourcePoint> point =
                transform.sourcePointForLonLat(coordinate);
            if (!point.has_value())
            {
                continue;
            }
            const uint32_t vertexOffset = static_cast<uint32_t>(mesh.pointVertices.size());
            const float x = static_cast<float>(point->x - transform.centerSourceX);
            const float y = static_cast<float>(point->y - transform.centerSourceY);
            mesh.pointVertices.insert(mesh.pointVertices.end(),
                                      {{x, y, -1.F, -1.F}, {x, y, -1.F, 1.F},
                                       {x, y, 1.F, -1.F}, {x, y, 1.F, 1.F}});
            mesh.pointIndices.insert(
                mesh.pointIndices.end(),
                {vertexOffset, vertexOffset + 1, vertexOffset + 2, vertexOffset + 2,
                 vertexOffset + 1, vertexOffset + 3});
        }
    }
}
} // namespace

DomDownToolPreview::GisGeometryMesh DomDownToolPreview::GisMeshBuilder::Build(
    const std::vector<GisMeshFeature>& features,
    const ViewportTransform& transform,
    uint32_t zoom)
{
    GisGeometryMesh mesh;
    mesh.sourceOriginX = transform.centerSourceX;
    mesh.sourceOriginY = transform.centerSourceY;
    mesh.sourceZoom = transform.sourceZoom;
    for (const GisMeshFeature& feature : features)
    {
        if (zoom < feature.minZoom || (feature.maxZoom != 0 && zoom > feature.maxZoom))
        {
            continue;
        }
        switch (feature.geometryType)
        {
        case DownTool::AmapVectorGeometryType::Polygon:
            appendFillGeometry(feature.geometryParts, transform, mesh);
            break;
        case DownTool::AmapVectorGeometryType::LineString:
            appendLineGeometry(feature.geometryParts, transform, mesh);
            break;
        case DownTool::AmapVectorGeometryType::Point:
            appendPointGeometry(feature.geometryParts, transform, mesh);
            break;
        }
    }
    mesh.revision = 1;
    return mesh;
}
