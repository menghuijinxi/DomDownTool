#pragma once
#include <numbers>

#include "DownToolBase.h"
#include "GoogleDownBase.h"

namespace DownTool
{
	class GaoDeDown : public GoogleDownBase
	{
	public:

		GaoDeDown();

		virtual bool IsTileOffset() const override {return true;}

		virtual TileRange GetOffsetTiles(const Tile& tile) const override;

		static double Transform_Lat(double lat, double lon);

		static double Transform_Lng(double lng, double lat);

		static const double a_gcj02;
		static const double ee_gcj02;

		//将GCJ02坐标转换为WGS84坐标
		static LonLat GCJ02ToWGS84(const LonLat& tile);

		static LonLat WGS84ToGCJ02(const LonLat& tile);

		static bool IsInChina(double lon, double lat)
		{
			//纬度3.86~53.55, 经度73.66~135.05
			if (73.66 < lon && lon < 135.05 && 3.86 < lat && lat < 53.55)
			{
				return true;
			}

			return false;
		}
	};
}
