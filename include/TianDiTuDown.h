#pragma once
#include <map>
#include <string>
#include <vector>

#include "GoogleDownBase.h"

namespace DownTool
{
	class TianDiTuDown : public GoogleDownBase
	{
	public:

		TianDiTuDown();

	protected:

		virtual bool IsUnavailableTileResponseHeader(
			const std::map<std::string, std::string>& responseHeaders,
			const Tile& tile) const override;

		virtual std::vector<std::string> GetRequestHeaders(
			const Tile& tile) const override;
	};
}
