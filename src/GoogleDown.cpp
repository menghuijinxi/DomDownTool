#include "GoogleDown.h"

#include "SystemProxy.h"

DownTool::GoogleDown::GoogleDown()
{
    ServerRange.DomServerRange = DownTool::IntVector2(0, 3);
    SetUrlTemplate(
        "https://mt{s}.google.com/vt/lyrs=s&hl=zh-CN&gl=cn&scale=1&"
        "x={x}&y={y}&z={z}");
    const std::string systemProxy = detail::GetSystemHttpsProxy();
    if (!systemProxy.empty())
    {
        SetProxy(systemProxy);
    }
    SetCachePath("cache/Google地图");
}

std::vector<std::string> DownTool::GoogleDown::GetRequestHeaders(const Tile& tile) const
{
    (void)tile;
    return {"User-Agent: DomDownTool/1.0"};
}
