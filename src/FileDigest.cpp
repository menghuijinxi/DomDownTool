#include "FileDigest.h"

#include <algorithm>
#include <fstream>
#include <memory>

#include "openssl/evp.h"

std::optional<std::array<unsigned char, 16>> DownTool::CalculateFileMd5(
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
		if (readCount > 0 &&
			EVP_DigestUpdate(
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
	if (!digestSuccess || digestLength != 16)
	{
		return std::nullopt;
	}

	std::array<unsigned char, 16> digest{};
	std::copy_n(digestBuffer.begin(), digest.size(), digest.begin());
	return digest;
}
