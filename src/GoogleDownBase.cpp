#include "GoogleDownBase.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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

bool DownTool::GoogleDownBase::TryGetLonLatForTilePixel(
	const Tile& tile, double pixelX, double pixelY, LonLat& lonLat) const
{
	const double tileSize = static_cast<double>(TileSize);
	if (tileSize <= 0.)
	{
		return false;
	}

	const double n = std::pow(2., tile.Zoom);
	const double globalX = static_cast<double>(tile.X) + pixelX / tileSize;
	const double globalY = static_cast<double>(tile.Y) + pixelY / tileSize;
	lonLat.lon = globalX / n * 360. - 180.;
	const double latRadians =
		std::atan(std::sinh(std::numbers::pi * (1. - 2. * globalY / n)));
	lonLat.lat = latRadians * 180. / std::numbers::pi;
	return true;
}

bool DownTool::GoogleDownBase::TryGetTilePixelForLonLat(
	const LonLat& lonLat,
	uint32_t zoom,
	Tile& tile,
	double& pixelX,
	double& pixelY) const
{
	const double tileSize = static_cast<double>(TileSize);
	if (tileSize <= 0.)
	{
		return false;
	}

	const double n = std::pow(2., zoom);
	const double latRadians = lonLat.lat * std::numbers::pi / 180.;
	double globalX = (lonLat.lon + 180.) / 360. * n;
	double globalY =
		(1. - std::log(std::tan(latRadians) + 1. / std::cos(latRadians)) /
			std::numbers::pi) / 2. * n;

	const double maxGlobal = std::nextafter(n, 0.);
	globalX = std::clamp(globalX, 0., maxGlobal);
	globalY = std::clamp(globalY, 0., maxGlobal);

	tile.Zoom = zoom;
	tile.X = static_cast<uint32_t>(std::floor(globalX));
	tile.Y = static_cast<uint32_t>(std::floor(globalY));
	pixelX = (globalX - tile.X) * tileSize;
	pixelY = (globalY - tile.Y) * tileSize;
	return true;
}

DownTool::Tile DownTool::GoogleDownBase::LonLatToTile(const LonLat& lonLat, uint32_t zoom)
{
	Tile tile;
	tile.Zoom = zoom;
	tile.X = static_cast<uint32_t>(std::floor((lonLat.lon + 180.) / 360. * std::pow(2., zoom)));
	tile.Y = static_cast<uint32_t>(std::floor((1. - std::log(std::tan(lonLat.lat * std::numbers::pi / 180.) + 1. / std::cos(lonLat.lat * std::numbers::pi / 180.)) / std::numbers::pi) / 2. * std::pow(2., zoom)));
	return tile;
}
