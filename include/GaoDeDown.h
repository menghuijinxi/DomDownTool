#pragma once

#include <map>
#include <string>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
	class GaoDeDown : public GCJ02GoogleDownBase
	{
	public:

		GaoDeDown();

	protected:

		virtual bool IsUnavailableTileResponseHeader(
			const std::map<std::string, std::string>& responseHeaders,
			const Tile& tile) const override;
	};
}
