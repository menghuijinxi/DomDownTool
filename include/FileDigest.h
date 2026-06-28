#pragma once

#include <array>
#include <filesystem>
#include <optional>

namespace DownTool
{
	std::optional<std::array<unsigned char, 16>> CalculateFileMd5(
		const std::filesystem::path& filePath);
}
