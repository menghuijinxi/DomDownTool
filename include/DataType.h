#pragma once
#include <cstdint>
#include <boost/preprocessor.hpp>

namespace DownTool
{
#define GEN_GET_NAME(var) Get##var
#define GEN_SET_NAME(var) Set##var

#define GEN_GETSET(r, data, var) \
    const decltype(var)& GEN_GET_NAME(var)() { return var; } \
    void GEN_SET_NAME(var)(const decltype(var)& value) { var = value; }

#define GEN_GETSET_FOR(...) \
    BOOST_PP_SEQ_FOR_EACH(GEN_GETSET, ～, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

	struct LonLat
	{
		LonLat() = default;

		LonLat(double newLon, double newLat)
			: lon(newLon), lat(newLat)
		{

		}
		double lon = 0.;
		double lat = 0.;
	};

	struct IntVector2
	{
		IntVector2() = default;

		IntVector2(int32_t newX, int32_t newY)
			: X(newX), Y(newY)
		{
		}

		int32_t X = 0;
		int32_t Y = 0;
	};

	struct Tile
	{
		Tile() = default;
		Tile(uint32_t newX, uint32_t newY, uint32_t newZoom)
			: X(newX), Y(newY), Zoom(newZoom)
		{
		}

		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Zoom = 0;
	};

	struct TileRange
	{
		TileRange() = default;
		TileRange(const Tile& minTile, const Tile& maxTile)
			: MinTile(minTile), MaxTile(maxTile)
		{
		}
		Tile MinTile;
		Tile MaxTile;
	};

	enum class FbxPlanetShape
	{
		Plane,
		Sphere,
		Both
	};

	struct FbxTextureDistanceOffset
	{
		FbxTextureDistanceOffset() = default;

		FbxTextureDistanceOffset(double newDistance, uint32_t newOffset)
			: Distance(newDistance), Offset(newOffset)
		{
		}

		//单位厘米。距离项目中心超过该值时应用 Offset。
		double Distance = 0.;

		//贴图降级级别，1 表示边长减半。
		uint32_t Offset = 0;
	};

	struct GetDownServerIndex
	{
		GetDownServerIndex() = default;
		GetDownServerIndex(const IntVector2& range)
			: DomServerRange(range)
		{
			currentDomServerIndex = range.X - 1;
		}

		std::atomic<int32_t> currentDomServerIndex = 0;
		IntVector2 DomServerRange = IntVector2(1, 4);

		int32_t GetNextServerIndex()
		{
			int32_t index = currentDomServerIndex.fetch_add(1);
			return index % (DomServerRange.Y - DomServerRange.X + 1) + DomServerRange.X;
		}
	};
}
