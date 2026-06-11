#pragma once

#include <cstdint>
#include <string>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
	class TencentDown : public GCJ02GoogleDownBase
	{
	public:

		TencentDown();

		virtual std::string MakeUri(const Tile& tile) override;

	private:

		static uint64_t ToTencentY(const Tile& tile);
	};
}
