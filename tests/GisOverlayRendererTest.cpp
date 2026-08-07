#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "preview/GisOverlayRenderer.h"

namespace
{
bool colorsEqual(const DownTool::AmapVectorColor& left,
                 const DownTool::AmapVectorColor& right)
{
    return left.red == right.red && left.green == right.green &&
        left.blue == right.blue && left.alpha == right.alpha;
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

        const DownTool::AmapVectorColor customWaterColor{12, 34, 56, 78};
        renderer.SetColor(DomDownToolPreview::GisOverlayCategory::Water,
                          customWaterColor);
        if (!colorsEqual(renderer.GetColor(DomDownToolPreview::GisOverlayCategory::Water),
                         customWaterColor) ||
            redrawCount.load() != 2)
        {
            throw std::runtime_error("GIS overlay color change was not applied");
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
