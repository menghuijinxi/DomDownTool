#pragma once

#include <atomic>
#include <cstdint>

#include "DataType.h"

namespace DownTool
{
class BaiduVectorTile
{
  public:
    BaiduVectorTile(int32_t x = 0, int32_t y = 0, uint32_t zoom = 19) : x_(x), y_(y), zoom_(zoom) {}
    int32_t GetX() const { return x_; }
    int32_t GetY() const { return y_; }
    uint32_t GetZoom() const { return zoom_; }

  protected:
    int32_t x_;
    int32_t y_;
    uint32_t zoom_;
};

class BaiduVectorTileGrid
{
  public:
    static uint32_t ResolveDataZoom(uint32_t displayZoom);
    static uint32_t LocalSize(uint32_t nativeZoom);
    static double MercatorSpan(uint32_t nativeZoom);
    static uint32_t StartZoom(uint32_t nativeZoom);
    static uint32_t EndZoom(uint32_t nativeZoom);
    static void Validate(const BaiduVectorTile& tile);
    static BaiduVectorTile LonLatToTile(const LonLat& bd09, uint32_t nativeZoom);
    static LonLat LocalToWgs84(const BaiduVectorTile& tile, double x, double y);
};
} // namespace DownTool
