#pragma once
#include "DownToolBase.h"

namespace DownTool
{
	class GoogleDownBase : public DownToolBase
	{
	public:

		virtual void CalculateDownRange() override;

		static LonLat TileToLonLat(const Tile& tile);

		static Tile LonLatToTile(const LonLat& lonLat, uint32_t zoom);
	};
}