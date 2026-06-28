#include "TencentDown.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

#include "fmt/format.h"
#include "openssl/evp.h"

namespace
{
	constexpr std::array<unsigned char, 16> TencentUnavailableTileMd5 = {
		0x6B, 0x93, 0x8C, 0xB2, 0xCB, 0xA7, 0xAB, 0xD4,
		0xE6, 0x53, 0x21, 0xAF, 0xF4, 0x21, 0x5F, 0x8A
	};

	constexpr uintmax_t TencentUnavailableTileSize = 1226;

	std::optional<std::array<unsigned char, 16>> CalculateFileMd5(
		const std::filesystem::path& filePath)
	{
		std::ifstream fileStream(filePath, std::ios::binary);
		if (!fileStream.is_open())
		{
			return std::nullopt;
		}

		std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
			EVP_MD_CTX_new(), EVP_MD_CTX_free);
		if (!context)
		{
			return std::nullopt;
		}

		if (EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1)
		{
			return std::nullopt;
		}

		std::array<char, 8192> buffer{};
		while (fileStream)
		{
			fileStream.read(buffer.data(), buffer.size());
			const std::streamsize readCount = fileStream.gcount();
			if (readCount > 0
				&& EVP_DigestUpdate(
					context.get(), buffer.data(), static_cast<size_t>(readCount)) != 1)
			{
				return std::nullopt;
			}
		}

		if (!fileStream.eof())
		{
			return std::nullopt;
		}

		std::array<unsigned char, EVP_MAX_MD_SIZE> digestBuffer{};
		unsigned int digestLength = 0;
		const bool digestSuccess =
			EVP_DigestFinal_ex(context.get(), digestBuffer.data(), &digestLength) == 1;
		if (!digestSuccess || digestLength != TencentUnavailableTileMd5.size())
		{
			return std::nullopt;
		}

		std::array<unsigned char, 16> digest{};
		std::copy_n(digestBuffer.begin(), digest.size(), digest.begin());
		return digest;
	}
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

	const std::optional<std::array<unsigned char, 16>> digest = CalculateFileMd5(tilePath);
	return digest.has_value() && digest.value() == TencentUnavailableTileMd5;
}

uint64_t DownTool::TencentDown::ToTencentY(const Tile& tile)
{
	const uint64_t tileCount = uint64_t{ 1 } << tile.Zoom;
	return tileCount - 1 - static_cast<uint64_t>(tile.Y);
}
