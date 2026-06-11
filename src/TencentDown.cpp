#include "TencentDown.h"

#include "fmt/format.h"

DownTool::TencentDown::TencentDown()
{
	ServerRange.DomServerRange = DownTool::IntVector2(0, 3);
	SetCachePath("cache/腾讯地图");
}

std::string DownTool::TencentDown::MakeUri(const Tile& tile)
{
	const uint64_t tencentY = ToTencentY(tile);
	const uint64_t x16 = tile.X / 16;
	const uint64_t y16 = tencentY / 16;

	return fmt::format(
		"https://p{}.map.gtimg.com/sateTiles/{}/{}/{}/{}_{}.png",
		ServerRange.GetNextServerIndex(),
		static_cast<uint32_t>(tile.Zoom),
		x16,
		y16,
		tile.X,
		tencentY);
}

uint64_t DownTool::TencentDown::ToTencentY(const Tile& tile)
{
	const uint64_t tileCount = uint64_t{ 1 } << tile.Zoom;
	return tileCount - 1 - tile.Y;
}
