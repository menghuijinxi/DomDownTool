#include "GoogleDownBase.h"
#include "numbers"

void DownTool::GoogleDownBase::CalculateDownRange()
{
	DownToolBase::CalculateDownRange();

	//根据MinLonLat通过谷歌瓦片坐标计算公式计算得到MinTile
	MinTile.Zoom = GetZoom();
	MinTile.X = static_cast<uint32_t>(std::floor((GetMinLonLat().lon + 180.) / 360. * std::pow(2., GetZoom())));
	MinTile.Y = static_cast<uint32_t>(std::floor((1. - std::log(std::tan(GetMinLonLat().lat * std::numbers::pi / 180.) + 1. / std::cos(GetMinLonLat().lat * std::numbers::pi / 180.)) / std::numbers::pi) / 2. * std::pow(2., GetZoom())));
	
	MaxTile.Zoom = GetZoom();
	MaxTile.X = static_cast<uint32_t>(std::floor((GetMaxLonLat().lon + 180.) / 360. * std::pow(2., GetZoom())));
	MaxTile.Y = static_cast<uint32_t>(std::floor((1. - std::log(std::tan(GetMaxLonLat().lat * std::numbers::pi / 180.) + 1. / std::cos(GetMaxLonLat().lat * std::numbers::pi / 180.)) / std::numbers::pi) / 2. * std::pow(2., GetZoom())));

	uint32_t temp = MinTile.Y;
	MinTile.Y = MaxTile.Y;
	MaxTile.Y = temp;
}
