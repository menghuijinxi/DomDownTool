#include "GoogleDownBase.h"
#include "numbers"

void DownTool::GoogleDownBase::CalculateDownRange()
{
	DownToolBase::CalculateDownRange();

	//根据MinLonLat通过谷歌瓦片坐标计算公式计算得到MinTile
	MinTile = LonLatToTile(GetMinLonLat(), GetZoom());
	MaxTile = LonLatToTile(GetMaxLonLat(), GetZoom());
	uint32_t temp = MinTile.Y;
	MinTile.Y = MaxTile.Y;
	MaxTile.Y = temp;
}

DownTool::LonLat DownTool::GoogleDownBase::TileToLonLat(const Tile& tile)
{
	double n = std::pow(2., tile.Zoom);
	double lon = tile.X / n * 360. - 180.;
	double lat_rad = std::atan(std::sinh(std::numbers::pi * (1 - 2 * tile.Y / n)));
	double lat = lat_rad * 180. / std::numbers::pi;
	return { lon, lat };
}

DownTool::Tile DownTool::GoogleDownBase::LonLatToTile(const LonLat& lonLat, uint32_t zoom)
{
	Tile tile;
	tile.Zoom = zoom;
	tile.X = static_cast<uint32_t>(std::floor((lonLat.lon + 180.) / 360. * std::pow(2., zoom)));
	tile.Y = static_cast<uint32_t>(std::floor((1. - std::log(std::tan(lonLat.lat * std::numbers::pi / 180.) + 1. / std::cos(lonLat.lat * std::numbers::pi / 180.)) / std::numbers::pi) / 2. * std::pow(2., zoom)));
	return tile;
}
