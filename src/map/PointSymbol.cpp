#include "PointSymbol.h"

#include <algorithm>
#include <initializer_list>
#include <map>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/utils/SkParsePath.h"

namespace DownTool
{
namespace
{
// 高德详细 POI 按子类编号选图标。范围根据当前前门瓦片的类型分组，未知编号保留普通标记。
PointSymbol classifyDetailedPoi(uint32_t subKey)
{
    using enum PointSymbol;
    struct Range
    {
        uint32_t first;
        uint32_t last;
        PointSymbol symbol;
    };
    static constexpr Range styles[] = {
        {2, 2, Park}, {8, 9, Park}, {24, 26, Museum}, {28, 30, Museum},
        {34, 36, Museum}, {37, 39, Park}, {43, 43, Park}, {55, 55, Museum},
        {62, 91, Hotel}, {95, 351, Food}, {352, 361, Cafe}, {366, 366, Cafe},
        {375, 442, Shop}, {516, 516, Shop}, {528, 532, Shop}, {534, 534, Shop},
        {554, 561, Shop}, {564, 569, Shop}, {664, 664, School}, {672, 672, School},
        {676, 676, School}, {680, 680, School}, {854, 854, Restroom},
        {979, 979, Museum}, {999, 999, Museum}, {1055, 1055, Cafe},
        {1104, 1105, Medical}, {1111, 1111, Medical}, {1118, 1118, Medical},
        {1133, 1133, Medical}, {1227, 1227, Exit}, {1237, 1239, Exit},
        {1249, 1249, Address}, {1259, 1259, Home}, {1261, 1263, Home},
        {1264, 1264, Taxi}, {1266, 1266, Charging}, {1269, 1269, Charging},
        {1273, 1273, Taxi}, {1372, 1374, Parking}, {1377, 1377, Parking},
        {1436, 1437, Bank}, {1439, 1439, Bank}, {1444, 1445, Food},
    };
    const auto match = std::ranges::lower_bound(styles, subKey, {}, &Range::last);
    return match != std::end(styles) && match->first <= subKey ? match->symbol : Dot;
}
} // namespace

PointSymbol classifyPointSymbol(const AmapVectorFeature& feature)
{
    if (feature.GetDataSource() == VectorDataSource::Amap)
    {
        // 高德 JSAPI 2.3.5.6 的 districtsname、restaurant、hotel、hospital 样式表。
        const auto matches = [&](uint32_t main, std::initializer_list<uint32_t> sub) {
            return feature.mainKey == main && std::ranges::find(sub, feature.subKey) != sub.end();
        };
        if (feature.mainKey == 12024)
        {
            return classifyDetailedPoi(feature.subKey);
        }
        if (matches(10001, {2}))
        {
            return PointSymbol::TrafficLight;
        }
        if (matches(10001, {24, 25}))
        {
            return PointSymbol::Bus;
        }
        if (matches(10001, {26}) || matches(10005, {1, 53}))
        {
            return PointSymbol::Train;
        }
        if (matches(10005, {41}))
        {
            return PointSymbol::Exit;
        }
        if (matches(10001, {14, 69, 100, 101, 102}))
        {
            return PointSymbol::Park;
        }
        if (matches(10001, {59}))
        {
            return PointSymbol::Restroom;
        }
        if (matches(10007, {14, 15, 16, 17}))
        {
            return PointSymbol::Bank;
        }
        if (matches(10002, {1, 2, 3, 4, 5, 7, 24, 25, 27, 30, 31, 32, 34, 35}))
        {
            return PointSymbol::City;
        }
        if (matches(10002, {6, 8, 37}))
        {
            return PointSymbol::District;
        }
        if (matches(10001, {19, 20, 21, 22, 114, 115, 116, 117, 118, 119, 183, 187}) ||
            matches(10007, {1, 2, 3, 4, 166, 167, 168, 179, 180, 181, 203, 205, 206, 215}))
        {
            return PointSymbol::Food;
        }
        if (matches(10001, {9, 133, 134, 135, 136, 155, 156, 157, 158, 159, 160, 161, 162, 186}) ||
            matches(10007, {31, 32, 33, 34, 35, 36, 37, 38, 39, 164, 165}))
        {
            return PointSymbol::Hotel;
        }
        if (matches(10001, {32, 33, 57, 70, 131, 132, 169, 193, 206, 207, 208, 209, 210}) ||
            matches(10007, {170, 209}))
        {
            return PointSymbol::Medical;
        }
    }
    const auto& props = feature.GetSourceProperties();
    const auto it =
        props.find(feature.GetDataSource() == VectorDataSource::Tencent ? "tag" : "featureType");
    if (it != props.end() && it->is_string())
    {
        const auto& tag = it->get_ref<const std::string&>();
        if (tag == "carpark" || tag == "PARKINGLOTLABEL")
        {
            return PointSymbol::Parking;
        }
        if (tag == "hospital" || tag == "MEDICALLABEL")
        {
            return PointSymbol::Medical;
        }
        if (tag == "restrant" || tag == "RESTAURANTLABEL")
        {
            return PointSymbol::Food;
        }
        if (tag == "hotel" || tag == "HOTELLABEL")
        {
            return PointSymbol::Hotel;
        }
        if (tag == "city")
        {
            return PointSymbol::City;
        }
        if (tag == "district")
        {
            return PointSymbol::District;
        }
    }
    return PointSymbol::Dot;
}

bool hasPointIcon(PointSymbol symbol)
{
    return symbol != PointSymbol::Dot && symbol != PointSymbol::City &&
        symbol != PointSymbol::District && symbol != PointSymbol::Address;
}

void drawPointIcon(SkCanvas& canvas, PointSymbol symbol, AmapVectorColor color, bool usePalette)
{
    SkPaint paint;
    paint.setAntiAlias(true);
    const auto tint = [&](SkColor rgb) { paint.setColor(SkColorSetA(rgb, color.alpha)); };
    const auto markerColor = [&](SkColor paletteColor) {
        return usePalette ? paletteColor : SkColorSetRGB(color.red, color.green, color.blue);
    };
    if (symbol == PointSymbol::TrafficLight)
    {
        tint(SK_ColorWHITE);
        canvas.drawRoundRect(SkRect::MakeLTRB(-4.5F, -10.F, 4.5F, 10.F), 4.F, 4.F, paint);
        tint(markerColor(0xff344154));
        canvas.drawRoundRect(SkRect::MakeLTRB(-3.5F, -9.F, 3.5F, 9.F), 3.F, 3.F, paint);
        tint(0xfffa5752);
        canvas.drawCircle(0.F, -5.F, 2.1F, paint);
        tint(0xffffc444);
        canvas.drawCircle(0.F, 0.F, 2.1F, paint);
        tint(0xff38d584);
        canvas.drawCircle(0.F, 5.F, 2.1F, paint);
        return;
    }

    // 图形以锚点为原点，首次使用时解析一次，预览和各导出线程共享只读路径。
    struct Glyph
    {
        Glyph(SkColor background, const char* outline)
            : background(background), path(SkParsePath::FromSVGString(outline).value())
        {
        }
        SkColor background;
        SkPath path;
    };
    using enum PointSymbol;
    static const std::map<PointSymbol, Glyph> glyphs = {
        {Parking, {0xff2779cf, "M-2.5 4.6V-4.6H.4C5.1 -4.6 5.1 1 .4 1H-2.5"}},
        {Bus, {0xff079775, "M-5 -3.5H5V3H-5Z M-4 -1H4 M-3 3V4.5 M3 3V4.5"}},
        {Train, {0xff2878cb, "M-3.7 -4.6H3.7V2H-3.7Z M-3.7 -1H3.7 M-1.8 2L-4 5 M1.8 2L4 5"}},
        {Exit, {0xff657da4, "M0 -4.8H4.5V4.8H0 M-5 0H1 M-2 -2.8L-5 0L-2 2.8"}},
        {Taxi, {0xff607da4, "M-5 2.5V-.3L-3 -3H3L5 -.3V2.5Z M-4.8 -.2H4.8 M-3.5 2.5V4 M3.5 2.5V4"}},
        {Charging, {0xff269269, "M1 -5L-3.5 .4H.4L-1.2 5L4 -1H.1Z"}},
        {Food, {0xffe37c22, "M-4 -4.5V-1Q-4 1 -2.3 1Q-.6 1 -.6 -1V-4.5 M-2.3 -4.5V4.6 "
                             "M3.5 4.6V-4.5Q.9 -2.8 1.6 0H3.5"}},
        {Cafe, {0xffaa7249, "M-4 -3H2.5V.5Q2.5 3.5 -.6 3.5Q-4 3.5 -4 .5Z "
                            "M2.5 -2.5H3.5Q6.5 -2 4.2 .6H2.5 M-4 5H3.5"}},
        {Hotel, {0xff7062c3, "M-4.8 -3.5V4.5 M4.8 1V4.5 M-4.8 1.8H4.8V-1.2H0V1.8 "
                             "M-3 -1.6H-2V-.5H-3Z"}},
        {Shop, {0xffa452bc, "M-4.2 -4.2H4.2L5 -1H-5Z M-3.8 -.6V4.1H3.8V-.6 M0 4V1H2V4"}},
        {Medical, {0xffc74962, "M-1.4 -4.5H1.4V-1.4H4.5V1.4H1.4V4.5H-1.4V1.4H-4.5V-1.4H-1.4Z"}},
        {Restroom, {0xff3486a0, "M-4.8 -3.5L-3.5 3L-2 .3L-.8 3L.2 -3.5 "
                                "M4.8 -2.8Q1.4 -4.4 1.4 0Q1.4 4.4 4.8 2.8"}},
        {Park, {0xff479538, "M0 -5L-3 -1.2H-1.8L-4.1 2H4.1L1.8 -1.2H3Z M0 2V4.8 M-2 4.8H2"}},
        {Museum, {0xff916e61, "M-4.9 -1.8L0 -4.8L4.9 -1.8Z M-3 -1V3 M0 -1V3 M3 -1V3 M-4.8 4.3H4.8"}},
        {Bank, {0xff408d98, "M-3.3 -4L0 -.3L3.3 -4 M0 -.3V4.6 M-3 -.3H3 M-3 2H3"}},
        {Home, {0xff7589aa, "M-5 -.3L0 -4.5L5 -.3 M-3.5 -1V4H3.5V-1 M-.9 4V1H.9V4"}},
        {School, {0xff618ca0, "M-5 -2L0 -4.5L5 -2L0 .5Z M-3 .2V2.4Q0 5.1 3 2.4V.2 M5 -2V2"}},
    };
    const auto& glyph = glyphs.at(symbol);
    tint(SK_ColorWHITE);
    canvas.drawCircle(0.F, 0.F, 9.5F, paint);
    tint(markerColor(glyph.background));
    canvas.drawCircle(0.F, 0.F, 8.2F, paint);
    tint(SK_ColorWHITE);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeJoin(SkPaint::kRound_Join);
    paint.setStrokeCap(SkPaint::kRound_Cap);
    paint.setStrokeWidth(1.5F);
    canvas.drawPath(glyph.path, paint);
}
} // namespace DownTool
