#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"

#include "GCJ02GoogleDownBase.h"
#include "GoogleDownBase.h"
#include "preview/GisOverlayRenderer.h"

namespace DomDownToolPreview
{
class GisOverlayRendererTestAccess
{
  public:
    static void SetDecodedData(GisOverlayRenderer& renderer,
                               const DownTool::AmapVectorDecodedData& decodedData)
    {
        const std::lock_guard lock(renderer.mutex_);
        renderer.preparedData_ = GisOverlayRenderer::prepareData(decodedData);
    }

};
} // namespace DomDownToolPreview

namespace
{
bool colorsEqual(const DownTool::AmapVectorColor& left,
                 const DownTool::AmapVectorColor& right)
{
    return left.red == right.red && left.green == right.green &&
        left.blue == right.blue && left.alpha == right.alpha;
}

bool defaultColorsMatch(const DomDownToolPreview::GisOverlayRenderer& renderer)
{
    using DomDownToolPreview::GisOverlayCategory;

    return colorsEqual(renderer.GetColor(GisOverlayCategory::Road),
                       {255, 218, 113, 255}) &&
        colorsEqual(renderer.GetColor(GisOverlayCategory::Water),
                    {106, 142, 202, 255}) &&
        colorsEqual(renderer.GetColor(GisOverlayCategory::Green),
                    {83, 133, 78, 255}) &&
        colorsEqual(renderer.GetColor(GisOverlayCategory::Building),
                    {255, 255, 255, 255}) &&
        colorsEqual(renderer.GetColor(GisOverlayCategory::Boundary),
                    {54, 55, 56, 255});
}

bool pointRendersAfterPanningAndColorChange(bool wgs84)
{
    const DownTool::LonLat centerWgs84{116.397389, 39.908722};
    const DownTool::LonLat centerGcj02 =
        DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(centerWgs84);
    DownTool::AmapVectorFeature pointFeature;
    pointFeature.geometryType = DownTool::AmapVectorGeometryType::Point;
    pointFeature.layerName = "poi";
    const auto coordinate = wgs84 ? centerWgs84 : centerGcj02;
    if (wgs84)
    {
        pointFeature.SetCoordinateSystem(DownTool::VectorCoordinateSystem::Wgs84);
    }
    pointFeature.geometryParts.push_back(
        {{{coordinate.lon, coordinate.lat}}});

    DownTool::AmapVectorDecodedData decodedData;
    decodedData.features.push_back(std::move(pointFeature));

    DomDownToolPreview::GisOverlayRenderer renderer([] {});
    renderer.SetCategory(DomDownToolPreview::GisOverlayCategory::Point);
    DomDownToolPreview::GisOverlayRendererTestAccess::SetDecodedData(renderer, decodedData);

    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);
    DomDownToolPreview::MapViewport viewport;
    viewport.setSize(64, 64);
    viewport.setCenter(centerWgs84);
    viewport.setZoom(15.);

    SkBitmap bitmap;
    bitmap.allocN32Pixels(64, 64);
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(bitmap);
    renderer.Draw(canvas, viewport, projection,
                  DomDownToolPreview::TileYAxisDirection::Down, 256);
    if (SkColorGetA(bitmap.getColor(32, 32)) == 0)
    {
        return false;
    }

    bitmap.eraseColor(SK_ColorTRANSPARENT);
    if (!viewport.panBy(8., 0., projection,
                        DomDownToolPreview::TileYAxisDirection::Down, 256))
    {
        return false;
    }
    renderer.Draw(canvas, viewport, projection,
                  DomDownToolPreview::TileYAxisDirection::Down, 256);
    if (SkColorGetA(bitmap.getColor(40, 32)) == 0)
    {
        return false;
    }

    const DownTool::AmapVectorColor updatedColor{12, 220, 34, 255};
    renderer.SetColor(DomDownToolPreview::GisOverlayCategory::Point, updatedColor);
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    renderer.Draw(canvas, viewport, projection,
                  DomDownToolPreview::TileYAxisDirection::Down, 256);
    const SkColor renderedColor = bitmap.getColor(40, 32);
    return SkColorGetR(renderedColor) == updatedColor.red &&
        SkColorGetG(renderedColor) == updatedColor.green &&
        SkColorGetB(renderedColor) == updatedColor.blue;
}
bool pointNamesFollowSelectionAndPriority()
{
    using namespace DomDownToolPreview;
    const DownTool::LonLat center{116.397389, 39.908722};
    DownTool::AmapVectorFeature district;
    district.SetCoordinateSystem(DownTool::VectorCoordinateSystem::Wgs84);
    district.layerName = "poi";
    district.mainKey = 10002;
    district.subKey = 37;
    district.names = {{"Changping", "en"}, {"昌平区", "zh-Hans"}};
    district.geometryParts = {{{center.lon, center.lat}}};
    auto shop = district;
    shop.mainKey = 0;
    shop.names = {{"测试商店", "zh"}};
    DownTool::AmapVectorDecodedData decoded;
    decoded.features = {shop, district};
    GisOverlayRenderer renderer([] {});
    renderer.SetCategory(GisOverlayCategory::Point);
    GisOverlayRendererTestAccess::SetDecodedData(renderer, decoded);
    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);
    MapViewport viewport;
    viewport.setSize(160, 64);
    viewport.setCenter(center);
    viewport.setZoom(15.);
    SkBitmap bitmap;
    bitmap.allocN32Pixels(160, 64);
    const auto pixels = [&] {
        bitmap.eraseColor(SK_ColorTRANSPARENT);
        SkCanvas canvas(bitmap);
        renderer.Draw(canvas, viewport, projection, TileYAxisDirection::Down, 256);
        std::vector<SkColor> result;
        for (int x = 0; x < bitmap.width(); ++x)
        {
            for (int y = 0; y < bitmap.height(); ++y)
            {
                result.push_back(bitmap.getColor(x, y));
            }
        }
        return result;
    };
    const auto combined = pixels();
    district.names.erase(district.names.begin());
    decoded.features = {district};
    GisOverlayRendererTestAccess::SetDecodedData(renderer, decoded);
    if (pixels() != combined || std::ranges::count(combined, SK_ColorWHITE) == 0)
    {
        return false;
    }
    renderer.SetCategory(GisOverlayCategory::Road);
    return std::ranges::all_of(pixels(), [](SkColor pixel) { return pixel == SK_ColorTRANSPARENT; });
}

bool qianmenPointStylesRenderDistinctIcons()
{
    using namespace DomDownToolPreview;
    const DownTool::LonLat center{116.394028, 39.898489};
    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);
    MapViewport viewport;
    viewport.setSize(80, 40);
    viewport.setCenter(center);
    viewport.setZoom(18.);
    GisOverlayRenderer renderer([] {});
    renderer.SetCategory(GisOverlayCategory::Point);

    const auto render = [&](uint32_t mainKey, uint32_t subKey, const char* name) {
        DownTool::AmapVectorFeature point;
        point.SetCoordinateSystem(DownTool::VectorCoordinateSystem::Wgs84);
        point.layerName = "poi";
        point.mainKey = mainKey;
        point.subKey = subKey;
        point.names = {{name, "zh-Hans"}};
        point.geometryParts = {{{center.lon, center.lat}}};
        DownTool::AmapVectorDecodedData decoded;
        decoded.features = {point};
        GisOverlayRendererTestAccess::SetDecodedData(renderer, decoded);
        SkBitmap bitmap;
        bitmap.allocN32Pixels(80, 40);
        bitmap.eraseColor(SK_ColorTRANSPARENT);
        SkCanvas canvas(bitmap);
        renderer.Draw(canvas, viewport, projection, TileYAxisDirection::Down, 256);
        return bitmap;
    };
    // 前门东大街两个无名点都使用 10001/2，灯珠从上到下为红、黄、绿。
    const auto signal = render(10001, 2, "");
    const auto red = signal.getColor(40, 15);
    const auto amber = signal.getColor(40, 20);
    const auto green = signal.getColor(40, 25);
    if (SkColorGetR(red) < 180 || SkColorGetG(amber) < 150 ||
        SkColorGetR(amber) < 180 || SkColorGetG(green) < 130 ||
        SkColorGetG(green) <= SkColorGetR(green))
    {
        return false;
    }
    const auto parking = render(12024, 1373, "");
    const auto museum = render(12024, 25, "铁道博物馆");
    const auto parkingColor = parking.getColor(34, 20);
    const auto museumColor = museum.getColor(34, 20);
    return SkColorGetB(parkingColor) > SkColorGetR(parkingColor) &&
        SkColorGetR(museumColor) > SkColorGetB(museumColor) &&
        SkColorGetA(parkingColor) == 255 && SkColorGetA(museumColor) == 255;
}

bool pointLabelArtworkIsReusedWhileNavigating()
{
    using namespace DomDownToolPreview;
    class CountingCanvas final : public SkCanvas
    {
      public:
        using SkCanvas::SkCanvas;
        uint32_t spriteId = 0;

      protected:
        void onDrawImageRect2(const SkImage* image, const SkRect& source, const SkRect& target,
                              const SkSamplingOptions& sampling, const SkPaint* paint,
                              SrcRectConstraint constraint) override
        {
            spriteId = image->uniqueID();
            SkCanvas::onDrawImageRect2(image, source, target, sampling, paint, constraint);
        }
    };

    const DownTool::LonLat center{116.394028, 39.898489};
    DownTool::AmapVectorFeature point;
    point.SetCoordinateSystem(DownTool::VectorCoordinateSystem::Wgs84);
    point.layerName = "poi";
    point.mainKey = 12024;
    point.subKey = 1373;
    point.names = {{"停车场", "zh"}};
    point.geometryParts = {{{center.lon, center.lat}}};
    DownTool::AmapVectorDecodedData decoded;
    decoded.features = {point};
    GisOverlayRenderer renderer([] {});
    renderer.SetCategory(GisOverlayCategory::Point);
    GisOverlayRendererTestAccess::SetDecodedData(renderer, decoded);
    DownTool::GoogleDownBase projection;
    projection.SetTileSize(256);
    MapViewport view;
    view.setSize(160, 64);
    view.setCenter(center);
    view.setZoom(18.);
    SkBitmap bitmap;
    bitmap.allocN32Pixels(160, 64);
    CountingCanvas canvas(bitmap);
    const auto draw = [&] {
        canvas.spriteId = 0;
        canvas.clear(SK_ColorTRANSPARENT);
        renderer.Draw(canvas, view, projection, TileYAxisDirection::Down, 256);
        return canvas.spriteId;
    };
    const auto original = draw();
    view.panBy(4., 0., projection, TileYAxisDirection::Down, 256);
    if (original == 0 || draw() != original)
    {
        return false;
    }
    view.setZoom(18.25);
    if (draw() != original || SkColorGetA(bitmap.getColor(84, 32)) == 0)
    {
        return false;
    }
    view.setZoom(19.);
    if (draw() != original)
    {
        return false;
    }
    view.setCenter(center);
    renderer.SetColor(GisOverlayCategory::Point, {11, 177, 29, 255});
    return draw() != original && bitmap.getColor(74, 32) == SkColorSetRGB(11, 177, 29);
}
} // namespace

int main()
{
    try
    {
        std::atomic_uint32_t redrawCount = 0;
        DomDownToolPreview::GisOverlayRenderer renderer([&redrawCount] { ++redrawCount; });
        if (renderer.GetCategory() != DomDownToolPreview::GisOverlayCategory::None)
        {
            throw std::runtime_error("GIS overlay must be disabled by default");
        }
        if (!defaultColorsMatch(renderer))
        {
            throw std::runtime_error("GIS overlay defaults do not match the Preview palette");
        }

        if (!pointRendersAfterPanningAndColorChange(false) ||
            !pointRendersAfterPanningAndColorChange(true))
        {
            throw std::runtime_error(
                "point did not render after panning or refresh its color");
        }

        renderer.UpdateContext({116.397389, 39.908722}, 100., 15);
        if (!qianmenPointStylesRenderDistinctIcons())
        {
            throw std::runtime_error("前门的无名信号灯、停车场或博物馆未使用对应图标");
        }
        if (!pointLabelArtworkIsReusedWhileNavigating())
        {
            throw std::runtime_error("平移、缩放没有复用点标注，或改色后仍显示旧图标");
        }
        if (!pointNamesFollowSelectionAndPriority())
        {
            throw std::runtime_error("GIS 点的中文名称、碰撞优先级或图层开关不正确");
        }
        if (renderer.IsLoading())
        {
            throw std::runtime_error("disabled GIS overlay started a download");
        }

        renderer.SetCategory(DomDownToolPreview::GisOverlayCategory::Road);
        if (renderer.GetCategory() != DomDownToolPreview::GisOverlayCategory::Road ||
            redrawCount.load() != 1)
        {
            throw std::runtime_error("GIS overlay category change was not applied");
        }

        renderer.SetCategory(DomDownToolPreview::GisOverlayCategory::Road);
        if (redrawCount.load() != 1)
        {
            throw std::runtime_error("unchanged GIS overlay category requested a redraw");
        }

        const DomDownToolPreview::GisOverlayCategories combinedCategories = {
            DomDownToolPreview::GisOverlayCategory::Road,
            DomDownToolPreview::GisOverlayCategory::Water};
        renderer.SetCategories(combinedCategories);
        if (renderer.GetCategories() != combinedCategories ||
            renderer.GetCategory() != DomDownToolPreview::GisOverlayCategory::All ||
            redrawCount.load() != 2)
        {
            throw std::runtime_error("GIS overlay multiple categories were not applied");
        }

        renderer.SetCategories(combinedCategories);
        if (redrawCount.load() != 2)
        {
            throw std::runtime_error("unchanged GIS overlay categories requested a redraw");
        }

        const DownTool::AmapVectorColor customWaterColor{12, 34, 56, 78};
        renderer.SetColor(DomDownToolPreview::GisOverlayCategory::Water,
                          customWaterColor);
        if (!colorsEqual(renderer.GetColor(DomDownToolPreview::GisOverlayCategory::Water),
                         customWaterColor) ||
            redrawCount.load() != 3)
        {
            throw std::runtime_error("GIS overlay color change was not applied");
        }

        renderer.ResetColors();
        if (!defaultColorsMatch(renderer) ||
            redrawCount.load() != 4)
        {
            throw std::runtime_error("GIS overlay colors were not restored to defaults");
        }

        DownTool::AmapVectorFeature feature;
        feature.layerName = "road";
        feature.mainKey = 20001;
        if (DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(feature) !=
            DomDownToolPreview::GisOverlayCategory::Highway)
        {
            throw std::runtime_error("highway style code was not classified as highway");
        }

        feature.mainKey = 20003;
        feature.shield = "G107";
        feature.shieldType = 110101;
        if (DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(feature) !=
            DomDownToolPreview::GisOverlayCategory::Road)
        {
            throw std::runtime_error("ordinary shielded road was misclassified as highway");
        }

        feature = {};
        feature.layerName = "region";
        feature.mainKey = 30001;
        feature.subKey = 3;
        if (DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(feature) !=
            DomDownToolPreview::GisOverlayCategory::Green)
        {
            throw std::runtime_error("green style code was not classified as green space");
        }

        feature = {};
        feature.layerName = "region-line";
        feature.geometryType = DownTool::AmapVectorGeometryType::LineString;
        feature.mainKey = 20014;
        feature.subKey = 2;
        if (DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(feature) !=
            DomDownToolPreview::GisOverlayCategory::Water)
        {
            throw std::runtime_error("water line was not classified as water");
        }

        feature.mainKey = 20010;
        feature.subKey = 1;
        if (DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(feature) !=
            DomDownToolPreview::GisOverlayCategory::Other)
        {
            throw std::runtime_error("railway line was misclassified as a boundary");
        }

        feature.mainKey = 20017;
        if (DomDownToolPreview::GisOverlayRenderer::ClassifyFeature(feature) !=
            DomDownToolPreview::GisOverlayCategory::Boundary)
        {
            throw std::runtime_error(
                "administrative boundary style was not classified as boundary");
        }

        renderer.SetUseTencent(true);
        if (!renderer.GetUseTencent() || renderer.IsLoading())
        {
            throw std::runtime_error("腾讯来源切换没有清除旧请求状态");
        }
        renderer.SetUseTencent(false);
        if (renderer.GetUseTencent())
        {
            throw std::runtime_error("无法恢复高德来源");
        }
        renderer.SetDataSource(DownTool::VectorDataSource::Baidu);
        if (renderer.GetDataSource() != DownTool::VectorDataSource::Baidu ||
            renderer.GetUseTencent() || renderer.IsLoading())
        {
            throw std::runtime_error("百度来源切换没有清除旧请求状态");
        }
        renderer.SetDataSource(DownTool::VectorDataSource::Amap);
        std::cout << "GisOverlayRendererTest passed\n" << std::flush;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "GisOverlayRendererTest failed: " << exception.what() << '\n';
        return 1;
    }
}
