#include "BaiduDown.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <system_error>

#include "fmt/format.h"

#include "FileDigest.h"

namespace
{
	constexpr double Pi = 3.141592653589793238462643383279502884;
	constexpr double BaiduCoordinatePi = Pi * 3000. / 180.;
	constexpr uintmax_t BaiduMinImageFileSize = 128;
	constexpr uintmax_t BaiduUnavailableTileSize = 2506;
	constexpr std::array<unsigned char, 16> BaiduUnavailableTileMd5 = {
		0x31, 0x07, 0xA2, 0xAF, 0x9F, 0x8D, 0xF9, 0x75,
		0x56, 0x08, 0x22, 0x28, 0xEF, 0x2A, 0x61, 0xBD
	};

	using BaiduCoeff = std::array<double, 10>;

	constexpr std::array<double, 6> BaiduLlBand = { 75., 60., 45., 30., 15., 0. };
	constexpr std::array<double, 6> BaiduMcBand = {
		12890594.86, 8362377.87, 5591021., 3481989.83, 1678043.12, 0.
	};

	constexpr std::array<BaiduCoeff, 6> BaiduLlToMc = {
		BaiduCoeff{
			-0.0015702102444, 111320.7020616939, 1704480524535203.,
			-10338987376042340., 26112667856603880., -35149669176653700.,
			26595700718403920., -10725012454188240., 1800819912950474., 82.5 },
		BaiduCoeff{
			0.0008277824516172526, 111320.7020463578, 647795574.6671607,
			-4082003173.641316, 10774905663.51142, -15171875531.51559,
			12053065338.62167, -5124939663.577472, 913311935.9512032, 67.5 },
		BaiduCoeff{
			0.00337398766765, 111320.7020202162, 4481351.045890365,
			-23393751.19931662, 79682215.47186455, -115964993.2797253,
			97236711.15602145, -43661946.33752821, 8477230.501135234, 52.5 },
		BaiduCoeff{
			0.00220636496208, 111320.7020209128, 51751.86112841131,
			3796837.749470245, 992013.7397791013, -1221952.21711287,
			1340652.697009075, -620943.6990984312, 144416.9293806241, 37.5 },
		BaiduCoeff{
			-0.0003441963504368392, 111320.7020576856, 278.2353980772752,
			2485758.690035394, 6070.750963243378, 54821.18345352118,
			9540.606633304236, -2710.55326746645, 1405.483844121726, 22.5 },
		BaiduCoeff{
			-0.0003218135878613132, 111320.7020701615, 0.00369383431289,
			823725.6402795718, 0.46104986909093, 2351.343141331292,
			1.58060784298199, 8.77738589078284, 0.37238884252424, 7.45 }
	};

	constexpr std::array<BaiduCoeff, 6> BaiduMcToLl = {
		BaiduCoeff{
			1.410526172116255e-8, 0.00000898305509648872, -1.9939833816331,
			200.9824383106796, -187.2403703815547, 91.6087516669843,
			-23.38765649603339, 2.57121317296198, -0.03801003308653, 17337981.2 },
		BaiduCoeff{
			-7.435856389565537e-9, 0.000008983055097726239, -0.78625201886289,
			96.32687599759846, -1.85204757529826, -59.36935905485877,
			47.40033549296737, -16.50741931063887, 2.28786674699375, 10260144.86 },
		BaiduCoeff{
			-3.030883460898826e-8, 0.00000898305509983578, 0.30071316287616,
			59.74293618442277, 7.357984074871, -25.38371002664745,
			13.45380521110908, -3.29883767235584, 0.32710905363475, 6856817.37 },
		BaiduCoeff{
			-1.981981304930552e-8, 0.000008983055099779535, 0.03278182852591,
			40.31678527705744, 0.65659298677277, -4.44255534477492,
			0.85341911805263, 0.12923347998204, -0.04625736007561, 4482777.06 },
		BaiduCoeff{
			3.09191371068437e-9, 0.000008983055096812155, 0.00006995724062,
			23.10934304144901, -0.00023663490511, -0.6321817810242,
			-0.00663494467273, 0.03430082397953, -0.00466043876332, 2555164.4 },
		BaiduCoeff{
			2.890871144776878e-9, 0.000008983055095805407, -3.068298e-8,
			7.47137025468032, -0.00000353937994, -0.02145144861037,
			-0.00001234426596, 0.00010322952773, -0.00000323890364, 826088.5 }
	};

	double LoopValue(double value, double minValue, double maxValue)
	{
		const double range = maxValue - minValue;
		while (value > maxValue)
		{
			value -= range;
		}
		while (value < minValue)
		{
			value += range;
		}
		return value;
	}

	DownTool::BaiduDown::MercatorPoint ConvertByBaiduCoeff(
		double x,
		double y,
		const BaiduCoeff& coeff)
	{
		const double cB = std::abs(y) / coeff[9];
		double cE = coeff[2] + coeff[3] * cB + coeff[4] * cB * cB +
			coeff[5] * cB * cB * cB + coeff[6] * cB * cB * cB * cB +
			coeff[7] * cB * cB * cB * cB * cB +
			coeff[8] * cB * cB * cB * cB * cB * cB;

		double cT = coeff[0] + coeff[1] * std::abs(x);
		if (x < 0.)
		{
			cT = -cT;
		}
		if (y < 0.)
		{
			cE = -cE;
		}

		return { cT, cE };
	}

	std::optional<size_t> SelectLlToMcCoeffIndex(double latitude)
	{
		for (size_t index = 0; index < BaiduLlBand.size(); index++)
		{
			if (latitude >= BaiduLlBand[index])
			{
				return index;
			}
		}

		for (size_t reverseIndex = BaiduLlBand.size(); reverseIndex > 0; reverseIndex--)
		{
			const size_t index = reverseIndex - 1;
			if (latitude <= -BaiduLlBand[index])
			{
				return index;
			}
		}

		return std::nullopt;
	}

	std::optional<size_t> SelectMcToLlCoeffIndex(double absMercatorY)
	{
		for (size_t index = 0; index < BaiduMcBand.size(); index++)
		{
			if (absMercatorY >= BaiduMcBand[index])
			{
				return index;
			}
		}

		return std::nullopt;
	}
}

DownTool::BaiduDown::BaiduDown()
{
	ServerRange.DomServerRange = DownTool::IntVector2(0, 3);
	SetCachePath("cache/百度地图");
}

std::string DownTool::BaiduDown::MakeUri(const Tile& tile)
{
	return fmt::format(
		"https://maponline{}.bdimg.com/starpic/?qt=satepc&u=x={};y={};z={};v=009;type=sate",
		ServerRange.GetNextServerIndex(),
		tile.X,
		tile.Y,
		tile.Zoom);
}

DownTool::TileRange DownTool::BaiduDown::GetOffsetTiles(const Tile& tile) const
{
	const double tileSize = static_cast<double>(TileSize);
	const std::array<std::pair<double, double>, 4> corners = {
		std::pair<double, double>{ 0., 0. },
		std::pair<double, double>{ tileSize, 0. },
		std::pair<double, double>{ 0., tileSize },
		std::pair<double, double>{ tileSize, tileSize }
	};

	bool hasTile = false;
	int32_t minX = std::numeric_limits<int32_t>::max();
	int32_t maxX = std::numeric_limits<int32_t>::min();
	int32_t minY = std::numeric_limits<int32_t>::max();
	int32_t maxY = std::numeric_limits<int32_t>::min();
	for (const auto& [pixelX, pixelY] : corners)
	{
		LonLat targetLonLat;
		if (!TryGetLonLatForTilePixel(tile, pixelX, pixelY, targetLonLat))
		{
			continue;
		}

		Tile sourceTile;
		double sourcePixelX = 0.;
		double sourcePixelY = 0.;
		const LonLat sourceLonLat = ConvertTargetLonLatToSourceLonLat(targetLonLat);
		if (!TryGetSourceTilePixelForLonLat(
			sourceLonLat, tile.Zoom, sourceTile, sourcePixelX, sourcePixelY))
		{
			continue;
		}

		hasTile = true;
		minX = std::min(minX, sourceTile.X);
		maxX = std::max(maxX, sourceTile.X);
		minY = std::min(minY, sourceTile.Y);
		maxY = std::max(maxY, sourceTile.Y);
	}

	if (!hasTile)
	{
		return { tile, tile };
	}

	return { Tile(minX, minY, tile.Zoom), Tile(maxX, maxY, tile.Zoom) };
}

DownTool::LonLat DownTool::BaiduDown::ConvertTargetLonLatToSourceLonLat(
	const LonLat& lonLat) const
{
	return WGS84ToBD09(lonLat);
}

DownTool::LonLat DownTool::BaiduDown::ConvertSourceLonLatToTargetLonLat(
	const LonLat& lonLat) const
{
	return BD09ToWGS84(lonLat);
}

bool DownTool::BaiduDown::TryGetSourceTilePixelForLonLat(
	const LonLat& lonLat,
	uint32_t zoom,
	Tile& tile,
	double& pixelX,
	double& pixelY) const
{
	return TryGetBaiduTilePixelForBD09LonLat(lonLat, zoom, tile, pixelX, pixelY);
}

bool DownTool::BaiduDown::TryGetSourceLonLatForTilePixel(
	const Tile& tile,
	double pixelX,
	double pixelY,
	LonLat& lonLat) const
{
	return TryGetBD09LonLatForBaiduTilePixel(tile, pixelX, pixelY, lonLat);
}

std::filesystem::path DownTool::BaiduDown::GetTileFilePath(const Tile& tile) const
{
	const std::string filePath =
		fmt::format("{}/{}/{}/{}.png", CachePath, tile.Zoom, tile.X, tile.Y);
	return std::filesystem::u8path(filePath);
}

bool DownTool::BaiduDown::IsUnavailableTileFile(
	const std::filesystem::path& tilePath, const Tile& tile) const
{
	(void)tile;

	std::error_code fileSizeError;
	const uintmax_t tileSize = std::filesystem::file_size(tilePath, fileSizeError);
	if (fileSizeError)
	{
		return false;
	}

	if (tileSize < BaiduMinImageFileSize)
	{
		return true;
	}

	if (tileSize != BaiduUnavailableTileSize)
	{
		return false;
	}

	const std::optional<std::array<unsigned char, 16>> digest =
		CalculateFileMd5(tilePath);
	return digest.has_value() && digest.value() == BaiduUnavailableTileMd5;
}

DownTool::LonLat DownTool::BaiduDown::BD09ToGCJ02(const LonLat& lonLat)
{
	const double x = lonLat.lon - 0.0065;
	const double y = lonLat.lat - 0.006;
	const double z =
		std::sqrt(x * x + y * y) - 0.00002 * std::sin(y * BaiduCoordinatePi);
	const double theta =
		std::atan2(y, x) - 0.000003 * std::cos(x * BaiduCoordinatePi);
	return { z * std::cos(theta), z * std::sin(theta) };
}

DownTool::LonLat DownTool::BaiduDown::GCJ02ToBD09(const LonLat& lonLat)
{
	const double z =
		std::sqrt(lonLat.lon * lonLat.lon + lonLat.lat * lonLat.lat) +
		0.00002 * std::sin(lonLat.lat * BaiduCoordinatePi);
	const double theta =
		std::atan2(lonLat.lat, lonLat.lon) +
		0.000003 * std::cos(lonLat.lon * BaiduCoordinatePi);
	return { z * std::cos(theta) + 0.0065, z * std::sin(theta) + 0.006 };
}

DownTool::LonLat DownTool::BaiduDown::BD09ToWGS84(const LonLat& lonLat)
{
	return GCJ02ToWGS84(BD09ToGCJ02(lonLat));
}

DownTool::LonLat DownTool::BaiduDown::WGS84ToBD09(const LonLat& lonLat)
{
	return GCJ02ToBD09(WGS84ToGCJ02(lonLat));
}

double DownTool::BaiduDown::GetBaiduResolution(uint32_t zoom)
{
	return std::pow(2., static_cast<double>(18) - zoom);
}

DownTool::BaiduDown::MercatorPoint DownTool::BaiduDown::BD09ToMercator(
	const LonLat& lonLat)
{
	const double longitude = LoopValue(lonLat.lon, -180., 180.);
	const double latitude = std::clamp(lonLat.lat, -74., 74.);
	const std::optional<size_t> coeffIndex = SelectLlToMcCoeffIndex(latitude);
	if (!coeffIndex.has_value())
	{
		return {};
	}

	return ConvertByBaiduCoeff(longitude, latitude, BaiduLlToMc[coeffIndex.value()]);
}

DownTool::LonLat DownTool::BaiduDown::MercatorToBD09(const MercatorPoint& point)
{
	const std::optional<size_t> coeffIndex = SelectMcToLlCoeffIndex(std::abs(point.y));
	if (!coeffIndex.has_value())
	{
		return {};
	}

	const MercatorPoint lonLat =
		ConvertByBaiduCoeff(point.x, point.y, BaiduMcToLl[coeffIndex.value()]);
	return { lonLat.x, lonLat.y };
}

bool DownTool::BaiduDown::TryGetBaiduTilePixelForBD09LonLat(
	const LonLat& lonLat,
	uint32_t zoom,
	Tile& tile,
	double& pixelX,
	double& pixelY) const
{
	const double resolution = GetBaiduResolution(zoom);
	if (resolution <= 0.)
	{
		return false;
	}

	const double tileSize = static_cast<double>(TileSize);
	if (tileSize <= 0.)
	{
		return false;
	}

	const MercatorPoint mercator = BD09ToMercator(lonLat);
	const double globalTileX = mercator.x / (tileSize * resolution);
	const double globalTileY = mercator.y / (tileSize * resolution);
	const int32_t tileX = static_cast<int32_t>(std::floor(globalTileX));
	const int32_t tileY = static_cast<int32_t>(std::floor(globalTileY));
	const double tileFractionY = globalTileY - tileY;

	tile = Tile(tileX, tileY, zoom);
	pixelX = (globalTileX - tileX) * tileSize;
	pixelY = std::clamp(
		(1. - tileFractionY) * tileSize,
		0.,
		std::nextafter(tileSize, 0.));
	return std::isfinite(pixelX) && std::isfinite(pixelY);
}

bool DownTool::BaiduDown::TryGetBD09LonLatForBaiduTilePixel(
	const Tile& tile,
	double pixelX,
	double pixelY,
	LonLat& lonLat) const
{
	const double resolution = GetBaiduResolution(tile.Zoom);
	if (resolution <= 0.)
	{
		return false;
	}

	const double tileSize = static_cast<double>(TileSize);
	if (tileSize <= 0.)
	{
		return false;
	}

	const MercatorPoint mercator{
		(static_cast<double>(tile.X) * tileSize + pixelX) * resolution,
		(static_cast<double>(tile.Y) * tileSize + (tileSize - pixelY)) * resolution
	};
	lonLat = MercatorToBD09(mercator);
	return std::isfinite(lonLat.lon) && std::isfinite(lonLat.lat);
}
