#pragma once

#include <string_view>

namespace DomDownToolPreview
{
inline constexpr std::string_view kPlaceDetailPrefix = "https://www.amap.com/ssr/place/";

[[nodiscard]] constexpr bool isPlaceDetailUrl(std::string_view url)
{
    if (!url.starts_with(kPlaceDetailPrefix))
    {
        return false;
    }
    const std::string_view id = url.substr(kPlaceDetailPrefix.size());
    return !id.empty() &&
        id.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") ==
            std::string_view::npos;
}

[[nodiscard]] constexpr bool isSearchPageUrl(std::string_view url)
{
    // 高德会将旧版搜索地址重定向到 SSR 结果页，并保留查询参数。
    const std::string_view page = url.substr(0, url.find_first_of("?#"));
    return page == "https://www.amap.com/search" || page == "https://www.amap.com/ssr/search";
}
} // namespace DomDownToolPreview
