#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GeographicPolygon.h"

namespace DownTool
{
class FbxTerrainClipPoint
{
  public:
    FbxTerrainClipPoint(double x, double y) : x_(x), y_(y) {}
    double GetX() const { return x_; }
    double GetY() const { return y_; }

  protected:
    double x_;
    double y_;
};

using FbxTerrainClipTriangle = std::array<FbxTerrainClipPoint, 3>;

// 使用 Web Mercator 米坐标裁切；调用方插值高度、UV，并在最后统一重算法线。
class FbxTerrainRegionClipper
{
  public:
    FbxTerrainRegionClipper();
    ~FbxTerrainRegionClipper();
    FbxTerrainRegionClipper(const FbxTerrainRegionClipper&) = delete;
    FbxTerrainRegionClipper& operator=(const FbxTerrainRegionClipper&) = delete;

    bool Initialize(const Wgs84Polygons& included, const Wgs84Polygons& excluded,
                    std::string& error, const std::function<bool()>& cancellationRequested = {});
    bool IsActive() const;
    bool HasVisibleArea(const GeographicBounds& bounds, bool& visible, std::string& error) const;
    bool ClipTriangle(const FbxTerrainClipTriangle& triangle,
                      std::vector<FbxTerrainClipTriangle>& output, std::string& error) const;

  protected:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace DownTool
