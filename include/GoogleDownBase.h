#pragma once
#include "DownToolBase.h"

namespace DownTool
{
	class GoogleDownBase : public DownToolBase
	{
	public:

		virtual void CalculateDownRange() override;

		virtual bool TryGetLonLatForTilePixel(
			const Tile& tile, double pixelX, double pixelY, LonLat& lonLat) const override;

		virtual bool TryGetTilePixelForLonLat(
			const LonLat& lonLat,
			uint32_t zoom,
			Tile& tile,
			double& pixelX,
			double& pixelY) const override;

		static LonLat TileToLonLat(const Tile& tile);

		static Tile LonLatToTile(const LonLat& lonLat, uint32_t zoom);
	};
}
