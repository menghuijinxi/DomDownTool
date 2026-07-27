#include "ArcGISDown.h"

DownTool::ArcGISDown::ArcGISDown()
{
    SetUrlTemplate(
        "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/"
        "MapServer/tile/{z}/{y}/{x}");
    SetCachePath("cache/ArcGIS地图");
}
