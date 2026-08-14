#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "DataType.h"

namespace DownTool
{
    struct AmapVectorTile
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t zoom = 0;
    };

    class AmapVectorTileGrid
    {
    public:
        static uint32_t ResolveDataZoom(uint32_t renderZoom);

        // 输入和半径使用 WGS84；返回的瓦片属于高德 GCJ-02 矢量网格。
        static std::vector<AmapVectorTile> SelectTilesForWgs84Radius(
            const LonLat& centerWgs84,
            double radiusMeters,
            uint32_t zoom);

        // 输入为 WGS84 西北角和东南角；返回覆盖完整边界的高德 GCJ-02 矢量瓦片。
        static std::vector<AmapVectorTile> SelectTilesForWgs84Bounds(
            const LonLat& northWestWgs84,
            const LonLat& southEastWgs84,
            uint32_t zoom);

        static AmapVectorTile LonLatToTile(const LonLat& gcj02LonLat, uint32_t zoom);

        static LonLat TileNorthWestToLonLat(const AmapVectorTile& tile);

        static LonLat TileSouthEastToLonLat(const AmapVectorTile& tile);
    };
}
