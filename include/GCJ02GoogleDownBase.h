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

		virtual bool CanCopyPostProcessTileDirectly() const override { return false; }

		virtual LonLat ConvertTargetLonLatToSourceLonLat(
			const LonLat& lonLat) const override;

		virtual LonLat ConvertSourceLonLatToTargetLonLat(
			const LonLat& lonLat) const override;

		static double Transform_Lat(double lng, double lat);

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
