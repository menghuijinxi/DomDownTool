#include "EsriWaybackDown.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <stdexcept>
#include <utility>

#include "nlohmann/json.hpp"

#include "SystemProxy.h"

namespace
{
bool IsReleaseNumber(std::string_view number)
{
    return !number.empty() && number.size() <= 10 && number.front() != '0' &&
           std::all_of(number.begin(), number.end(),
                       [](char character) { return character >= '0' && character <= '9'; });
}

bool IsReleaseDate(std::string_view date)
{
    if (date.size() != 10 || date[4] != '-' || date[7] != '-')
    {
        return false;
    }
    const auto parse = [](std::string_view value) {
        int number = 0;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
        return result.ec == std::errc{} && result.ptr == value.data() + value.size() ? number : -1;
    };
    const int year = parse(date.substr(0, 4));
    const int month = parse(date.substr(5, 2));
    const int day = parse(date.substr(8, 2));
    return year >= 2000 && month > 0 && day > 0 &&
           std::chrono::year_month_day{std::chrono::year(year), std::chrono::month(month),
                                       std::chrono::day(day)}
               .ok();
}

bool IsTileUrl(std::string_view url, const std::string& number)
{
    for (const std::string_view host : {"wayback", "wayback-a", "wayback-b"})
    {
        const std::string expected =
            "https://" + std::string(host) +
            ".maptiles.arcgis.com/arcgis/rest/services/World_Imagery/WMTS/1.0.0/"
            "default028mm/MapServer/tile/" +
            number + "/{level}/{row}/{col}";
        if (url == expected)
        {
            return true;
        }
    }
    return false;
}
} // namespace

DownTool::WaybackRelease::WaybackRelease(std::string number, std::string date, std::string itemId,
                                         std::string tileUrl)
    : number_(std::move(number)), date_(std::move(date)), itemId_(std::move(itemId)),
      tileUrl_(std::move(tileUrl))
{
    const bool validItem =
        itemId_.size() == 32 && std::all_of(itemId_.begin(), itemId_.end(), [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
    if (!IsReleaseNumber(number_) || !IsReleaseDate(date_) || !validItem ||
        !IsTileUrl(tileUrl_, number_))
    {
        throw std::invalid_argument("Wayback 版本记录无效。");
    }
}

std::vector<DownTool::WaybackRelease> DownTool::ParseWaybackCatalog(std::string_view text)
{
    if (text.size() > 4 * 1024 * 1024)
    {
        throw std::runtime_error("Wayback 版本目录过大。");
    }
    const auto catalog = nlohmann::json::parse(text);
    if (!catalog.is_object() || catalog.empty() || catalog.size() > 2000)
    {
        throw std::runtime_error("Wayback 版本目录格式无效。");
    }
    constexpr std::string_view prefix = "World Imagery (Wayback ";
    std::vector<WaybackRelease> releases;
    releases.reserve(catalog.size());
    for (const auto& [number, item] : catalog.items())
    {
        const std::string title = item.at("itemTitle").get<std::string>();
        if (!title.starts_with(prefix) || title.size() != prefix.size() + 11 || title.back() != ')')
        {
            throw std::runtime_error("Wayback 发布日期格式无效。");
        }
        releases.emplace_back(number, title.substr(prefix.size(), 10),
                              item.at("itemID").get<std::string>(),
                              item.at("itemURL").get<std::string>());
    }
    // 目录键是无序版本编号，不能用编号大小推断发布日期。
    std::sort(releases.begin(), releases.end(), [](const auto& left, const auto& right) {
        if (left.GetDate() != right.GetDate())
        {
            return left.GetDate() > right.GetDate();
        }
        return left.GetNumber() < right.GetNumber();
    });
    return releases;
}

DownTool::EsriWaybackDown::EsriWaybackDown()
{
    SetProxy(detail::GetSystemHttpsProxy());
    SetCachePath("cache/Esri Wayback/preview");
}

void DownTool::EsriWaybackDown::SetRelease(const WaybackRelease& release)
{
    std::string url = release.GetTileUrl();
    for (const auto& [from, to] : {std::pair{"{level}", "{z}"}, {"{row}", "{y}"}, {"{col}", "{x}"}})
    {
        url.replace(url.find(from), std::char_traits<char>::length(from), to);
    }
    SetUrlTemplate(url);
    SetCachePath("cache/Esri Wayback/preview/" + release.GetNumber());
}

std::vector<std::string> DownTool::EsriWaybackDown::GetRequestHeaders(const Tile&) const
{
    return {"User-Agent: DomDownToolPreview/1.0"};
}
