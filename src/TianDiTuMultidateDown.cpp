#include "TianDiTuMultidateDown.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
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
	TianDiTuDown::StartDown();
	CachePath = originalCachePath;
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
	SetLatestWmtsUrlTemplate();
}

void DownTool::TianDiTuMultidateDown::UseLatestYear()
{
	SelectedYear.clear();
	ResolvedYear.clear();
	ResolvedKey.clear();
	bUseLatestYear = true;
	bHasPresetYearKey = false;
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

    const Json root = Json::parse(responseBody, nullptr, false);
    if (root.is_discarded())
	{
		return false;
	}

    if (root.is_object() && GetJsonInteger(root, "status", 200) != 200)
	{
		return false;
	}

    const Json* dataArray = nullptr;
    if (root.is_array())
    {
        dataArray = &root;
    }
    else if (root.is_object())
    {
        const auto data = root.find("data");
        if (data != root.end() && data->is_array())
        {
            dataArray = &*data;
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
        record.year = ExtractYear(record.time);
        record.key = GetJsonString(item, "key");
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

	return !records.empty();
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

    const std::string gbCode = GetJsonString(root, "parentGbcode");
    const std::string name = GetJsonString(root, "name");
    RegionGbCode = gbCode.empty() ? GetJsonString(root, "gbcode") : gbCode;
	RegionName = name;
	return !RegionGbCode.empty();
}

std::string DownTool::TianDiTuMultidateDown::BuildTimelineListUrl() const
{
	return fmt::format(
		"{}/api/multidate/basic/list?gb={}&lbx={:.8f}&lby={:.8f}&rux={:.8f}&ruy={:.8f}&level={}",
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
	if (!ResolvedYear.empty())
	{
		return ResolvedYear;
	}

	return "latest";
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
