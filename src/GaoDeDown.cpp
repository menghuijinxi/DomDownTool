#include "GaoDeDown.h"

DownTool::GaoDeDown::GaoDeDown()
{
	//SetUrlTemplate("https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=7&x={x}&y={y}&z={z}");

	ServerRange.DomServerRange = DownTool::IntVector2(1, 4);
	SetUrlTemplate("http://webst0{s}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&lang=zh_cn&size=1&scale=1&style=6");
	SetCachePath("cache/高德地图");
}
