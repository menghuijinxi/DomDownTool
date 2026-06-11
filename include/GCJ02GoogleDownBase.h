#pragma once
#include <numbers>

#include "GoogleDownBase.h"

namespace DownTool
{
	class GCJ02GoogleDownBase : public GoogleDownBase
	{
	public:

		virtual bool IsTileOffset() const override { return true; }

		virtual TileRange GetOffsetTiles(const Tile& tile) const override;

		static double Transform_Lat(double lat, double lon);

		static double Transform_Lng(double lng, double lat);

		static const double a_gcj02;
		static const double ee_gcj02;

		static LonLat GCJ02ToWGS84(const LonLat& tile);

		static LonLat WGS84ToGCJ02(const LonLat& tile);

		static bool IsInChina(double lon, double lat)
		{
			if (73.66 < lon && lon < 135.05 && 3.86 < lat && lat < 53.55)
			{
				return true;
			}

			return false;
		}
	};
}
