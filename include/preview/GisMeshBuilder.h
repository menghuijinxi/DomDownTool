#pragma once

#include <cstdint>
#include <vector>

#include "AmapVectorDecoder.h"
#include "preview/MapViewport.h"

namespace DomDownToolPreview
{
struct GisMeshBounds
{
    double minLon = 0.;
    double maxLon = 0.;
    double minLat = 0.;
    double maxLat = 0.;
    bool initialized = false;
};

struct GisMeshFeature
{
    DownTool::AmapVectorGeometryType geometryType = DownTool::AmapVectorGeometryType::Point;
    std::vector<std::vector<DownTool::LonLat>> geometryParts;
    GisMeshBounds bounds;
    uint32_t minZoom = 0;
    uint32_t maxZoom = 0;
};

struct GisMeshVertex
{
    float x = 0.F;
    float y = 0.F;
};

struct GisLineVertex
{
    float startX = 0.F;
    float startY = 0.F;
    float endX = 0.F;
    float endY = 0.F;
    float along = 0.F;
    float side = 0.F;
};

struct GisPointVertex
{
    float centerX = 0.F;
    float centerY = 0.F;
    float offsetX = 0.F;
    float offsetY = 0.F;
};

struct GisGeometryMesh
{
    // 顶点坐标相对这个源世界原点保存，避免高缩放层级下 float 丢失亚像素精度。
    double sourceOriginX = 0.;
    double sourceOriginY = 0.;
    uint32_t sourceZoom = 0;
    std::vector<GisMeshVertex> fillVertices;
    std::vector<uint32_t> fillIndices;
    std::vector<GisLineVertex> lineVertices;
    std::vector<uint32_t> lineIndices;
    std::vector<GisPointVertex> pointVertices;
    std::vector<uint32_t> pointIndices;
    uint64_t revision = 0;
};

class GisMeshBuilder
{
  public:
    [[nodiscard]] static GisGeometryMesh Build(
        const std::vector<GisMeshFeature>& features,
        const ViewportTransform& transform,
        uint32_t zoom);
};
} // namespace DomDownToolPreview
