#include "DownToolBase.h"
#include "fmt/format.h"

DownTool::DownToolBase::~DownToolBase()
{
	if (curlMulti)
	{
		curl_multi_cleanup(curlMulti);
		curlMulti = nullptr;
	}
}

void DownTool::DownToolBase::StartDown()
{
	if (bDowning)
	{
		fmt::print("下载中...无法重复进行下载");
		return;
	}

	bDowning = true;

	curlMulti = curl_multi_init();
	CalculateDownRange();

	for (uint32_t x = MinTile.X; x <= MaxTile.X; x++)
	{
		for (uint32_t y = MinTile.Y; y <= MaxTile.Y; y++)
		{
			
		}
	}

	bDowning = false;
}

std::string DownTool::DownToolBase::MakeUri(const Tile& tile)
{
	auto x = fmt::arg("x", tile.X);
	auto y = fmt::arg("y", tile.Y);
	auto z = fmt::arg("z", static_cast<uint32_t>(tile.Zoom));
	auto s = fmt::arg("s", ServerRange.GetNextServerIndex());
	fmt::format_args args = fmt::make_format_args(x, y, z, s);
	return fmt::vformat(UrlTemplate, args);
}

void DownTool::DownToolBase::CalculateDownRange()
{
	GeographicLib::Math::real dummy;
	auto& geo = GeographicLib::Geodesic::WGS84();
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 0., GetRadius(), MaxLonLat.lat, dummy);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 180., GetRadius(), MinLonLat.lat, dummy);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 90., GetRadius(), dummy, MaxLonLat.lon);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 270., GetRadius(), dummy, MinLonLat.lon);

	// 处理经度越界（GeographicLib 通常返回 [-180, 180]）
	if (MinLonLat.lon > MaxLonLat.lon) {
		// 跨越了 ±180° 经线，此时 bounding box 包围极点或环绕地球
		// 简单处理：设为全球（根据需求调整）
		MaxLonLat.lon = 180.0;
		MinLonLat.lon = -180.0;
	}


}
