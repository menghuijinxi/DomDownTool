#include "BingDown.h"

#include <cstdint>
#include <string>

#include "fmt/format.h"

DownTool::BingDown::BingDown()
{
    ServerRange.DomServerRange = DownTool::IntVector2(0, 3);
    SetUrlTemplate(
        "https://ecn.t{s}.tiles.virtualearth.net/tiles/a{quadkey}.jpeg?g=0");
    SetCachePath("cache/Bing地图");
}

std::string DownTool::BingDown::MakeUri(const Tile& tile)
{
    const std::string quadKey = TileToQuadKey(tile);
    const int32_t serverIndex = ServerRange.GetNextServerIndex();
    auto x = fmt::arg("x", tile.X);
    auto y = fmt::arg("y", tile.Y);
    auto z = fmt::arg("z", tile.Zoom);
    auto s = fmt::arg("s", serverIndex);
    auto p = fmt::arg("p", quadKey);
    auto namedQuadKey = fmt::arg("quadkey", quadKey);
    fmt::format_args args = fmt::make_format_args(x, y, z, s, p, namedQuadKey);
    return fmt::vformat(GetUrlTemplate(), args);
}

std::string DownTool::BingDown::TileToQuadKey(const Tile& tile)
{
    std::string quadKey;
    quadKey.reserve(tile.Zoom);
    for (uint32_t level = tile.Zoom; level > 0; --level)
    {
        char digit = '0';
        const uint32_t mask = uint32_t{1} << (level - 1);
        if ((static_cast<uint32_t>(tile.X) & mask) != 0)
        {
            ++digit;
        }
        if ((static_cast<uint32_t>(tile.Y) & mask) != 0)
        {
            digit += 2;
        }
        quadKey.push_back(digit);
    }
    return quadKey;
}
