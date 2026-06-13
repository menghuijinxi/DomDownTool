#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
	class TencentDown : public GCJ02GoogleDownBase
	{
	public:

		TencentDown();

		virtual std::string MakeUri(const Tile& tile) override;

	protected:

		virtual bool IsUnavailableTileFile(
			const std::filesystem::path& tilePath, const Tile& tile) const override;

	private:

		static uint64_t ToTencentY(const Tile& tile);
	};
}
