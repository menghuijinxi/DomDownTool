#include <iostream>
#include <string>

#include "ArcGISDown.h"
#include "BingDown.h"
#include "GoogleDown.h"

namespace
{
    bool Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
        }
        return condition;
    }

    bool RunTest()
    {
        const DownTool::Tile beijingTile(215872, 99317, 18);

        DownTool::GoogleDown google;
        const std::string googleUrl = google.MakeUri(beijingTile);
        if (!Require(
                googleUrl ==
                    "https://mt0.google.com/vt/lyrs=s&hl=zh-CN&gl=cn&scale=1&"
                    "x=215872&y=99317&z=18",
                "Google URL does not match the compatibility template") ||
            !Require(
                google.GetProxy() == "http://127.0.0.1:10090",
                "Google downloads must use the configured local proxy") ||
            !Require(google.IsTileOffset(), "Google China imagery must use GCJ02 correction"))
        {
            return false;
        }

        DownTool::BingDown bing;
        const std::string quadKey = DownTool::BingDown::TileToQuadKey(beijingTile);
        if (!Require(quadKey == "132100103323220202", "Bing QuadKey is incorrect") ||
            !Require(
                bing.MakeUri(beijingTile) ==
                    "https://ecn.t0.tiles.virtualearth.net/tiles/"
                    "a132100103323220202.jpeg?g=0",
                "Bing URL does not contain the expected QuadKey") ||
            !Require(bing.GetProxy().empty(), "Bing downloads must remain direct") ||
            !Require(!bing.IsTileOffset(), "Bing imagery must remain WGS84 aligned"))
        {
            return false;
        }

        bing.SetUrlTemplate("https://example.test/{z}/{x}/{y}/{p}/{quadkey}/{s}");
        if (!Require(
                bing.MakeUri(beijingTile) ==
                    "https://example.test/18/215872/99317/"
                    "132100103323220202/132100103323220202/1",
                "Bing custom templates must support both QuadKey placeholders"))
        {
            return false;
        }

        DownTool::ArcGISDown arcgis;
        if (!Require(
                arcgis.MakeUri(beijingTile) ==
                    "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/"
                    "MapServer/tile/18/99317/215872",
                "ArcGIS URL must use level/row/column order") ||
            !Require(arcgis.GetProxy().empty(), "ArcGIS downloads must remain direct") ||
            !Require(!arcgis.IsTileOffset(), "ArcGIS imagery must remain WGS84 aligned"))
        {
            return false;
        }

        return Require(
            google.GetCachePath() != bing.GetCachePath() &&
                google.GetCachePath() != arcgis.GetCachePath() &&
                bing.GetCachePath() != arcgis.GetCachePath(),
            "Provider cache roots must not overlap");
    }
} // namespace

int main()
{
    if (!RunTest())
    {
        return 1;
    }

    std::cout << "TileProviderUrlTest passed\n";
    return 0;
}
