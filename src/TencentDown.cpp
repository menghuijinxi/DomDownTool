#include "TencentDown.h"

#include <array>
#include <filesystem>
#include <optional>
#include <system_error>

#include "fmt/format.h"

#include "FileDigest.h"

namespace
{
	constexpr std::array<unsigned char, 16> TencentUnavailableTileMd5 = {
		0x6B, 0x93, 0x8C, 0xB2, 0xCB, 0xA7, 0xAB, 0xD4,
		0xE6, 0x53, 0x21, 0xAF, 0xF4, 0x21, 0x5F, 0x8A
	};

	constexpr uintmax_t TencentUnavailableTileSize = 1226;
}

DownTool::TencentDown::TencentDown()
{
	ServerRange.DomServerRange = DownTool::IntVector2(0, 3);
	SetCachePath("cache/腾讯地图");
}

std::string DownTool::TencentDown::MakeUri(const Tile& tile)
{
	const uint64_t tencentY = ToTencentY(tile);
	const uint64_t tileX = static_cast<uint64_t>(tile.X);
	const uint64_t x16 = tileX / 16;
	const uint64_t y16 = tencentY / 16;

	return fmt::format(
		"https://p{}.map.gtimg.com/sateTiles/{}/{}/{}/{}_{}.png",
		ServerRange.GetNextServerIndex(),
		static_cast<uint32_t>(tile.Zoom),
		x16,
		y16,
		tileX,
		tencentY);
}

bool DownTool::TencentDown::IsUnavailableTileFile(
	const std::filesystem::path& tilePath, const Tile& tile) const
{
	(void)tile;

	std::error_code fileSizeError;
	const uintmax_t tileSize = std::filesystem::file_size(tilePath, fileSizeError);
	if (fileSizeError)
	{
		return false;
	}

	if (tileSize != TencentUnavailableTileSize)
	{
		return false;
	}

	const std::optional<std::array<unsigned char, 16>> digest =
		CalculateFileMd5(tilePath);
	return digest.has_value() && digest.value() == TencentUnavailableTileMd5;
}

uint64_t DownTool::TencentDown::ToTencentY(const Tile& tile)
{
	const uint64_t tileCount = uint64_t{ 1 } << tile.Zoom;
	return tileCount - 1 - static_cast<uint64_t>(tile.Y);
}
