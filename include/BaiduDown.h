#pragma once

#include <filesystem>
#include <string>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
	class BaiduDown : public GCJ02GoogleDownBase
	{
	public:

		BaiduDown();

		struct MercatorPoint
		{
			double x = 0.;
			double y = 0.;
		};

		virtual std::string MakeUri(const Tile& tile) override;

		virtual TileRange GetOffsetTiles(const Tile& tile) const override;

		virtual LonLat ConvertTargetLonLatToSourceLonLat(
			const LonLat& lonLat) const override;

		virtual bool TryGetSourceTilePixelForLonLat(
			const LonLat& lonLat,
			uint32_t zoom,
			Tile& tile,
			double& pixelX,
			double& pixelY) const override;

		static LonLat BD09ToGCJ02(const LonLat& lonLat);

		static LonLat GCJ02ToBD09(const LonLat& lonLat);

		static LonLat BD09ToWGS84(const LonLat& lonLat);

		static LonLat WGS84ToBD09(const LonLat& lonLat);

	protected:

		virtual std::filesystem::path GetTileFilePath(const Tile& tile) const override;

		virtual bool IsUnavailableTileFile(
			const std::filesystem::path& tilePath, const Tile& tile) const override;

	private:

		static double GetBaiduResolution(uint32_t zoom);

		static MercatorPoint BD09ToMercator(const LonLat& lonLat);

		static LonLat MercatorToBD09(const MercatorPoint& point);

		bool TryGetBaiduTilePixelForBD09LonLat(
			const LonLat& lonLat,
			uint32_t zoom,
			Tile& tile,
			double& pixelX,
			double& pixelY) const;

		bool TryGetBD09LonLatForBaiduTilePixel(
			const Tile& tile,
			double pixelX,
			double pixelY,
			LonLat& lonLat) const;
	};
}
