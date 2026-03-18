#include "TianDiTuDown.h"
#include "fmt/format.h"

DownTool::TianDiTuDown::TianDiTuDown()
{
	SetUrlTemplate(fmt::format("https://t{}.tianditu.gov.cn/img_w/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=img&STYLE=default&TILEMATRIXSET=w&FORMAT=tiles&TILECOL={}&TILEROW={}&TILEMATRIX={}&tk=75f0434f240669f4a2df6359275146d2", serverIndexReplace, xReplace, yReplace, zReplace));
	ServerRange.DomServerRange = DownTool::IntVector2(0, 7);
	SetCachePath("cache/天地图");
}
