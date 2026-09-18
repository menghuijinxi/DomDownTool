#include "TianDiTuMultidateDown.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "curl/curl.h"
#include "fmt/format.h"
#include "nlohmann/json.hpp"

namespace
{
    using Json = nlohmann::json;

	constexpr double MinLatitude = -90.;
	constexpr double MaxLatitude = 90.;
	constexpr double MinLongitude = -180.;
	constexpr double MaxLongitude = 180.;
	constexpr const char* TianDiTuMultidateToken = "b88bfb160c81dab8d9d20aaa74846360";
	constexpr const char* TianDiTuMultidateHost = "https://image.tianditu.gov.cn";
	constexpr const char* TianDiTuMapHost = "https://map.tianditu.gov.cn";
	constexpr unsigned char TransparentPngBytes[] = {
		0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
		0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
		0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
		0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
		0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
		0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

	bool WriteTransparentPng(const std::filesystem::path& path)
	{
		std::error_code directoryError;
		std::filesystem::create_directories(path.parent_path(), directoryError);
		if (directoryError)
		{
			return false;
		}

		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			return false;
		}

		output.write(
			reinterpret_cast<const char*>(TransparentPngBytes),
			static_cast<std::streamsize>(sizeof(TransparentPngBytes)));
		return output.good();
	}

	bool IsTransparentPlaceholderPng(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			return false;
		}

		std::array<unsigned char, sizeof(TransparentPngBytes)> bytes{};
		input.read(
			reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
		{
			return false;
		}

		char extraByte = 0;
		if (input.read(&extraByte, 1))
		{
			return false;
		}

		return std::equal(bytes.begin(), bytes.end(), TransparentPngBytes);
	}

	std::string SanitizeCacheLayerComponent(std::string value)
	{
		if (value.empty())
		{
			return "unknown";
		}

		for (char& character : value)
		{
			const unsigned char byte = static_cast<unsigned char>(character);
			if (byte < 32 || character == '<' || character == '>' || character == ':' ||
				character == '"' || character == '/' || character == '\\' || character == '|' ||
				character == '?' || character == '*')
			{
				character = '_';
			}
		}
		return value;
	}

	uint64_t HashCacheLayerText(std::string_view year, std::string_view key,
		std::string_view gbCode, std::string_view regionName)
	{
		uint64_t hash = 1469598103934665603ull;
		auto append = [&hash](std::string_view text)
		{
			for (const unsigned char character : text)
			{
				hash ^= static_cast<uint64_t>(character);
				hash *= 1099511628211ull;
			}
			hash ^= static_cast<uint64_t>('|');
			hash *= 1099511628211ull;
		};

		append(year);
		append(key);
		append(gbCode);
		append(regionName);
		return hash;
	}

    std::string GetJsonString(const Json& object, const char* fieldName)
    {
        if (!object.is_object())
        {
            return {};
        }
        const auto iterator = object.find(fieldName);
        return iterator != object.end() && iterator->is_string()
            ? iterator->get<std::string>()
            : std::string{};
    }

    int GetJsonInteger(const Json& object, const char* fieldName, int defaultValue)
    {
        if (!object.is_object())
        {
            return defaultValue;
        }
        const auto iterator = object.find(fieldName);
        if (iterator == object.end() || !iterator->is_number())
        {
            return defaultValue;
        }
        const double value = iterator->get<double>();
        if (!std::isfinite(value) || value < std::numeric_limits<int>::lowest() ||
            value > std::numeric_limits<int>::max())
        {
            return defaultValue;
        }
        return static_cast<int>(value);
    }

	struct HttpResponse
	{
		long statusCode = 0;
		std::string body;
		std::string effectiveUrl;
		std::map<std::string, std::string> headers;
	};

	size_t WriteHttpResponseData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		std::string* body = static_cast<std::string*>(userdata);
		const size_t totalSize = size * nmemb;
		body->append(static_cast<const char*>(ptr), totalSize);
		return totalSize;
	}

	std::string ToLowerAscii(std::string text)
	{
		for (char& character : text)
		{
			character = static_cast<char>(
				std::tolower(static_cast<unsigned char>(character)));
		}

		return text;
	}

	std::string TrimAscii(std::string text)
	{
		const size_t begin = text.find_first_not_of(" \t\r\n");
		if (begin == std::string::npos)
		{
			return "";
		}

		const size_t end = text.find_last_not_of(" \t\r\n");
		return text.substr(begin, end - begin + 1);
	}

	size_t WriteHttpHeaderData(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		HttpResponse* response = static_cast<HttpResponse*>(userdata);
		const size_t totalSize = size * nmemb;
		std::string headerLine(ptr, totalSize);
		headerLine = TrimAscii(headerLine);
		const size_t separatorIndex = headerLine.find(':');
		if (separatorIndex == std::string::npos)
		{
			return totalSize;
		}

		std::string name = TrimAscii(headerLine.substr(0, separatorIndex));
		std::string value = TrimAscii(headerLine.substr(separatorIndex + 1));
		if (!name.empty())
		{
			response->headers[ToLowerAscii(name)] = value;
		}

		return totalSize;
	}

	std::string GetResponseHeader(
		const HttpResponse& response,
		const std::string& headerName)
	{
		const auto iterator = response.headers.find(ToLowerAscii(headerName));
		if (iterator == response.headers.end())
		{
			return "";
		}

		return iterator->second;
	}

	std::string BuildBodyPreview(const std::string& body)
	{
		constexpr size_t previewLimit = 300;
		std::string preview = body.substr(0, std::min(body.size(), previewLimit));
		for (char& character : preview)
		{
			if (character == '\r' || character == '\n' || character == '\t')
			{
				character = ' ';
			}
		}

		return TrimAscii(preview);
	}

	std::string BuildAvailableYearText(
		const std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo>& records)
	{
		std::set<std::string> years;
		for (const DownTool::TianDiTuMultidateDown::TimelineInfo& record : records)
		{
			if (!record.year.empty())
			{
				years.insert(record.year);
			}
		}

		std::string text;
		for (const std::string& year : years)
		{
			if (!text.empty())
			{
				text += ", ";
			}

			text += year;
		}

		return text.empty() ? "无" : text;
	}

	std::string UrlEncode(const std::string& text)
	{
		constexpr char hexDigits[] = "0123456789ABCDEF";
		std::string encoded;
		encoded.reserve(text.size());
		for (const unsigned char character : text)
		{
			const bool isUnreserved =
				(character >= 'A' && character <= 'Z') ||
				(character >= 'a' && character <= 'z') ||
				(character >= '0' && character <= '9') ||
				character == '-' || character == '_' ||
				character == '.' || character == '~';
			if (isUnreserved)
			{
				encoded.push_back(static_cast<char>(character));
				continue;
			}

			encoded.push_back('%');
			encoded.push_back(hexDigits[character >> 4]);
			encoded.push_back(hexDigits[character & 0x0F]);
		}

		return encoded;
	}

	std::string ExtractYear(const std::string& time)
	{
		std::string year;
		for (const char character : time)
		{
			if (std::isdigit(static_cast<unsigned char>(character)) == 0)
			{
				continue;
			}

			year.push_back(character);
			if (year.size() == 4)
			{
				break;
			}
		}

		return year.size() == 4 ? year : "";
	}

	std::string ExtractSortableDate(const std::string& time)
	{
		std::string sortableDate;
		for (const char character : time)
		{
			if (std::isdigit(static_cast<unsigned char>(character)) != 0)
			{
				sortableDate.push_back(character);
			}
		}

		return sortableDate;
	}

	uint32_t GetRegionLevel(uint32_t zoom)
	{
		if (zoom < 6)
		{
			return 2;
		}

		if (zoom < 11)
		{
			return 3;
		}

		return 4;
	}

	const Json* FindTimelineRecordsJson(const Json& root)
	{
		if (root.is_array())
		{
			return &root;
		}
		if (!root.is_object())
		{
			return nullptr;
		}

		const auto data = root.find("data");
		if (data == root.end())
		{
			return nullptr;
		}
		if (data->is_array())
		{
			return &*data;
		}
		if (!data->is_object())
		{
			return nullptr;
		}

		const auto records = data->find("records");
		if (records != data->end() && records->is_array())
		{
			return &*records;
		}

		const auto page = data->find("page");
		if (page == data->end() || !page->is_object())
		{
			return nullptr;
		}
		const auto pageRecords = page->find("records");
		return pageRecords != page->end() && pageRecords->is_array() ? &*pageRecords : nullptr;
	}

	std::string DecodeTianDiTuGetRegionBody(const std::string& responseBody)
	{
		constexpr size_t encodedCharacterSize = 4;
		if (responseBody.empty() || responseBody.size() % encodedCharacterSize != 0)
		{
			return "";
		}

		std::string decoded;
		decoded.reserve(responseBody.size() / encodedCharacterSize);
		for (size_t offset = 0; offset + 3 < responseBody.size(); offset += encodedCharacterSize)
		{
			uint32_t packedValue = 0;
			for (size_t byteIndex = 0; byteIndex < encodedCharacterSize; ++byteIndex)
			{
				const auto character = static_cast<unsigned char>(responseBody[offset + byteIndex]);
				packedValue |= static_cast<uint32_t>(character) << (8 * (3 - byteIndex));
			}

			decoded.push_back(static_cast<char>((packedValue >> 2) & 0xFF));
		}

		return decoded;
	}

	bool HttpGet(
		const std::string& url,
		const std::vector<std::string>& headers,
		HttpResponse& response)
	{
		CURL* curl = curl_easy_init();
		if (!curl)
		{
			return false;
		}

		curl_slist* requestHeaders = nullptr;
		for (const std::string& header : headers)
		{
			curl_slist* nextHeaders = curl_slist_append(requestHeaders, header.c_str());
			if (!nextHeaders)
			{
				curl_slist_free_all(requestHeaders);
				curl_easy_cleanup(curl);
				return false;
			}
			requestHeaders = nextHeaders;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteHttpResponseData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteHttpHeaderData);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, requestHeaders);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		const CURLcode result = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
		char* effectiveUrl = nullptr;
		curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
		if (effectiveUrl)
		{
			response.effectiveUrl = effectiveUrl;
		}

		curl_slist_free_all(requestHeaders);
		curl_easy_cleanup(curl);
		return result == CURLE_OK;
	}
}

DownTool::TianDiTuMultidateDown::TianDiTuMultidateDown()
{
	SetLatestWmtsUrlTemplate();
	ServerRange.DomServerRange = DownTool::IntVector2(0, 7);
	SetCachePath("cache/天地图多时相");
}

std::string DownTool::TianDiTuMultidateDown::BuildCacheLayerName(
	std::string_view year,
	std::string_view key,
	std::string_view gbCode,
	std::string_view regionName)
{
	if (key.empty())
	{
		return "latest";
	}

	const std::string sanitizedYear = SanitizeCacheLayerComponent(
		std::string(year.empty() ? std::string_view("timeline") : year));
	const std::string sanitizedGbCode = SanitizeCacheLayerComponent(std::string(gbCode));
	return fmt::format(
		"{}_{}_{:016x}",
		sanitizedYear,
		sanitizedGbCode,
		HashCacheLayerText(year, key, gbCode, regionName));
}

void DownTool::TianDiTuMultidateDown::StartDown()
{
	CalculateDownRange();
	if (!PrepareMultidateLayer())
	{
		return;
	}

	const std::string originalCachePath = CachePath;
	CachePath = fmt::format("{}/{}", originalCachePath, GetLayerCacheName());
	fmt::print("多时相天地图：下载年份：{}\n", GetLayerCacheName());
	if (bTransparentPlaceholderForEmptyTiles && !bUseLatestYear)
	{
		StartDownWithTransparentEmptyTiles();
	}
	else
	{
		TianDiTuDown::StartDown();
	}
	CachePath = originalCachePath;
}

void DownTool::TianDiTuMultidateDown::StartDownWithTransparentEmptyTiles()
{
	std::vector<Tile> targetTiles;
	BuildTargetTiles(targetTiles);
	const uint32_t stalePlaceholderCount = RemoveTransparentPlaceholderTiles(targetTiles);
	if (stalePlaceholderCount != 0)
	{
		fmt::print("多时相天地图：{} 个历史透明占位已清理并重新请求。\n",
			stalePlaceholderCount);
	}

	const bool postProcessEnabled = GetPostProcessEnabled();
	const bool postProcessMergedImageEnabled = GetPostProcessMergedImageEnabled();
	const bool postProcessFbxEnabled = GetPostProcessFbxEnabled();
	SetPostProcessEnabled(false);
	SetPostProcessMergedImageEnabled(false);
	SetPostProcessFbxEnabled(false);
	TianDiTuDown::StartDown();
	SetPostProcessEnabled(postProcessEnabled);
	SetPostProcessMergedImageEnabled(postProcessMergedImageEnabled);
	SetPostProcessFbxEnabled(postProcessFbxEnabled);

	if (IsStopRequested())
	{
		return;
	}

	const uint32_t placeholderCount =
		ReplaceUnavailableTilesWithTransparentPlaceholders(targetTiles);
	if (placeholderCount != 0)
	{
		fmt::print("多时相天地图：{} 个历史覆盖不可用瓦片已转为透明占位。\n",
			placeholderCount);
	}

	std::vector<Tile> failedTiles = ScanCacheAndCollectPendingTiles(
		targetTiles, "透明占位后缓存检测", false, true);
	if (!failedTiles.empty())
	{
		fmt::print("多时相天地图：透明占位后仍有 {} 个瓦片不可用，跳过后处理。\n",
			failedTiles.size());
		return;
	}

	if (postProcessEnabled || postProcessMergedImageEnabled || postProcessFbxEnabled)
	{
		RunPostProcess();
	}
}

uint32_t DownTool::TianDiTuMultidateDown::RemoveTransparentPlaceholderTiles(
	const std::vector<Tile>& targetTiles) const
{
	uint32_t removedCount = 0;
	for (const Tile& tile : targetTiles)
	{
		if (IsStopRequested())
		{
			break;
		}

		const std::filesystem::path tilePath = GetTileFilePath(tile);
		if (!IsTransparentPlaceholderPng(tilePath))
		{
			continue;
		}

		std::error_code removeError;
		std::filesystem::remove(tilePath, removeError);
		if (!removeError)
		{
			removedCount++;
		}
	}
	return removedCount;
}

uint32_t DownTool::TianDiTuMultidateDown::ReplaceUnavailableTilesWithTransparentPlaceholders(
	const std::vector<Tile>& targetTiles) const
{
	uint32_t placeholderCount = 0;
	for (const Tile& tile : targetTiles)
	{
		if (IsStopRequested())
		{
			break;
		}

		const std::filesystem::path tilePath = GetTileFilePath(tile);
		if (GetTileCacheState(tile, false) != TileCacheState::Ready &&
			WriteTransparentPng(tilePath))
		{
			placeholderCount++;
		}
	}
	return placeholderCount;
}

DownTool::TileRange DownTool::TianDiTuMultidateDown::GetOffsetTiles(
	const Tile& tile) const
{
	const double tileSize = static_cast<double>(TileSize);
	const std::array<std::pair<double, double>, 4> corners = {
		std::pair<double, double>{ 0., 0. },
		std::pair<double, double>{ tileSize, 0. },
		std::pair<double, double>{ 0., tileSize },
		std::pair<double, double>{ tileSize, tileSize }
	};

	bool hasTile = false;
	int32_t minX = std::numeric_limits<int32_t>::max();
	int32_t maxX = std::numeric_limits<int32_t>::min();
	int32_t minY = std::numeric_limits<int32_t>::max();
	int32_t maxY = std::numeric_limits<int32_t>::min();
	for (const auto& [pixelX, pixelY] : corners)
	{
		LonLat targetLonLat;
		if (!TryGetLonLatForTilePixel(tile, pixelX, pixelY, targetLonLat))
		{
			continue;
		}

		Tile sourceTile;
		double sourcePixelX = 0.;
		double sourcePixelY = 0.;
		const LonLat sourceLonLat = ConvertTargetLonLatToSourceLonLat(targetLonLat);
		if (!TryGetSourceTilePixelForLonLat(
			sourceLonLat, tile.Zoom, sourceTile, sourcePixelX, sourcePixelY))
		{
			continue;
		}

		hasTile = true;
		minX = std::min(minX, sourceTile.X);
		maxX = std::max(maxX, sourceTile.X);
		minY = std::min(minY, sourceTile.Y);
		maxY = std::max(maxY, sourceTile.Y);
	}

	if (!hasTile)
	{
		return { tile, tile };
	}

	return { Tile(minX, minY, tile.Zoom), Tile(maxX, maxY, tile.Zoom) };
}

bool DownTool::TianDiTuMultidateDown::TryGetSourceTilePixelForLonLat(
	const LonLat& lonLat,
	uint32_t zoom,
	Tile& tile,
	double& pixelX,
	double& pixelY) const
{
	return TryGetGeographicTilePixelForLonLat(lonLat, zoom, tile, pixelX, pixelY);
}

bool DownTool::TianDiTuMultidateDown::TryGetSourceLonLatForTilePixel(
	const Tile& tile,
	double pixelX,
	double pixelY,
	LonLat& lonLat) const
{
	const double tileSize = static_cast<double>(TileSize);
	if (tileSize <= 0. || tile.Zoom == 0 || !std::isfinite(pixelX) ||
		!std::isfinite(pixelY))
	{
		return false;
	}

	const double rowCount = std::pow(2., static_cast<double>(tile.Zoom) - 1.);
	const double globalX = static_cast<double>(tile.X) + pixelX / tileSize;
	const double globalY = static_cast<double>(tile.Y) + pixelY / tileSize;
	if (!std::isfinite(globalX) || !std::isfinite(globalY))
	{
		return false;
	}

	lonLat.lon = globalX / rowCount * 180. - 180.;
	lonLat.lat = (rowCount - globalY) / rowCount * 180. - 90.;
	return std::isfinite(lonLat.lon) && std::isfinite(lonLat.lat);
}

bool DownTool::TianDiTuMultidateDown::TryGetSourceTileMatrixSize(
	uint32_t zoom,
	uint32_t& columnCount,
	uint32_t& rowCount) const
{
	if (zoom == 0 || zoom >= 32)
	{
		return false;
	}

	rowCount = uint32_t{ 1 } << (zoom - 1);
	columnCount = rowCount << 1;
	return columnCount != 0 && rowCount != 0;
}

std::vector<std::string> DownTool::TianDiTuMultidateDown::GetRequestHeaders(
	const Tile& tile) const
{
	(void)tile;

	std::vector<std::string> headers = {
		"Referer: https://image.tianditu.gov.cn/",
		"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
			"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/149.0.0.0 "
			"Safari/537.36 Edg/149.0.0.0"
	};

	if (!SessionCookie.empty())
	{
		headers.push_back(fmt::format("Cookie: {}", SessionCookie));
	}

	return headers;
}

void DownTool::TianDiTuMultidateDown::SetYear(const std::string& year)
{
	if (year.empty())
	{
		UseLatestYear();
		return;
	}

	SelectedYear = year;
	ResolvedYear.clear();
	ResolvedKey.clear();
	bUseLatestYear = false;
	bHasPresetYearKey = false;
	bForceLatestWmtsOnly = false;
	SetLatestWmtsUrlTemplate();
}

void DownTool::TianDiTuMultidateDown::UseLatestYear()
{
	SelectedYear.clear();
	ResolvedYear.clear();
	ResolvedKey.clear();
	bUseLatestYear = true;
	bHasPresetYearKey = false;
	bForceLatestWmtsOnly = false;
	SetLatestWmtsUrlTemplate();
}

void DownTool::TianDiTuMultidateDown::UseLatestWmtsOnly()
{
	SelectedYear.clear();
	ResolvedYear.clear();
	ResolvedKey.clear();
	bUseLatestYear = true;
	bHasPresetYearKey = false;
	bForceLatestWmtsOnly = true;
	SetLatestWmtsUrlTemplate();
}

void DownTool::TianDiTuMultidateDown::DownloadYear(const std::string& year)
{
	SetYear(year);
	StartDown();
}

std::vector<std::string> DownTool::TianDiTuMultidateDown::ResolveAvailableYears()
{
	std::set<std::string> uniqueYears;
	for (const TimelineInfo& record : ResolveAvailableTimelines())
	{
		if (!record.year.empty())
		{
			uniqueYears.insert(record.year);
		}
	}

	return { uniqueYears.begin(), uniqueYears.end() };
}

std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo>
DownTool::TianDiTuMultidateDown::ResolveAvailableTimelines()
{
	CalculateDownRange();
	if (!EnsureRegion())
	{
		return {};
	}

	std::vector<TimelineInfo> records;
	if (!FetchTimelineRecords(records))
	{
		return {};
	}

	return records;
}

std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo>
DownTool::TianDiTuMultidateDown::ResolveAvailableTimelinesForBounds(
	const LonLat& minLonLat,
	const LonLat& maxLonLat,
	uint32_t zoom)
{
	if (!PrepareTimelineQueryForBounds(minLonLat, maxLonLat, zoom))
	{
		return {};
	}

	std::vector<TimelineInfo> records;
	if (!FetchTimelineRecords(records))
	{
		return {};
	}

	return records;
}

bool DownTool::TianDiTuMultidateDown::PrepareTimelineQueryForBounds(
	const LonLat& minLonLat,
	const LonLat& maxLonLat,
	uint32_t zoom)
{
	MinLonLat = minLonLat;
	MaxLonLat = maxLonLat;
	Zoom = zoom;
	return EnsureRegion();
}

std::string DownTool::TianDiTuMultidateDown::GetTimelineListUrl() const
{
	return BuildTimelineListUrl();
}

bool DownTool::TianDiTuMultidateDown::TryParseTimelineRecords(
	const std::string& responseBody,
	std::vector<TimelineInfo>& records) const
{
	return ParseTimelineRecords(responseBody, records);
}

void DownTool::TianDiTuMultidateDown::SetMultidateRegion(
	const std::string& gbCode,
	const std::string& name)
{
	RegionGbCode = gbCode;
	RegionName = name;
	if (!ResolvedKey.empty())
	{
		SetTimelineTileUrlTemplate();
	}
}

void DownTool::TianDiTuMultidateDown::SetSessionCookie(const std::string& cookie)
{
	SessionCookie = cookie;
}

void DownTool::TianDiTuMultidateDown::SetYearKey(
	const std::string& year,
	const std::string& key)
{
	SelectedYear = year;
	ResolvedYear = year;
	ResolvedKey = key;
	bUseLatestYear = false;
	bHasPresetYearKey = !year.empty() && !key.empty();
	bForceLatestWmtsOnly = false;
	SetTimelineTileUrlTemplate();
}

void DownTool::TianDiTuMultidateDown::SetLatestWmtsUrlTemplate()
{
	SetUrlTemplate(fmt::format(
		"https://t{}.tianditu.gov.cn/img_c/wmts?"
		"SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=img&STYLE=default&"
		"TILEMATRIXSET=c&FORMAT=tiles&TILEMATRIX={}&TILEROW={}&TILECOL={}&"
		"tk={}",
		serverIndexReplace,
		zReplace,
		yReplace,
		xReplace,
		TianDiTuMultidateToken));
}

void DownTool::TianDiTuMultidateDown::SetTimelineTileUrlTemplate()
{
	if (ResolvedKey.empty())
	{
		return;
	}

	SetUrlTemplate(fmt::format(
		"{}/api/multidate/basic/tile?x={}&y={}&z={}&key={}&gb={}&name={}",
		TianDiTuMultidateHost,
		xReplace,
		yReplace,
		zReplace,
		UrlEncode(ResolvedKey),
		UrlEncode(RegionGbCode),
		UrlEncode(RegionName)));
}

bool DownTool::TianDiTuMultidateDown::PrepareMultidateLayer()
{
	SetLatestWmtsUrlTemplate();
	if (bForceLatestWmtsOnly)
	{
		return true;
	}

	if (bHasPresetYearKey && !ResolvedKey.empty())
	{
		if (!EnsureRegion())
		{
			fmt::print("多时相天地图：指定年份 {} 已有 key，但无法获取区域参数。\n",
				ResolvedYear);
			return false;
		}

		SetTimelineTileUrlTemplate();
		return true;
	}

	ResolvedYear.clear();
	ResolvedKey.clear();
	if (!EnsureRegion())
	{
		if (bUseLatestYear)
		{
			fmt::print("多时相天地图：无法获取区域参数，改用最新 WMTS 影像。\n");
			return true;
		}

		fmt::print("多时相天地图：无法获取区域参数，不能解析年份 {}。\n", SelectedYear);
		return false;
	}

	std::vector<TimelineInfo> records;
	if (!FetchTimelineRecords(records))
	{
		if (bUseLatestYear)
		{
			fmt::print("多时相天地图：时间线接口不可用，改用最新 WMTS 影像。\n");
			return true;
		}

		fmt::print("多时相天地图：时间线接口不可用，不能解析年份 {}。\n", SelectedYear);
		return false;
	}

	TimelineInfo selectedRecord;
	if (!SelectTimelineRecord(records, selectedRecord))
	{
		if (bUseLatestYear)
		{
			fmt::print("多时相天地图：当前范围没有可用时间线，改用最新 WMTS 影像。\n");
			return true;
		}

		fmt::print("多时相天地图：当前范围没有年份 {} 的影像。\n", SelectedYear);
		return false;
	}

	ResolvedYear = selectedRecord.year;
	ResolvedKey = selectedRecord.key;
	SetTimelineTileUrlTemplate();
	fmt::print("多时相天地图：使用 {} 年影像，key={}\n", ResolvedYear, ResolvedKey);
	return true;
}

bool DownTool::TianDiTuMultidateDown::EnsureRegion()
{
	if (!RegionGbCode.empty())
	{
		return true;
	}

	return TryResolveRegionFromCenter();
}

bool DownTool::TianDiTuMultidateDown::FetchTimelineRecords(
	std::vector<TimelineInfo>& records)
{
	const std::string url = BuildTimelineListUrl();
	HttpResponse response;
	if (!HttpGet(url, GetRequestHeaders({}), response))
	{
		fmt::print("多时相天地图：时间线请求失败，url={}\n", url);
		return false;
	}

	if (response.statusCode != 200)
	{
		const std::string location = GetResponseHeader(response, "location");
		if (location.empty())
		{
			fmt::print("多时相天地图：时间线接口 HTTP {}，url={}\n",
				response.statusCode,
				url);
		}
		else
		{
			fmt::print("多时相天地图：时间线接口 HTTP {}，Location={}\n",
				response.statusCode,
				location);
		}

		if (location.find("oauth.tianditu.gov.cn/login") != std::string::npos)
		{
			fmt::print("多时相天地图：时间线接口需要登录态，自动解析年份前请调用 SetSessionCookie() 传入浏览器 Cookie。\n");
		}

		return false;
	}

	if (!ParseTimelineRecords(response.body, records))
	{
		const std::string contentType = GetResponseHeader(response, "content-type");
		fmt::print("多时相天地图：时间线接口返回内容无法解析，Content-Type={}，body={}\n",
			contentType.empty() ? "unknown" : contentType,
			BuildBodyPreview(response.body));
		return false;
	}

	fmt::print("多时相天地图：解析到 {} 个时间线记录，可用年份：{}。\n",
		records.size(),
		BuildAvailableYearText(records));
	return true;
}

bool DownTool::TianDiTuMultidateDown::SelectTimelineRecord(
	const std::vector<TimelineInfo>& records,
	TimelineInfo& record) const
{
	std::vector<TimelineInfo> candidates;
	for (const TimelineInfo& timelineRecord : records)
	{
		if (timelineRecord.key.empty())
		{
			continue;
		}

		if (!SelectedYear.empty() && timelineRecord.year != SelectedYear)
		{
			continue;
		}

		if (!timelineRecord.projection.empty() && timelineRecord.projection != "c")
		{
			continue;
		}

		candidates.push_back(timelineRecord);
	}

	if (candidates.empty())
	{
		for (const TimelineInfo& timelineRecord : records)
		{
			if (timelineRecord.key.empty())
			{
				continue;
			}

			if (SelectedYear.empty() || timelineRecord.year == SelectedYear)
			{
				candidates.push_back(timelineRecord);
			}
		}
	}

	if (candidates.empty())
	{
		return false;
	}

	record = candidates.front();
	auto isRecordNewer = [](const TimelineInfo& left, const TimelineInfo& right)
	{
		const std::string leftDate = ExtractSortableDate(left.time);
		const std::string rightDate = ExtractSortableDate(right.time);
		if (leftDate.size() != rightDate.size())
		{
			return leftDate.size() > rightDate.size();
		}

		return leftDate > rightDate;
	};
	for (const TimelineInfo& candidate : candidates)
	{
		if (isRecordNewer(candidate, record))
		{
			record = candidate;
		}
	}

	return true;
}

bool DownTool::TianDiTuMultidateDown::ParseTimelineRecords(
	const std::string& responseBody,
	std::vector<TimelineInfo>& records) const
{
	records.clear();

	Json root = Json::parse(responseBody, nullptr, false);
    if (root.is_discarded())
	{
		const std::string decodedBody = DecodeTianDiTuGetRegionBody(responseBody);
		if (decodedBody.empty())
		{
			return false;
		}

		root = Json::parse(decodedBody, nullptr, false);
		if (root.is_discarded())
		{
			return false;
		}
	}

    if (root.is_object() && GetJsonInteger(root, "status", 200) != 200)
	{
		return false;
	}

	Json parsedDataString;
    const Json* dataArray = FindTimelineRecordsJson(root);
	if (dataArray == nullptr && root.is_object())
	{
		const auto data = root.find("data");
		if (data != root.end() && data->is_string())
		{
			parsedDataString = Json::parse(data->get<std::string>(), nullptr, false);
			if (!parsedDataString.is_discarded())
			{
				dataArray = FindTimelineRecordsJson(parsedDataString);
			}
		}
	}

    if (dataArray == nullptr)
    {
        return false;
    }

    for (const Json& item : *dataArray)
    {
        TimelineInfo record;
        record.time = GetJsonString(item, "time");
        if (record.time.empty())
        {
            record.time = GetJsonString(item, "title");
        }
        if (record.time.empty())
        {
            record.time = GetJsonString(item, "year");
        }
        record.year = ExtractYear(record.time);
        if (record.year.empty())
        {
            record.year = ExtractYear(GetJsonString(item, "year"));
        }
        record.key = GetJsonString(item, "key");
        if (record.key.empty())
        {
            record.key = GetJsonString(item, "uuid");
        }
        record.projection = ToLowerAscii(GetJsonString(item, "projection"));
        record.minZoom = static_cast<uint32_t>(
            std::max(0, GetJsonInteger(item, "minlevel", 0)));
        record.maxZoom = static_cast<uint32_t>(
            std::max(0, GetJsonInteger(item, "maxlevel", 0)));
		if (!record.year.empty() && !record.key.empty())
		{
			records.push_back(record);
		}
	}

	return true;
}

bool DownTool::TianDiTuMultidateDown::TryResolveRegionFromCenter()
{
	HttpResponse response;
	if (!HttpGet(BuildChangeCityUrl(), GetRequestHeaders({}), response) ||
		response.statusCode != 200)
	{
		return false;
	}

    const Json root = Json::parse(response.body, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        return false;
    }

    const std::string parentGbCode = GetJsonString(root, "parentGbcode");
    const std::string gbCode = GetJsonString(root, "gbcode");
    const std::string name = GetJsonString(root, "name");
	RegionGbCode = parentGbCode.empty() ? gbCode : parentGbCode;
	RegionName = name;
	return !RegionGbCode.empty();
}

std::string DownTool::TianDiTuMultidateDown::BuildTimelineListUrl() const
{
	return fmt::format(
		"{}/api/multidate/basic/GetRegion?gb={}&lbx={:.8f}&lby={:.8f}&rux={:.8f}&ruy={:.8f}&level={}",
		TianDiTuMultidateHost,
		UrlEncode(RegionGbCode),
		MinLonLat.lon,
		MinLonLat.lat,
		MaxLonLat.lon,
		MaxLonLat.lat,
		Zoom);
}

std::string DownTool::TianDiTuMultidateDown::BuildChangeCityUrl() const
{
	return fmt::format(
		"{}/api/city/changeCity?lng={:.8f}&lat={:.8f}&level={}",
		TianDiTuMapHost,
		CenterLonLat.lon,
		CenterLonLat.lat,
		GetRegionLevel(Zoom));
}

std::string DownTool::TianDiTuMultidateDown::GetLayerCacheName() const
{
	return BuildCacheLayerName(ResolvedYear, ResolvedKey, RegionGbCode, RegionName);
}

bool DownTool::TianDiTuMultidateDown::TryGetGeographicTilePixelForLonLat(
	const LonLat& lonLat,
	uint32_t zoom,
	Tile& tile,
	double& pixelX,
	double& pixelY) const
{
	const double tileSize = static_cast<double>(TileSize);
	if (tileSize <= 0. || zoom == 0)
	{
		return false;
	}

	const double rowCount = std::pow(2., static_cast<double>(zoom) - 1.);
	const double columnCount = rowCount * 2.;
	const double longitude = std::clamp(lonLat.lon, MinLongitude, MaxLongitude);
	const double latitude = std::clamp(lonLat.lat, MinLatitude, MaxLatitude);
	double globalX = (longitude + 180.) / 180. * rowCount;
	double globalY = rowCount - (latitude + 90.) / 180. * rowCount;

	globalX = std::clamp(globalX, 0., std::nextafter(columnCount, 0.));
	globalY = std::clamp(globalY, 0., std::nextafter(rowCount, 0.));

	tile.Zoom = zoom;
	tile.X = static_cast<int32_t>(std::floor(globalX));
	tile.Y = static_cast<int32_t>(std::floor(globalY));
	pixelX = (globalX - tile.X) * tileSize;
	pixelY = (globalY - tile.Y) * tileSize;
	return std::isfinite(pixelX) && std::isfinite(pixelY);
}
