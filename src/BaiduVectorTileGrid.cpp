#include "BaiduVectorTileGrid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include "BaiduDown.h"

namespace
{
struct Grid
{
    uint32_t zoom;
    uint32_t size;
    double span;
    uint32_t start;
    uint32_t end;
};
constexpr std::array<Grid, 9> kGrids = {{{3, 256, 8388608., 3, 3},
                                         {5, 512, 4194304., 4, 5},
                                         {7, 512, 1048576., 6, 7},
                                         {9, 512, 262144., 8, 9},
                                         {10, 256, 65536., 10, 10},
                                         {12, 512, 32768., 11, 13},
                                         {15, 512, 4096., 14, 15},
                                         {17, 1024, 2048., 16, 17},
                                         {19, 1024, 512., 18, 21}}};

const Grid& FindGrid(uint32_t zoom)
{
    const auto found = std::find_if(kGrids.begin(), kGrids.end(),
                                    [zoom](const Grid& grid) { return grid.zoom == zoom; });
    if (found == kGrids.end())
    {
        throw std::invalid_argument("百度 GIS 必须使用已验证的原生数据层级。");
    }
    return *found;
}
} // namespace

uint32_t DownTool::BaiduVectorTileGrid::ResolveDataZoom(uint32_t displayZoom)
{
    displayZoom = std::clamp(displayZoom, 3u, 21u);
    for (const auto& grid : kGrids)
    {
        if (displayZoom <= grid.end)
        {
            return grid.zoom;
        }
    }
    return 19;
}

uint32_t DownTool::BaiduVectorTileGrid::LocalSize(uint32_t nativeZoom)
{
    return FindGrid(nativeZoom).size;
}

double DownTool::BaiduVectorTileGrid::MercatorSpan(uint32_t nativeZoom)
{
    return FindGrid(nativeZoom).span;
}

uint32_t DownTool::BaiduVectorTileGrid::StartZoom(uint32_t nativeZoom)
{
    return FindGrid(nativeZoom).start;
}

uint32_t DownTool::BaiduVectorTileGrid::EndZoom(uint32_t nativeZoom)
{
    return FindGrid(nativeZoom).end;
}

void DownTool::BaiduVectorTileGrid::Validate(const BaiduVectorTile& tile)
{
    const double span = MercatorSpan(tile.GetZoom());
    if (std::abs(double(tile.GetX()) * span) > 30000000. ||
        std::abs(double(tile.GetY()) * span) > 20000000.)
    {
        throw std::invalid_argument("百度 GIS 原生瓦片坐标超出有效范围。");
    }
}

DownTool::BaiduVectorTile DownTool::BaiduVectorTileGrid::LonLatToTile(const LonLat& bd09,
                                                                      uint32_t nativeZoom)
{
    if (!std::isfinite(bd09.lon) || !std::isfinite(bd09.lat) || std::abs(bd09.lon) > 180. ||
        std::abs(bd09.lat) > 74.)
    {
        throw std::invalid_argument("百度 GIS 的 BD09 经纬度无效。");
    }
    const auto mercator = BaiduDown::BD09ToMercator(bd09);
    const double span = MercatorSpan(nativeZoom);
    const BaiduVectorTile tile{static_cast<int32_t>(std::floor(mercator.x / span)),
                               static_cast<int32_t>(std::floor(mercator.y / span)), nativeZoom};
    Validate(tile);
    return tile;
}

DownTool::LonLat DownTool::BaiduVectorTileGrid::LocalToWgs84(const BaiduVectorTile& tile, double x,
                                                             double y)
{
    const auto& grid = FindGrid(tile.GetZoom());
    const auto bd09 =
        BaiduDown::MercatorToBD09({tile.GetX() * grid.span + x * grid.span / grid.size,
                                   tile.GetY() * grid.span + y * grid.span / grid.size});
    return BaiduDown::BD09ToWGS84(bd09);
}
