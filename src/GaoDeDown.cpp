#include "GaoDeDown.h"

DownTool::GaoDeDown::GaoDeDown()
{
	//SetUrlTemplate("https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=7&x={x}&y={y}&z={z}");

	ServerRange.DomServerRange = DownTool::IntVector2(1, 4);
	SetUrlTemplate("http://webst0{s}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&lang=zh_cn&size=1&scale=1&style=6");
	SetCachePath("cache/高德地图");
}

DownTool::TileRange DownTool::GaoDeDown::GetOffsetTiles(const Tile& tile) const
{
	//计算tile的最小经纬度和最大经纬度
	LonLat minLonLat = TileToLonLat(tile);
	LonLat maxLonLat = TileToLonLat({ tile.X + 1, tile.Y + 1, tile.Zoom });

	//将经纬度从WGS84坐标系转换为GCJ02坐标系
	minLonLat = WGS84ToGCJ02(minLonLat);
	maxLonLat = WGS84ToGCJ02(maxLonLat);

	return {LonLatToTile(minLonLat, tile.Zoom), LonLatToTile(maxLonLat, tile.Zoom)};
}

double DownTool::GaoDeDown::Transform_Lat(double lat, double lon)
{
	auto ret = -100.0 + 2.0 * lon + 3.0 * lat + 0.2 * lat * lat + 0.1 * lon * lat + 0.2 * std::sqrt(std::abs(lon));
	ret += (20.0 * std::sin(6.0 * lon * std::numbers::pi) + 20.0 * std::sin(2.0 * lon * std::numbers::pi)) * 2.0 / 3.0;
	ret += (20.0 * std::sin(lat * std::numbers::pi) + 40.0 * std::sin(lat / 3.0 * std::numbers::pi)) * 2.0 / 3.0;
	ret += (160.0 * std::sin(lat / 12.0 * std::numbers::pi) + 320 * std::sin(lat * std::numbers::pi / 30.0)) * 2.0 / 3.0;
	return ret;
}

double DownTool::GaoDeDown::Transform_Lng(double lng, double lat)
{
	auto ret = 300.0 + lng + 2.0 * lat + 0.1 * lng * lng + 0.1 * lng * lat + 0.1 * std::sqrt(std::abs(lng));
	ret += (20.0 * std::sin(6.0 * lng * std::numbers::pi) + 20.0 * std::sin(2.0 * lng * std::numbers::pi)) * 2.0 / 3.0;
	ret += (20.0 * std::sin(lng * std::numbers::pi) + 40.0 * std::sin(lng / 3.0 * std::numbers::pi)) * 2.0 / 3.0;
	ret += (150.0 * std::sin(lng / 12.0 * std::numbers::pi) + 300.0 * std::sin(lng / 30.0 * std::numbers::pi)) * 2.0 / 3.0;
	return ret;
}

const double DownTool::GaoDeDown::a_gcj02 = 6378245.0;
const double DownTool::GaoDeDown::ee_gcj02 = 0.00669342162296594323;

DownTool::LonLat DownTool::GaoDeDown::GCJ02ToWGS84(const LonLat& tile)
{
	if (!IsInChina(tile.lon, tile.lat))
	{
		return tile;
	}

	auto dlat = Transform_Lat(tile.lon - 105.0, tile.lat - 35.0);
	auto dlng = Transform_Lng(tile.lon - 105.0, tile.lat - 35.0);
	auto radlat = tile.lat / 180.0 * std::numbers::pi;
	auto magic = std::sin(radlat);
	magic = 1 - ee_gcj02 * magic * magic;
	auto sqrtmagic = std::sqrt(magic);
	dlat = (dlat * 180.0) / ((a_gcj02 * (1 - ee_gcj02)) / (magic * sqrtmagic) * std::numbers::pi);
	dlng = (dlng * 180.0) / (a_gcj02 / sqrtmagic * std::cos(radlat) * std::numbers::pi);
	auto mglat = tile.lat + dlat;
	auto mglng = tile.lon + dlng;
	return { tile.lon * 2. - mglng, tile.lat * 2. - mglat };
}

DownTool::LonLat DownTool::GaoDeDown::WGS84ToGCJ02(const LonLat& tile)
{
	if (!IsInChina(tile.lon, tile.lat))
	{
		return tile;
	}

	auto dlat = Transform_Lat(tile.lon - 105.0, tile.lat - 35.0);
	auto dlng = Transform_Lng(tile.lon - 105.0, tile.lat - 35.0);
	auto rad_lat = tile.lat / 180.0 * std::numbers::pi;
	auto magic = std::sin(rad_lat);
	magic = 1. - ee_gcj02 * magic * magic;
	auto sqrt_magic = std::sqrt(magic);
	dlat = (dlat * 180.0) / ((a_gcj02 * (1 - ee_gcj02)) / (magic * sqrt_magic) * std::numbers::pi);
	dlng = (dlng * 180.0) / (a_gcj02 / sqrt_magic * std::cos(rad_lat) * std::numbers::pi);
	auto mg_lat = tile.lat + dlat;
	auto mg_lng = tile.lon + dlng;
	return { mg_lng, mg_lat };
}


