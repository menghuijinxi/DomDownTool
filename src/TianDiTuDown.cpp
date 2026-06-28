#include "TianDiTuDown.h"

#include <cctype>
#include <string>
#include <vector>

#include "fmt/format.h"

namespace
{
	std::string ToLowerAscii(std::string text)
	{
		for (char& character : text)
		{
			character = static_cast<char>(
				std::tolower(static_cast<unsigned char>(character)));
		}
		return text;
	}
}

DownTool::TianDiTuDown::TianDiTuDown()
{
	SetUrlTemplate(fmt::format(
		"https://t{}.tianditu.gov.cn/img_w/wmts?"
		"SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=img&STYLE=default&"
		"TILEMATRIXSET=w&FORMAT=tiles&TILECOL={}&TILEROW={}&TILEMATRIX={}&"
		"tk=75f0434f240669f4a2df6359275146d2",
		serverIndexReplace,
		xReplace,
		yReplace,
		zReplace));
	ServerRange.DomServerRange = DownTool::IntVector2(0, 7);
	SetCachePath("cache/天地图");
}

bool DownTool::TianDiTuDown::IsUnavailableTileResponseHeader(
	const std::map<std::string, std::string>& responseHeaders,
	const Tile& tile) const
{
	(void)tile;

	const auto contentTypeIt = responseHeaders.find("content-type");
	if (contentTypeIt == responseHeaders.end())
	{
		return false;
	}

	const std::string contentType = ToLowerAscii(contentTypeIt->second);
	return contentType.find("image/") == std::string::npos &&
		(contentType.find("json") != std::string::npos ||
			contentType.find("xml") != std::string::npos ||
			contentType.find("html") != std::string::npos ||
			contentType.find("text") != std::string::npos);
}

std::vector<std::string> DownTool::TianDiTuDown::GetRequestHeaders(
	const Tile& tile) const
{
	(void)tile;

	return {
		"Referer: https://map.tianditu.gov.cn/",
		"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
			"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/139.0.0.0 "
			"Safari/537.36 Edg/139.0.0.0"
	};
}
