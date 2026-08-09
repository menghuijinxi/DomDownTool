#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "GoogleDownBase.h"
#include "preview/GisMeshBuilder.h"

namespace
{
using DomDownToolPreview::GisGeometryMesh;
using DomDownToolPreview::GisMeshBuilder;
using DomDownToolPreview::GisMeshFeature;
using DomDownToolPreview::TileYAxisDirection;
using DomDownToolPreview::ViewportSourcePoint;
using DomDownToolPreview::ViewportTransform;
using DownTool::AmapVectorGeometryType;
using DownTool::LonLat;

double ringArea(const std::vector<LonLat>& ring, const ViewportTransform& transform)
{
    double area = 0.;
    for (size_t index = 0; index < ring.size(); ++index)
    {
        const std::optional<ViewportSourcePoint> current =
            transform.sourcePointForLonLat(ring[index]);
        const std::optional<ViewportSourcePoint> next =
            transform.sourcePointForLonLat(ring[(index + 1) % ring.size()]);
        if (!current.has_value() || !next.has_value())
        {
            throw std::runtime_error("test coordinate could not be projected");
        }
        area += current->x * next->y - next->x * current->y;
    }
    return std::abs(area * 0.5);
}

double triangleArea(const GisGeometryMesh& mesh)
{
    double area = 0.;
    for (size_t index = 0; index + 2 < mesh.fillIndices.size(); index += 3)
    {
        const auto& first = mesh.fillVertices.at(mesh.fillIndices[index]);
        const auto& second = mesh.fillVertices.at(mesh.fillIndices[index + 1]);
        const auto& third = mesh.fillVertices.at(mesh.fillIndices[index + 2]);
        area += std::abs((static_cast<double>(second.x) - first.x) *
                             (static_cast<double>(third.y) - first.y) -
                         (static_cast<double>(third.x) - first.x) *
                             (static_cast<double>(second.y) - first.y)) *
            0.5;
    }
    return area;
}

ViewportTransform makeTransform(DownTool::GoogleDownBase& projection)
{
    projection.SetTileSize(256);
    ViewportTransform transform;
    transform.sourceZoom = 15;
    transform.projection = &projection;
    transform.yDirection = TileYAxisDirection::Down;
    transform.tileSize = 256;
    DownTool::Tile tile;
    double pixelX = 0.;
    double pixelY = 0.;
    if (!projection.TryProjectToSourceTile({116.400, 39.910}, transform.sourceZoom, tile,
                                           pixelX, pixelY))
    {
        throw std::runtime_error("test center could not be projected");
    }
    transform.centerSourceX = static_cast<double>(tile.X) * transform.tileSize + pixelX;
    transform.centerSourceY = static_cast<double>(tile.Y) * transform.tileSize + pixelY;
    return transform;
}

void verifyPolygonHoleTriangulation(const ViewportTransform& transform)
{
    const std::vector<LonLat> outer = {
        {116.390, 39.900}, {116.410, 39.900}, {116.410, 39.920},
        {116.390, 39.920}, {116.390, 39.900}};
    const std::vector<LonLat> hole = {
        {116.396, 39.906}, {116.404, 39.906}, {116.404, 39.914},
        {116.396, 39.914}, {116.396, 39.906}};
    GisMeshFeature polygon;
    polygon.geometryType = AmapVectorGeometryType::Polygon;
    polygon.geometryParts = {outer, hole};

    const GisGeometryMesh mesh = GisMeshBuilder::Build({polygon}, transform, 15);
    if (mesh.fillVertices.size() != 8 || mesh.fillIndices.empty() ||
        mesh.fillIndices.size() % 3 != 0)
    {
        throw std::runtime_error("polygon mesh did not preserve the outer ring and hole");
    }
    const double expectedArea = ringArea(outer, transform) - ringArea(hole, transform);
    const double actualArea = triangleArea(mesh);
    const double relativeError = std::abs(actualArea - expectedArea) / expectedArea;
    if (relativeError > 1e-4)
    {
        throw std::runtime_error("polygon triangulation area mismatch: expected=" +
                                 std::to_string(expectedArea) +
                                 ", actual=" + std::to_string(actualArea) +
                                 ", relative_error=" + std::to_string(relativeError));
    }
}

void verifyLineAndPointBatches(const ViewportTransform& transform)
{
    GisMeshFeature line;
    line.geometryType = AmapVectorGeometryType::LineString;
    line.geometryParts = {{{116.390, 39.900}, {116.400, 39.910}, {116.410, 39.905}}};
    GisMeshFeature point;
    point.geometryType = AmapVectorGeometryType::Point;
    point.geometryParts = {{{116.400, 39.910}}};

    const GisGeometryMesh mesh = GisMeshBuilder::Build({line, point}, transform, 15);
    if (mesh.lineVertices.size() != 8 || mesh.lineIndices.size() != 12)
    {
        throw std::runtime_error("line segments were not batched into indexed quads");
    }
    if (mesh.pointVertices.size() != 4 || mesh.pointIndices.size() != 6)
    {
        throw std::runtime_error("point was not batched into one indexed quad");
    }
}

void verifyZoomFiltering(const ViewportTransform& transform)
{
    GisMeshFeature feature;
    feature.geometryType = AmapVectorGeometryType::Point;
    feature.geometryParts = {{{116.400, 39.910}}};
    feature.minZoom = 16;
    const GisGeometryMesh hidden = GisMeshBuilder::Build({feature}, transform, 15);
    if (!hidden.pointIndices.empty())
    {
        throw std::runtime_error("feature below its minimum zoom was included in the mesh");
    }
}

void verifyLocalOriginCompensation(const ViewportTransform& transform)
{
    GisMeshFeature point;
    point.geometryType = AmapVectorGeometryType::Point;
    point.geometryParts = {{{116.400, 39.910}}};
    const GisGeometryMesh mesh = GisMeshBuilder::Build({point}, transform, 15);
    if (mesh.sourceZoom != transform.sourceZoom)
    {
        throw std::runtime_error("mesh did not retain its source zoom");
    }
    const ViewportSourcePoint meshOrigin{mesh.sourceOriginX, mesh.sourceOriginY};

    ViewportTransform moved = transform;
    moved.centerSourceX += 40.;
    moved.centerSourceY -= 25.;
    const DomDownToolPreview::ViewportPoint screen =
        moved.screenPointForSourcePoint(meshOrigin);
    const double expectedX = moved.centerScreenX - 40. * moved.displayScale;
    const double expectedY = moved.centerScreenY + 25. * moved.displayScale;
    if (std::abs(screen.x - expectedX) > 1e-9 || std::abs(screen.y - expectedY) > 1e-9)
    {
        throw std::runtime_error("local mesh origin was not compensated for camera movement");
    }
}
} // namespace

int main()
{
    try
    {
        DownTool::GoogleDownBase projection;
        const ViewportTransform transform = makeTransform(projection);
        verifyPolygonHoleTriangulation(transform);
        verifyLineAndPointBatches(transform);
        verifyZoomFiltering(transform);
        verifyLocalOriginCompensation(transform);
        std::cout << "GisMeshBuilderTest passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "GisMeshBuilderTest failed: " << exception.what() << '\n';
        return 1;
    }
}
