#pragma once
#include <cpl_port.h>
#include <cstdint>
#include <algorithm>

namespace DownTool
{
    // 将经纬度（经度 lon [-180,180], 纬度 lat [-85.0511, 85.0511]）转为瓦片坐标
    Tile lonLatToTile(double lon, double lat, uint8_t zoom) 
	{
        // 限制纬度范围（Web Mercator 无法表示极点）
        lat = std::max(-85.0511, std::min(85.0511, lat));

        double latRad = lat * M_PI / 180.0;
        double n = std::pow(2.0, zoom);

        uint32_t x = static_cast<uint32_t>((lon + 180.0) / 360.0 * n);
        uint32_t y = static_cast<uint32_t>((1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / M_PI) / 2.0 * n);

        return { x, y, zoom };
    }
}
