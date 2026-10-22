#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"

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

        std::cout << "GisOverlayRendererTest passed\n" << std::flush;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "GisOverlayRendererTest failed: " << exception.what() << '\n';
        return 1;
    }
}
