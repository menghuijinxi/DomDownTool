#include "GaoDeDown.h"

#include <cctype>
#include <string>

namespace
{
    constexpr const char* kGaoDeUnavailableTileEtag =
        "a85b8640d75d21ca31aa928225590f50";

    std::string TrimAsciiWhitespace(const std::string& text)
    {
        size_t begin = 0;
        while (begin < text.size() &&
               std::isspace(static_cast<unsigned char>(text[begin])) != 0)
        {
            begin++;
        }

        size_t end = text.size();
        while (end > begin &&
               std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
        {
            end--;
        }

        return text.substr(begin, end - begin);
    }

    std::string NormalizeEtagValue(std::string etag)
    {
        etag = TrimAsciiWhitespace(etag);
        if (etag.size() >= 2 && (etag[0] == 'W' || etag[0] == 'w') && etag[1] == '/')
        {
            etag = TrimAsciiWhitespace(etag.substr(2));
        }

        if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"')
        {
            etag = etag.substr(1, etag.size() - 2);
        }

        for (char& character : etag)
        {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }

        return etag;
    }
} // namespace

bool DownTool::GaoDeDown::IsUnavailableTileResponseHeader(
    const std::map<std::string, std::string>& responseHeaders,
    const Tile& tile) const
{
    (void)tile;

    const auto etagIt = responseHeaders.find("etag");
    if (etagIt == responseHeaders.end())
    {
        return false;
    }

    return NormalizeEtagValue(etagIt->second) == kGaoDeUnavailableTileEtag;
}

DownTool::GaoDeDown::GaoDeDown(GaoDeMapType mapType)
{
    ServerRange.DomServerRange = DownTool::IntVector2(1, 4);
    SetMapType(mapType);
}

void DownTool::GaoDeDown::SetMapType(GaoDeMapType mapType)
{
    mapType_ = mapType;
    if (mapType == GaoDeMapType::Electronic)
    {
        SetUrlTemplate(
            "http://webst0{s}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&"
            "lang=zh_cn&size=1&scale=1&style=7");
        SetCachePath("cache/高德地图/电子地图");
        return;
    }

    if (mapType == GaoDeMapType::RoadNetworkOverlay)
    {
        SetUrlTemplate(
            "https://wprd0{s}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&"
            "lang=zh_cn&size=1&scl=1&style=8&ltype=11");
        SetCachePath("cache/高德地图/透明路网");
        return;
    }

    SetUrlTemplate(
        "http://webst0{s}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&"
        "lang=zh_cn&size=1&scale=1&style=6");
    SetCachePath("cache/高德地图");
}
