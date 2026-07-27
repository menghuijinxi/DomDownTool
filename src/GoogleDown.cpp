#include "GoogleDown.h"

DownTool::GoogleDown::GoogleDown()
{
    ServerRange.DomServerRange = DownTool::IntVector2(0, 3);
    SetUrlTemplate(
        "https://mt{s}.google.com/vt/lyrs=s&hl=zh-CN&gl=cn&scale=1&"
        "x={x}&y={y}&z={z}");
    SetProxy("http://127.0.0.1:10090");
    SetCachePath("cache/Google地图");
}
