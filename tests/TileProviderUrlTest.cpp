#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ArcGISDown.h"
#include "BaiduDown.h"
#include "BingDown.h"
#include "GaoDeDown.h"
#include "GoogleDown.h"
#include "TencentDown.h"

#include "../src/SystemProxy.h"

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

    bool CoordinatesAreNear(
        const DownTool::LonLat& actual,
        const DownTool::LonLat& expected,
        double tolerance)
    {
        return std::abs(actual.lon - expected.lon) <= tolerance &&
            std::abs(actual.lat - expected.lat) <= tolerance;
    }

    bool TestGcj02CoordinateCalibration()
    {
        // 独立校准值用于防止正反转换共享同一公式错误后仍然自洽。
        const DownTool::LonLat shandongWgs84{116.927549, 36.712797};
        const DownTool::LonLat shandongGcj02{116.93314131639661, 36.712916351380116};
        const DownTool::LonLat expectedApproximateWgs84{
            116.92753692675672, 36.71278976033607};

        return Require(
                   CoordinatesAreNear(
                       DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(shandongWgs84),
                       shandongGcj02,
                       1e-10),
                   "WGS84 to GCJ02 calibration failed") &&
            Require(
                CoordinatesAreNear(
                    DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(shandongGcj02),
                    expectedApproximateWgs84,
                    1e-10),
                "GCJ02 to WGS84 calibration failed");
    }

    bool TestElectronicMapProviders()
    {
        DownTool::TencentDown tencent;
        const DownTool::Tile sample(215872, 99317, 18);
        if (!Require(tencent.GetMapType() == DownTool::TencentMapType::Satellite &&
                         tencent.GetCachePath() == "cache/腾讯地图" &&
                         tencent.MakeUri(sample) ==
                             "https://p0.map.gtimg.com/sateTiles/18/13492/10176/215872_162826.png",
                     "Tencent satellite defaults changed"))
        {
            return false;
        }
        tencent.SetMapType(DownTool::TencentMapType::Electronic);
        if (!Require(tencent.GetMapType() == DownTool::TencentMapType::Electronic &&
                         tencent.GetCachePath() == "cache/腾讯地图/电子地图" &&
                         tencent.MakeUri(sample) ==
                             "https://rt1.map.gtimg.com/tile?z=18&x=215872&y=162826&"
                             "styleid=0&scene=0&version=114",
                     "Tencent electronic tiles must use their own endpoint and cache"))
        {
            return false;
        }
        DownTool::TencentDown electronicTencent(DownTool::TencentMapType::Electronic);
        if (!Require(electronicTencent.MakeUri({0, 0, 3}) ==
                             "https://rt0.map.gtimg.com/tile?z=3&x=0&y=7&"
                             "styleid=0&scene=0&version=114" &&
                         electronicTencent.MakeUri({7, 7, 3}) ==
                             "https://rt1.map.gtimg.com/tile?z=3&x=7&y=0&"
                             "styleid=0&scene=0&version=114",
                     "Tencent electronic TMS Y boundaries are inverted"))
        {
            return false;
        }
        tencent.SetMapType(DownTool::TencentMapType::Satellite);
        if (!Require(tencent.GetCachePath() == "cache/腾讯地图",
                     "Tencent satellite cache was not restored"))
        {
            return false;
        }

        DownTool::BaiduDown baidu;
        if (!Require(baidu.GetMapType() == DownTool::BaiduMapType::Satellite &&
                         baidu.GetCachePath() == "cache/百度地图" &&
                         baidu.MakeUri({12666, 4710, 16}) ==
                             "https://maponline0.bdimg.com/starpic/?qt=satepc&"
                             "u=x=12666;y=4710;z=16;v=009;type=sate",
                     "Baidu satellite defaults changed"))
        {
            return false;
        }
        baidu.SetMapType(DownTool::BaiduMapType::Electronic);
        if (!Require(baidu.GetMapType() == DownTool::BaiduMapType::Electronic &&
                         baidu.GetCachePath() == "cache/百度地图/电子地图" &&
                         baidu.MakeUri({12666, 4710, 16}) ==
                             "https://maponline1.bdimg.com/tile/?qt=vtile&x=12666&y=4710&"
                             "z=16&styles=pl&scaler=1",
                     "Baidu electronic tiles must use their own endpoint and cache"))
        {
            return false;
        }
        DownTool::BaiduDown electronicBaidu(DownTool::BaiduMapType::Electronic);
        if (!Require(electronicBaidu.MakeUri({-1, -2, 3}) ==
                             "https://maponline0.bdimg.com/tile/?qt=vtile&x=M1&y=M2&"
                             "z=3&styles=pl&scaler=1" &&
                         electronicBaidu.MakeUri({std::numeric_limits<int32_t>::min(), 0, 3}) ==
                             "https://maponline1.bdimg.com/tile/?qt=vtile&x=M2147483648&y=0&"
                             "z=3&styles=pl&scaler=1",
                     "Baidu negative tile coordinates must use M without overflowing"))
        {
            return false;
        }
        baidu.SetMapType(DownTool::BaiduMapType::Satellite);
        return Require(baidu.GetCachePath() == "cache/百度地图",
                       "Baidu satellite cache was not restored");
    }

    bool TestSystemHttpsProxySelection()
    {
        const std::array<std::pair<std::string_view, std::string_view>, 13> cases{
            {{"", ""},
             {" ; \t ", ""},
             {"127.0.0.1:18080", "http://127.0.0.1:18080"},
             {"http://127.0.0.1:18080", "http://127.0.0.1:18080"},
             {"http=127.0.0.1:18080;https=127.0.0.1:18081", "http://127.0.0.1:18081"},
             {"HTTPS=proxy.example:443;http=proxy.example:80", "http://proxy.example:443"},
             {"http=proxy.example:80", ""},
             {"socks=127.0.0.1:11080", ""},
             {"https=", ""},
             {"proxy.example:8080;https=secure.example:8081", "http://secure.example:8081"},
             {"https=socks5h://127.0.0.1:11080", "socks5h://127.0.0.1:11080"},
             {"https=[::1]:18080", "http://[::1]:18080"},
             {" \thttp=proxy.example:80 ; https=proxy.example:443 ; ",
              "http://proxy.example:443"}}};
        for (const auto& [configuration, expected] : cases)
        {
            if (!Require(DownTool::detail::SelectSystemHttpsProxy(configuration) == expected,
                         "Incorrect HTTPS proxy selection: " + std::string(configuration)))
            {
                return false;
            }
        }
        return true;
    }

    bool TestGoogleProxyRequests()
    {
        DownTool::GoogleDown google;
        const DownTool::Tile tile(215872, 99317, 18);
        const auto request = google.BuildTileHttpRequest(tile);
        if (!Require(request.proxy == DownTool::detail::GetSystemHttpsProxy(),
                     "Google preview requests must inherit the detected system proxy") ||
            !Require(request.headers == std::vector<std::string>{"User-Agent: DomDownTool/1.0"},
                     "Google requests must identify the downloader"))
        {
            return false;
        }

        google.SetProxy("http://manual.example:18080");
        if (!Require(google.BuildTileHttpRequest(tile).proxy == "http://manual.example:18080",
                     "An explicit Google proxy must override the system proxy"))
        {
            return false;
        }
        google.SetProxy("");
        if (!Require(google.BuildTileHttpRequest(tile).proxy.empty(),
                     "Disabling the Google proxy must not restore the system proxy"))
        {
            return false;
        }

        DownTool::DownToolBase defaultSource;
        DownTool::GaoDeDown gaode;
        DownTool::TencentDown tencent;
        DownTool::BaiduDown baidu;
        return Require(defaultSource.GetProxy().empty() && gaode.GetProxy().empty() &&
                           tencent.GetProxy().empty() && baidu.GetProxy().empty(),
                       "Detecting Google's system proxy must not change other map sources");
    }

    bool RunTest()
    {
        const DownTool::Tile beijingTile(215872, 99317, 18);
        const DownTool::Tile gaodeSampleTile(107839, 49649, 17);

        DownTool::GaoDeDown gaode;
        if (!Require(
                gaode.MakeUri(gaodeSampleTile) ==
                    "http://webst01.is.autonavi.com/appmaptile?x=107839&y=49649&z=17&"
                    "lang=zh_cn&size=1&scale=1&style=6",
                "GaoDe satellite URL does not match the compatibility template") ||
            !Require(
                gaode.GetMapType() == DownTool::GaoDeMapType::Satellite,
                "GaoDe must default to satellite maps"))
        {
            return false;
        }
        if (!Require(
                gaode.GetCachePath() == "cache/高德地图/卫星图",
                "GaoDe satellite tiles must use a dedicated cache"))
        {
            return false;
        }

        gaode.SetMapType(DownTool::GaoDeMapType::Electronic);
        if (!Require(
                gaode.MakeUri(gaodeSampleTile) ==
                    "http://webst02.is.autonavi.com/appmaptile?x=107839&y=49649&z=17&"
                    "lang=zh_cn&size=1&scale=1&style=7",
                "GaoDe electronic URL does not match the selected map type") ||
            !Require(
                gaode.GetCachePath() == "cache/高德地图/电子地图",
                "GaoDe electronic tiles must use a separate cache"))
        {
            return false;
        }

        gaode.SetMapType(DownTool::GaoDeMapType::RoadNetworkOverlay);
        if (!Require(
                gaode.MakeUri(gaodeSampleTile) ==
                    "https://wprd03.is.autonavi.com/appmaptile?x=107839&y=49649&z=17&"
                    "lang=zh_cn&size=1&scl=1&style=8&ltype=11",
                "GaoDe road network overlay URL does not match the selected map type") ||
            !Require(
                gaode.GetCachePath() == "cache/高德地图/透明路网",
                "GaoDe road network overlay tiles must use a separate cache"))
        {
            return false;
        }

        DownTool::GoogleDown google;
        const std::string googleUrl = google.MakeUri(beijingTile);
        if (!Require(
                googleUrl ==
                    "https://mt0.google.com/vt/lyrs=s&hl=zh-CN&gl=cn&scale=1&"
                    "x=215872&y=99317&z=18",
                "Google URL does not match the compatibility template") ||
            !Require(
                google.GetProxy() == DownTool::detail::GetSystemHttpsProxy(),
                "Google downloads must use the enabled system HTTPS proxy") ||
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

        return Require(google.GetCachePath() != bing.GetCachePath() &&
                           google.GetCachePath() != arcgis.GetCachePath() &&
                           bing.GetCachePath() != arcgis.GetCachePath(),
                       "Provider cache roots must not overlap") &&
               TestGcj02CoordinateCalibration() && TestElectronicMapProviders() &&
               TestSystemHttpsProxySelection() && TestGoogleProxyRequests();
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
