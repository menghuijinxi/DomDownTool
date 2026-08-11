#include "preview/PreviewController.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "fmt/format.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkRect.h"
#include "skui_runtime.h"

#include "preview/SearchTrace.h"

namespace
{
constexpr UINT kPreviewRedrawMessage = WM_APP + 0x642;
constexpr UINT kPreviewUiRefreshMessage = WM_APP + 0x643;
constexpr UINT kPreviewSearchRefreshMessage = WM_APP + 0x644;
constexpr UINT kPreviewMapUiRefreshMessage = WM_APP + 0x646;
constexpr UINT kPreviewExportTaskRefreshMessage = WM_APP + 0x647;
constexpr UINT kPreviewPageChangeMessage = WM_APP + 0x648;
constexpr UINT kPreviewImmediateMapRedrawMessage = WM_APP + 0x649;
constexpr UINT_PTR kPreviewEyedropperTimerId = 0x645;
constexpr UINT kPreviewEyedropperTimerIntervalMilliseconds = 16;
constexpr double kSearchControlLeft = 192.;
constexpr double kSearchControlWidth = 300.;
constexpr double kSearchPopupWidth = 360.;
constexpr double kSearchPopupTop = 46.;
constexpr double kSearchPopupHeight = 374.;
constexpr double kGisOverlayPanelLeft = 735.;
constexpr double kGisOverlayPanelTop = 48.;
constexpr double kGisOverlayPanelWidth = 184.;
constexpr double kGisOverlayPanelHeight = 322.;
constexpr double kGisColorPanelLeft = 720.;
constexpr double kGisColorPanelTop = 48.;
constexpr double kGisColorPanelWidth = 342.;
constexpr double kGisColorPanelHeight = 562.;
constexpr double kGisColorFieldLeft = 730.;
constexpr double kGisColorFieldTop = 427.;
constexpr double kGisColorFieldWidth = 190.;
constexpr double kGisColorFieldHeight = 150.;
constexpr double kGisColorHueTop = kGisColorFieldTop;
constexpr double kGisColorHueHeight = kGisColorFieldHeight;

bool isPointInside(double x, double y, double left, double top, double width, double height)
{
    return x >= left && x <= left + width && y >= top && y <= top + height;
}

POINT clientPoint(LPARAM lParam)
{
    return POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                 static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
}

class ScreenDeviceContext
{
  public:
    ScreenDeviceContext() : handle_(GetDC(nullptr))
    {
    }

    ~ScreenDeviceContext()
    {
        if (handle_)
        {
            ReleaseDC(nullptr, handle_);
        }
    }

    ScreenDeviceContext(const ScreenDeviceContext&) = delete;
    ScreenDeviceContext& operator=(const ScreenDeviceContext&) = delete;

    [[nodiscard]] HDC get() const
    {
        return handle_;
    }

  private:
    HDC handle_ = nullptr;
};

std::optional<DownTool::AmapVectorColor> screenPixelColor(const POINT& screenPoint)
{
    const ScreenDeviceContext screen;
    if (!screen.get())
    {
        return std::nullopt;
    }

    const COLORREF color = GetPixel(screen.get(), screenPoint.x, screenPoint.y);
    if (color == CLR_INVALID)
    {
        return std::nullopt;
    }
    return DownTool::AmapVectorColor{GetRValue(color), GetGValue(color), GetBValue(color), 255};
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool equalStats(const DomDownToolPreview::MapRenderStats& left,
                const DomDownToolPreview::MapRenderStats& right)
{
    return left.visibleTiles == right.visibleTiles && left.readyTiles == right.readyTiles &&
        left.cache.residentTiles == right.cache.residentTiles &&
        left.cache.pendingTiles == right.cache.pendingTiles &&
        left.cache.cpuBytes == right.cache.cpuBytes &&
        left.cache.gpuBytes == right.cache.gpuBytes &&
        left.downloads.queuedTiles == right.downloads.queuedTiles &&
        left.downloads.activeTiles == right.downloads.activeTiles &&
        left.downloads.failedTiles == right.downloads.failedTiles;
}

std::string escapeHtml(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

std::optional<size_t> searchResultIndex(std::string_view id)
{
    constexpr std::string_view kPrefix = "search-result-";
    if (!id.starts_with(kPrefix))
    {
        return std::nullopt;
    }

    size_t index = 0;
    const std::string_view indexText = id.substr(kPrefix.size());
    const auto parseResult =
        std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
    if (parseResult.ec != std::errc{} || parseResult.ptr != indexText.data() + indexText.size())
    {
        return std::nullopt;
    }
    return index;
}

std::optional<uint64_t> exportGisGroupId(std::string_view id)
{
    constexpr std::string_view kPrefix = "export-gis-group-";
    if (!id.starts_with(kPrefix))
    {
        return std::nullopt;
    }

    uint64_t groupId = 0;
    const std::string_view value = id.substr(kPrefix.size());
    const auto result = std::from_chars(value.data(), value.data() + value.size(), groupId);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    {
        return std::nullopt;
    }
    return groupId;
}

std::optional<std::string> taskIdFromCancelButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-cancel-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<double> parseFiniteDouble(std::string_view value)
{
    try
    {
        size_t parsedLength = 0;
        const double result = std::stod(std::string(value), &parsedLength);
        if (parsedLength != value.size() || !std::isfinite(result))
        {
            return std::nullopt;
        }
        return result;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<uint32_t> parseUint32(std::string_view value)
{
    uint32_t result = 0;
    const auto parseResult =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (parseResult.ec != std::errc{} ||
        parseResult.ptr != value.data() + value.size())
    {
        return std::nullopt;
    }
    return result;
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisCategoryFromValue(
    std::string_view value)
{
    static const std::pair<std::string_view, DomDownToolPreview::GisOverlayCategory> categories[] = {
        {"none", DomDownToolPreview::GisOverlayCategory::None},
        {"all", DomDownToolPreview::GisOverlayCategory::All},
        {"road", DomDownToolPreview::GisOverlayCategory::Road},
        {"water", DomDownToolPreview::GisOverlayCategory::Water},
        {"green", DomDownToolPreview::GisOverlayCategory::Green},
        {"highway", DomDownToolPreview::GisOverlayCategory::Highway},
        {"subway", DomDownToolPreview::GisOverlayCategory::Subway},
        {"building", DomDownToolPreview::GisOverlayCategory::Building},
        {"point", DomDownToolPreview::GisOverlayCategory::Point},
        {"boundary", DomDownToolPreview::GisOverlayCategory::Boundary},
        {"line", DomDownToolPreview::GisOverlayCategory::Boundary},
        {"other", DomDownToolPreview::GisOverlayCategory::Other}};
    for (const auto& [name, category] : categories)
    {
        if (name == value)
        {
            return category;
        }
    }
    return std::nullopt;
}

std::string gisCategoryValue(DomDownToolPreview::GisOverlayCategory category)
{
    switch (category)
    {
    case DomDownToolPreview::GisOverlayCategory::None:
        return "none";
    case DomDownToolPreview::GisOverlayCategory::All:
        return "all";
    case DomDownToolPreview::GisOverlayCategory::Road:
        return "road";
    case DomDownToolPreview::GisOverlayCategory::Water:
        return "water";
    case DomDownToolPreview::GisOverlayCategory::Green:
        return "green";
    case DomDownToolPreview::GisOverlayCategory::Highway:
        return "highway";
    case DomDownToolPreview::GisOverlayCategory::Subway:
        return "subway";
    case DomDownToolPreview::GisOverlayCategory::Building:
        return "building";
    case DomDownToolPreview::GisOverlayCategory::Point:
        return "point";
    case DomDownToolPreview::GisOverlayCategory::Boundary:
        return "boundary";
    case DomDownToolPreview::GisOverlayCategory::Other:
        return "other";
    }
    return "none";
}

DownTool::AmapVectorRenderCategory exportCategory(
    DomDownToolPreview::GisOverlayCategory category)
{
    switch (category)
    {
    case DomDownToolPreview::GisOverlayCategory::Point:
        return DownTool::AmapVectorRenderCategory::Point;
    case DomDownToolPreview::GisOverlayCategory::Road:
        return DownTool::AmapVectorRenderCategory::Road;
    case DomDownToolPreview::GisOverlayCategory::Green:
        return DownTool::AmapVectorRenderCategory::Green;
    case DomDownToolPreview::GisOverlayCategory::Highway:
        return DownTool::AmapVectorRenderCategory::Highway;
    case DomDownToolPreview::GisOverlayCategory::Subway:
        return DownTool::AmapVectorRenderCategory::Subway;
    case DomDownToolPreview::GisOverlayCategory::Building:
        return DownTool::AmapVectorRenderCategory::Building;
    case DomDownToolPreview::GisOverlayCategory::Water:
        return DownTool::AmapVectorRenderCategory::Water;
    case DomDownToolPreview::GisOverlayCategory::Boundary:
        return DownTool::AmapVectorRenderCategory::Boundary;
    case DomDownToolPreview::GisOverlayCategory::None:
    case DomDownToolPreview::GisOverlayCategory::All:
    case DomDownToolPreview::GisOverlayCategory::Other:
        return DownTool::AmapVectorRenderCategory::Other;
    }
    return DownTool::AmapVectorRenderCategory::Other;
}

DomDownToolPreview::GisOverlayCategory previewCategory(
    DownTool::AmapVectorRenderCategory category)
{
    switch (category)
    {
    case DownTool::AmapVectorRenderCategory::Point:
        return DomDownToolPreview::GisOverlayCategory::Point;
    case DownTool::AmapVectorRenderCategory::Road:
        return DomDownToolPreview::GisOverlayCategory::Road;
    case DownTool::AmapVectorRenderCategory::Green:
        return DomDownToolPreview::GisOverlayCategory::Green;
    case DownTool::AmapVectorRenderCategory::Highway:
        return DomDownToolPreview::GisOverlayCategory::Highway;
    case DownTool::AmapVectorRenderCategory::Subway:
        return DomDownToolPreview::GisOverlayCategory::Subway;
    case DownTool::AmapVectorRenderCategory::Building:
        return DomDownToolPreview::GisOverlayCategory::Building;
    case DownTool::AmapVectorRenderCategory::Water:
        return DomDownToolPreview::GisOverlayCategory::Water;
    case DownTool::AmapVectorRenderCategory::Boundary:
        return DomDownToolPreview::GisOverlayCategory::Boundary;
    case DownTool::AmapVectorRenderCategory::All:
        return DomDownToolPreview::GisOverlayCategory::All;
    case DownTool::AmapVectorRenderCategory::Other:
        return DomDownToolPreview::GisOverlayCategory::Other;
    }
    return DomDownToolPreview::GisOverlayCategory::Other;
}

std::string exportCategoryLabel(DownTool::AmapVectorRenderCategory category)
{
    switch (category)
    {
    case DownTool::AmapVectorRenderCategory::Point:
        return "点";
    case DownTool::AmapVectorRenderCategory::Road:
        return "道路";
    case DownTool::AmapVectorRenderCategory::Green:
        return "绿地";
    case DownTool::AmapVectorRenderCategory::Highway:
        return "高速";
    case DownTool::AmapVectorRenderCategory::Subway:
        return "地铁";
    case DownTool::AmapVectorRenderCategory::Building:
        return "建筑";
    case DownTool::AmapVectorRenderCategory::Water:
        return "水";
    case DownTool::AmapVectorRenderCategory::Boundary:
        return "边界";
    case DownTool::AmapVectorRenderCategory::Other:
        return "其他";
    case DownTool::AmapVectorRenderCategory::All:
        return "全部";
    }
    return "GIS";
}

constexpr std::array kExportCategories = {
    DownTool::AmapVectorRenderCategory::Road,
    DownTool::AmapVectorRenderCategory::Water,
    DownTool::AmapVectorRenderCategory::Green,
    DownTool::AmapVectorRenderCategory::Highway,
    DownTool::AmapVectorRenderCategory::Subway,
    DownTool::AmapVectorRenderCategory::Building,
    DownTool::AmapVectorRenderCategory::Point,
    DownTool::AmapVectorRenderCategory::Boundary,
    DownTool::AmapVectorRenderCategory::Other};

DomDownToolPreview::GisOverlayCategories allGisCategories()
{
    return {DomDownToolPreview::GisOverlayCategory::Road,
            DomDownToolPreview::GisOverlayCategory::Water,
            DomDownToolPreview::GisOverlayCategory::Green,
            DomDownToolPreview::GisOverlayCategory::Highway,
            DomDownToolPreview::GisOverlayCategory::Subway,
            DomDownToolPreview::GisOverlayCategory::Building,
            DomDownToolPreview::GisOverlayCategory::Point,
            DomDownToolPreview::GisOverlayCategory::Boundary,
            DomDownToolPreview::GisOverlayCategory::Other};
}

struct GisCategoryControl
{
    std::string_view id;
    std::string_view value;
    std::string_view label;
    DomDownToolPreview::GisOverlayCategory category;
};

constexpr std::array kGisCategoryControls = {
    GisCategoryControl{"gis-overlay-road", "road", "道路",
                       DomDownToolPreview::GisOverlayCategory::Road},
    GisCategoryControl{"gis-overlay-water", "water", "水",
                       DomDownToolPreview::GisOverlayCategory::Water},
    GisCategoryControl{"gis-overlay-green", "green", "绿地",
                       DomDownToolPreview::GisOverlayCategory::Green},
    GisCategoryControl{"gis-overlay-highway", "highway", "高速",
                       DomDownToolPreview::GisOverlayCategory::Highway},
    GisCategoryControl{"gis-overlay-subway", "subway", "地铁",
                       DomDownToolPreview::GisOverlayCategory::Subway},
    GisCategoryControl{"gis-overlay-building", "building", "建筑",
                       DomDownToolPreview::GisOverlayCategory::Building},
    GisCategoryControl{"gis-overlay-point", "point", "点",
                       DomDownToolPreview::GisOverlayCategory::Point},
    GisCategoryControl{"gis-overlay-boundary", "boundary", "边界",
                       DomDownToolPreview::GisOverlayCategory::Boundary},
    GisCategoryControl{"gis-overlay-other", "other", "其他",
                       DomDownToolPreview::GisOverlayCategory::Other}};

std::vector<std::string> gisSelectionValues(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    if (categories.empty())
    {
        return {"none"};
    }
    if (categories == allGisCategories())
    {
        return {"all"};
    }

    std::vector<std::string> values;
    values.reserve(categories.size());
    for (const DomDownToolPreview::GisOverlayCategory category : categories)
    {
        values.push_back(gisCategoryValue(category));
    }
    return values;
}

std::string gisCategoriesValue(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    const std::vector<std::string> values = gisSelectionValues(categories);
    std::string result;
    for (const std::string& value : values)
    {
        if (!result.empty())
        {
            result += ',';
        }
        result += value;
    }
    return result;
}

std::string gisCategoriesSummary(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    if (categories.empty())
    {
        return "无";
    }
    if (categories == allGisCategories())
    {
        return "全部";
    }
    if (categories.size() > 2)
    {
        return fmt::format("已选 {} 项", categories.size());
    }

    std::string summary;
    for (const GisCategoryControl& control : kGisCategoryControls)
    {
        if (!categories.contains(control.category))
        {
            continue;
        }
        if (!summary.empty())
        {
            summary += "、";
        }
        summary += control.label;
    }
    return summary;
}

void setCheckboxChecked(skui::Runtime& runtime, std::string_view id, bool checked)
{
    if (checked)
    {
        runtime.setAttributeById(id, "checked", "");
        return;
    }
    runtime.removeAttributeById(id, "checked");
}

void syncGisCheckboxes(skui::Runtime& runtime,
                       const DomDownToolPreview::GisOverlayCategories& categories)
{
    setCheckboxChecked(runtime, "gis-overlay-none", categories.empty());
    setCheckboxChecked(runtime, "gis-overlay-all", categories == allGisCategories());
    for (const GisCategoryControl& control : kGisCategoryControls)
    {
        setCheckboxChecked(runtime, control.id, categories.contains(control.category));
    }
    runtime.setTextById("gis-overlay-summary", gisCategoriesSummary(categories));
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisColorCategoryFromId(
    std::string_view id)
{
    constexpr std::string_view prefix = "gis-color-";
    if (!id.starts_with(prefix))
    {
        return std::nullopt;
    }
    return gisCategoryFromValue(id.substr(prefix.size()));
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisColorTargetCategoryFromId(
    std::string_view id)
{
    constexpr std::string_view prefix = "gis-color-target-";
    if (!id.starts_with(prefix))
    {
        return std::nullopt;
    }
    const std::optional<DomDownToolPreview::GisOverlayCategory> category =
        gisCategoryFromValue(id.substr(prefix.size()));
    if (!category.has_value() ||
        *category == DomDownToolPreview::GisOverlayCategory::None ||
        *category == DomDownToolPreview::GisOverlayCategory::All)
    {
        return std::nullopt;
    }
    return category;
}

std::optional<DownTool::AmapVectorColor> parseGisColor(std::string_view value)
{
    if (!value.empty() && value.front() == '#')
    {
        value.remove_prefix(1);
    }
    if (value.size() != 6)
    {
        return std::nullopt;
    }
    const auto parseByte = [](std::string_view text) -> std::optional<uint8_t>
    {
        uint32_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 255)
        {
            return std::nullopt;
        }
        return static_cast<uint8_t>(parsed);
    };
    const std::optional<uint8_t> red = parseByte(value.substr(0, 2));
    const std::optional<uint8_t> green = parseByte(value.substr(2, 2));
    const std::optional<uint8_t> blue = parseByte(value.substr(4, 2));
    if (!red || !green || !blue)
    {
        return std::nullopt;
    }
    return DownTool::AmapVectorColor{*red, *green, *blue, 255};
}

std::string gisColorHex(const DownTool::AmapVectorColor& color)
{
    return fmt::format("#{:02X}{:02X}{:02X}", static_cast<unsigned>(color.red),
                       static_cast<unsigned>(color.green),
                       static_cast<unsigned>(color.blue));
}

struct HsvColor
{
    double hue = 0.;
    double saturation = 0.;
    double brightness = 0.;
};

HsvColor toHsv(const DownTool::AmapVectorColor& color)
{
    const double red = static_cast<double>(color.red) / 255.;
    const double green = static_cast<double>(color.green) / 255.;
    const double blue = static_cast<double>(color.blue) / 255.;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;

    double hue = 0.;
    if (delta > 0.)
    {
        if (maximum == red)
        {
            hue = 60. * std::fmod((green - blue) / delta, 6.);
        }
        else if (maximum == green)
        {
            hue = 60. * ((blue - red) / delta + 2.);
        }
        else
        {
            hue = 60. * ((red - green) / delta + 4.);
        }
        if (hue < 0.)
        {
            hue += 360.;
        }
    }

    return HsvColor{hue, maximum > 0. ? delta / maximum : 0., maximum};
}

DownTool::AmapVectorColor fromHsv(const HsvColor& color)
{
    const double hue = std::fmod(std::max(0., color.hue), 360.);
    const double saturation = std::clamp(color.saturation, 0., 1.);
    const double brightness = std::clamp(color.brightness, 0., 1.);
    const double chroma = brightness * saturation;
    const double intermediate =
        chroma * (1. - std::abs(std::fmod(hue / 60., 2.) - 1.));

    double red = 0.;
    double green = 0.;
    double blue = 0.;
    if (hue < 60.)
    {
        red = chroma;
        green = intermediate;
    }
    else if (hue < 120.)
    {
        red = intermediate;
        green = chroma;
    }
    else if (hue < 180.)
    {
        green = chroma;
        blue = intermediate;
    }
    else if (hue < 240.)
    {
        green = intermediate;
        blue = chroma;
    }
    else if (hue < 300.)
    {
        red = intermediate;
        blue = chroma;
    }
    else
    {
        red = chroma;
        blue = intermediate;
    }

    const double match = brightness - chroma;
    const auto channel = [match](double value)
    {
        return static_cast<uint8_t>(
            std::clamp(std::lround((value + match) * 255.), 0L, 255L));
    };
    return DownTool::AmapVectorColor{channel(red), channel(green), channel(blue), 255};
}

} // namespace

DomDownToolPreview::PreviewController::PreviewController()
    : renderer_([this] { requestRedraw(); }),
      placeSearch_([this] { scheduleSearchRefresh(); },
                   [this](const std::string& url) { return webViewSearch_.get(url); },
                   [this] { webViewSearch_.cancelCurrent(); }),
      exportTaskManager_([this] { scheduleExportTaskRefresh(); })
{
}

void DomDownToolPreview::PreviewController::attachRuntime(skui::Runtime& runtime)
{
    writeSearchTrace("Controller", "runtime_attached");
    runtime_ = &runtime;
    selectGisColorCategory(gisColorPaletteCategory_);
    runtime.setElementEventCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.id == "export-radius" &&
                event.type == skui::ElementEventType::Input)
            {
                previewRadiusValue_ = event.value;
                const std::optional<double> radius = parseFiniteDouble(event.value);
                if (radius.has_value() && *radius >= 0.)
                {
                    renderer_.SetExportRadiusMeters(*radius);
                    requestRedraw();
                }
                return;
            }

            if (event.type == skui::ElementEventType::Input)
            {
                if (event.id == "export-lon")
                {
                    exportLongitudeValue_ = event.value;
                    return;
                }
                if (event.id == "export-lat")
                {
                    exportLatitudeValue_ = event.value;
                    return;
                }
                if (event.id == "export-config-radius")
                {
                    exportRadiusValue_ = event.value;
                    return;
                }
                if (event.id == "export-zoom")
                {
                    exportZoomValue_ = event.value;
                    return;
                }
                if (event.id == "export-satellite-output")
                {
                    exportSatelliteOutputValue_ = event.value;
                    return;
                }
                if (event.id == "export-gis-output")
                {
                    exportGisOutputValue_ = event.value;
                    return;
                }
                if (event.id == "export-merged-resolution")
                {
                    exportMergedResolutionValue_ = event.value;
                    return;
                }
            }

            if (event.type == skui::ElementEventType::Change)
            {
                if (event.id == "export-provider-select")
                {
                    exportMapSourceId_ = event.value;
                    const std::vector<MapSourceSummary> sources = renderer_.sources();
                    const auto source = std::find_if(
                        sources.begin(), sources.end(),
                        [this](const MapSourceSummary& item)
                        { return item.id == exportMapSourceId_; });
                    if (source != sources.end())
                    {
                        exportSatelliteOutputValue_ = source->cacheRoot.empty() ?
                            std::string{} : pathToUtf8(
                                source->cacheRoot /
                                std::filesystem::u8path("Google_墨卡托"));
                        runtime.setValueById(
                            "export-satellite-output", exportSatelliteOutputValue_);
                    }
                    return;
                }
                if (event.id == "export-merged-format")
                {
                    exportMergedFormat_ = event.value;
                    return;
                }
                if (event.id == "export-fbx-source")
                {
                    exportFbxSource_ = event.value == "gis" ?
                        ExportFbxSource::Gis : ExportFbxSource::Satellite;
                    return;
                }
                if (event.id == "export-fbx-size")
                {
                    exportFbxSizeValue_ = event.value;
                    return;
                }
                if (event.id == "export-satellite-tiles")
                {
                    exportSatelliteTiles_ = event.checked;
                    return;
                }
                if (event.id == "export-satellite-merged")
                {
                    exportSatelliteMergedImage_ = event.checked;
                    return;
                }
                if (event.id == "export-gis-tiles")
                {
                    exportGisTiles_ = event.checked;
                    return;
                }
                if (event.id == "export-gis-merged")
                {
                    exportGisMergedImage_ = event.checked;
                    return;
                }
                if (event.id == "export-fbx")
                {
                    exportFbx_ = event.checked;
                    return;
                }
                if (event.id == "export-building-height-colors")
                {
                    exportBuildingHeightColors_ = event.checked;
                    return;
                }
            }

            if (event.type == skui::ElementEventType::MouseDown &&
                event.button == skui::MouseButton::Right)
            {
                if (const std::optional<uint64_t> groupId = exportGisGroupId(event.id))
                {
                    HandleExportGisGroupContextAction(*groupId);
                    UpdateExportUi(runtime);
                    requestRedraw();
                    return;
                }
            }

            if (event.type == skui::ElementEventType::MouseDown && gisOverlayPopupOpen_ &&
                event.id != "gis-overlay-toggle" &&
                !isPointInside(event.x, event.y, kGisOverlayPanelLeft, kGisOverlayPanelTop,
                               kGisOverlayPanelWidth, kGisOverlayPanelHeight))
            {
                gisOverlayPopupOpen_ = false;
                updateUi(runtime);
                requestRedraw();
            }

            if (event.id == "place-search-input" &&
                event.type == skui::ElementEventType::Input)
            {
                searchQuery_ = event.value;
                pendingLocationRequest_.reset();
                writeSearchTrace("Controller", "input_changed", "query=" + searchQuery_);
                if (searchQuery_.find_first_not_of(" \t\r\n") == std::string::npos)
                {
                    placeSearch_.cancelPending();
                    searchResults_.clear();
                    highlightedSearchResult_.reset();
                    searchState_ = SearchState::Idle;
                    searchPopupOpen_ = false;
                    updateSearchUi(runtime);
                }
                else
                {
                    searchState_ = SearchState::Loading;
                    searchMessage_.clear();
                    searchPopupOpen_ = true;
                    const uint64_t requestId =
                        placeSearch_.submitSuggestions(searchQuery_, renderer_.center());
                    writeSearchTrace("Controller", "suggestions_requested",
                                     fmt::format("id={}", requestId));
                    updateSearchUi(runtime);
                }
                requestRedraw();
                return;
            }

            if (event.id == "place-search-input" &&
                event.type == skui::ElementEventType::MouseDown)
            {
                writeSearchTrace("Controller", "input_mouse_down",
                                 fmt::format("x={} y={}", event.x, event.y));
                if (!searchResults_.empty() || searchState_ == SearchState::Error)
                {
                    searchPopupOpen_ = true;
                    updateSearchUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.id == "provider-select" &&
                event.type == skui::ElementEventType::MouseDown)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_mouse_down id={} value={} x={} y={}",
                                             event.id, event.value, event.x, event.y));
                return;
            }

            if (event.id == "provider-select" && event.type == skui::ElementEventType::Change)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_change id={} value={}", event.id,
                                             event.value));
                if (renderer_.selectSource(event.value))
                {
                    updateUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.type == skui::ElementEventType::Change &&
                event.id.starts_with("gis-overlay-"))
            {
                const std::optional<GisOverlayCategory> category =
                    gisCategoryFromValue(event.value);
                if (!category.has_value())
                {
                    return;
                }

                GisOverlayCategories categories = renderer_.GetGisOverlayCategories();
                if (*category == GisOverlayCategory::None)
                {
                    if (event.checked)
                    {
                        categories.clear();
                    }
                }
                else if (*category == GisOverlayCategory::All)
                {
                    categories = event.checked ? allGisCategories() : GisOverlayCategories{};
                }
                else if (event.checked)
                {
                    categories.insert(*category);
                }
                else
                {
                    categories.erase(*category);
                }

                writeSearchTrace("Controller",
                                 fmt::format("checkbox_change id={} checked={} values={}",
                                             event.id, event.checked,
                                             gisCategoriesValue(categories)));
                renderer_.SetGisOverlayCategories(categories);
                syncGisCheckboxes(runtime, categories);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.type == skui::ElementEventType::MouseDown)
            {
                if (const std::optional<GisOverlayCategory> target =
                        gisColorTargetCategoryFromId(event.id))
                {
                    selectGisColorCategory(*target);
                    updateUi(runtime);
                    requestRedraw();
                    return;
                }
                if (const std::optional<GisOverlayCategory> category =
                        gisColorCategoryFromId(event.id))
                {
                    selectGisColorCategory(*category);
                    updateUi(runtime);
                    requestRedraw();
                    return;
                }
            }

            const bool colorFieldEvent = event.id == "gis-color-picker-field" &&
                (event.type == skui::ElementEventType::MouseDown ||
                 event.type == skui::ElementEventType::MouseUp ||
                 (event.type == skui::ElementEventType::MouseMove &&
                  gisColorFieldDragging_));
            if (colorFieldEvent)
            {
                gisColorFieldDragging_ =
                    event.type != skui::ElementEventType::MouseUp;
                gisColorHueDragging_ = false;
                gisColorSaturation_ = std::clamp(
                    (static_cast<double>(event.x) - kGisColorFieldLeft) /
                        kGisColorFieldWidth,
                    0., 1.);
                gisColorBrightness_ = 1. - std::clamp(
                    (static_cast<double>(event.y) - kGisColorFieldTop) /
                        kGisColorFieldHeight,
                    0., 1.);
                applyGisColorPicker(runtime);
                return;
            }

            const bool colorHueEvent = event.id == "gis-color-picker-hue" &&
                (event.type == skui::ElementEventType::MouseDown ||
                 event.type == skui::ElementEventType::MouseUp ||
                 (event.type == skui::ElementEventType::MouseMove &&
                  gisColorHueDragging_));
            if (colorHueEvent)
            {
                gisColorHueDragging_ = event.type != skui::ElementEventType::MouseUp;
                gisColorFieldDragging_ = false;
                gisColorHue_ = 360. * std::clamp(
                    (static_cast<double>(event.y) - kGisColorHueTop) /
                        kGisColorHueHeight,
                    0., 1.);
                if (gisColorHue_ >= 360.)
                {
                    gisColorHue_ = 0.;
                }
                applyGisColorPicker(runtime);
                return;
            }

            if (event.type == skui::ElementEventType::Input)
            {
                const std::optional<GisOverlayCategory> category = gisColorCategoryFromId(event.id);
                if (category.has_value())
                {
                    const std::optional<DownTool::AmapVectorColor> color =
                        parseGisColor(event.value);
                    if (color.has_value())
                    {
                        renderer_.SetGisOverlayColor(*category, *color);
                        selectGisColorCategory(*category);
                        updateUi(runtime);
                        requestRedraw();
                    }
                    return;
                }
            }

            if (event.type != skui::ElementEventType::Click)
            {
                return;
            }

            if (event.id == "open-export-page" || event.id == "tasks-new-export")
            {
                schedulePageChange(Page::Export, runtime);
                return;
            }
            if (event.id == "open-task-page" || event.id == "export-open-tasks")
            {
                schedulePageChange(Page::Tasks, runtime);
                return;
            }
            if (event.id == "export-back-preview" || event.id == "export-cancel" ||
                event.id == "tasks-back-preview")
            {
                schedulePageChange(Page::Preview, runtime);
                return;
            }
            if (event.id == "export-submit")
            {
                SubmitExport(runtime);
                return;
            }
            if (event.id == "tasks-clear-finished")
            {
                exportTaskManager_.ClearFinished();
                UpdateTaskUi(runtime);
                requestRedraw();
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromCancelButton(event.id))
            {
                exportTaskManager_.Cancel(*taskId);
                UpdateTaskUi(runtime);
                requestRedraw();
                return;
            }
            if (const std::optional<uint64_t> groupId = exportGisGroupId(event.id))
            {
                ToggleExportGisGroup(*groupId);
                UpdateExportUi(runtime);
                requestRedraw();
                return;
            }

            if (const std::optional<GisOverlayCategory> target =
                    gisColorTargetCategoryFromId(event.id))
            {
                selectGisColorCategory(*target);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "gis-color-eyedropper")
            {
                setGisColorEyedropperActive(!gisColorEyedropperActive_);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "gis-color-reset")
            {
                setGisColorEyedropperActive(false);
                renderer_.ResetGisOverlayColors();
                selectGisColorCategory(gisColorPaletteCategory_);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "zoom-in")
            {
                renderer_.zoomAt(static_cast<double>(runtime.width()) * 0.5,
                                 static_cast<double>(runtime.height()) * 0.5, 1.);
            }
            else if (event.id == "zoom-out")
            {
                renderer_.zoomAt(static_cast<double>(runtime.width()) * 0.5,
                                 static_cast<double>(runtime.height()) * 0.5, -1.);
            }
            else if (event.id == "reload-tiles")
            {
                renderer_.clearCache();
            }
            else if (event.id == "gis-color-toggle")
            {
                gisColorPopupOpen_ = !gisColorPopupOpen_;
                gisOverlayPopupOpen_ = false;
                if (!gisColorPopupOpen_)
                {
                    setGisColorEyedropperActive(false);
                }
            }
            else if (event.id == "gis-overlay-toggle")
            {
                gisOverlayPopupOpen_ = !gisOverlayPopupOpen_;
                gisColorPopupOpen_ = false;
                setGisColorEyedropperActive(false);
                writeSearchTrace("Controller", "gis_popup_toggle",
                                 fmt::format("open={}", gisOverlayPopupOpen_));
            }
            else if (event.id == "view-span-decrease")
            {
                renderer_.adjustViewSpan(-0.5);
            }
            else if (event.id == "view-span-increase")
            {
                renderer_.adjustViewSpan(0.5);
            }
            else if (event.id == "place-search-submit")
            {
                submitPlaceSearch(runtime);
            }
            else if (event.id == "place-search-clear")
            {
                clearPlaceSearch(runtime);
            }
            else if (const std::optional<size_t> resultIndex = searchResultIndex(event.id))
            {
                selectPlaceSearchResult(runtime, *resultIndex);
            }
            else
            {
                return;
            }

            updateUi(runtime);
            requestRedraw();
        });
    runtime.setElementKeyDownCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.key == VK_ESCAPE &&
                (gisOverlayPopupOpen_ || gisColorPopupOpen_ ||
                 gisColorEyedropperActive_))
            {
                gisOverlayPopupOpen_ = false;
                gisColorPopupOpen_ = false;
                setGisColorEyedropperActive(false);
                updateUi(runtime);
                requestRedraw();
                return true;
            }
            if (event.id != "place-search-input")
            {
                return false;
            }
            if (event.key == VK_RETURN)
            {
                if (searchPopupOpen_ && highlightedSearchResult_.has_value() &&
                    *highlightedSearchResult_ < searchResults_.size())
                {
                    selectPlaceSearchResult(runtime, *highlightedSearchResult_);
                    return true;
                }
                submitPlaceSearch(runtime);
                return true;
            }
            if (event.key == VK_DOWN)
            {
                moveSearchHighlight(runtime, 1);
                return true;
            }
            if (event.key == VK_UP)
            {
                moveSearchHighlight(runtime, -1);
                return true;
            }
            if (event.key == VK_ESCAPE)
            {
                placeSearch_.cancelPending();
                pendingLocationRequest_.reset();
                searchPopupOpen_ = false;
                updateSearchUi(runtime);
                requestRedraw();
                return true;
            }
            return false;
        });
    updateUi(runtime);
}

void DomDownToolPreview::PreviewController::resizeRuntime(skui::Runtime& runtime)
{
    updateUi(runtime);
}

void DomDownToolPreview::PreviewController::draw(SkCanvas& canvas, int width, int height,
                                                 float dpiScale)
{
    if (page_ == Page::Preview)
    {
        renderer_.draw(canvas, width, height, dpiScale);
    }
    else
    {
        canvas.clear(SkColorSetRGB(25, 29, 28));
    }

    if (runtime_)
    {
        SkAutoCanvasRestore restore(&canvas, true);
        canvas.clipRect(SkRect::MakeLTRB(0.F, 0.F, static_cast<float>(width),
                                         static_cast<float>(height)),
                        SkClipOp::kIntersect, false);
        skui::RuntimeRenderRegion overlayRegion;
        overlayRegion.drawBackground = false;
        runtime_->renderInto(canvas, overlayRegion);
    }

    scheduleUiRefresh();
}

bool DomDownToolPreview::PreviewController::handleWindowMessage(HWND window, UINT message,
                                                                WPARAM wParam, LPARAM lParam,
                                                                skui::Runtime& runtime)
{
    if (message == WM_DESTROY && gisColorEyedropperActive_)
    {
        KillTimer(window, kPreviewEyedropperTimerId);
        gisColorEyedropperActive_ = false;
    }
    window_.store(message == WM_DESTROY ? nullptr : window, std::memory_order_relaxed);
    if (message == WM_DESTROY)
    {
        pendingMapUiPoint_.reset();
        immediateMapRedrawPosted_ = false;
        mapUiRefreshPosted_ = false;
        mapDragRedrawThrottle_.reset();
        mapHoverRefreshThrottle_.reset();
    }
    if (message == kPreviewImmediateMapRedrawMessage)
    {
        immediateMapRedrawPosted_ = false;
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOCHILDREN | RDW_UPDATENOW);
        return false;
    }
    if (message == kPreviewRedrawMessage)
    {
        return true;
    }
    if (message == kPreviewUiRefreshMessage)
    {
        updateUi(runtime);
        return true;
    }
    if (message == kPreviewSearchRefreshMessage)
    {
        writeSearchTrace("Controller", "search_refresh_message_received");
        processPlaceSearchResponses(runtime);
        return true;
    }
    if (message == kPreviewExportTaskRefreshMessage)
    {
        exportTaskRefreshPosted_.store(false, std::memory_order_relaxed);
        UpdateTaskUi(runtime);
        requestRedraw();
        return true;
    }
    if (message == kPreviewPageChangeMessage)
    {
        switch (static_cast<Page>(wParam))
        {
        case Page::Preview:
            OpenPreviewPage(runtime);
            break;
        case Page::Export:
            OpenExportPage(runtime);
            break;
        case Page::Tasks:
            OpenTaskPage(runtime);
            break;
        }
        return true;
    }
    if (message == kPreviewMapUiRefreshMessage)
    {
        mapUiRefreshPosted_ = false;
        if (!pendingMapUiPoint_.has_value())
        {
            return false;
        }

        const POINT point = *pendingMapUiPoint_;
        pendingMapUiPoint_.reset();
        updateMapInteractionUi(runtime, point);
        return false;
    }
    if (message == WM_TIMER && wParam == kPreviewEyedropperTimerId)
    {
        if (gisColorEyedropperActive_)
        {
            sampleGisColorAtScreenCursor(runtime);
            const bool cancel = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            const bool confirm = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (cancel || confirm)
            {
                setGisColorEyedropperActive(false);
                updateUi(runtime);
                requestRedraw();
            }
        }
        return true;
    }

    if (page_ != Page::Preview)
    {
        dragging_ = false;
        return false;
    }

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE &&
        (gisOverlayPopupOpen_ || gisColorPopupOpen_ ||
         gisColorEyedropperActive_))
    {
        gisOverlayPopupOpen_ = false;
        gisColorPopupOpen_ = false;
        setGisColorEyedropperActive(false);
        updateUi(runtime);
        requestRedraw();
        return true;
    }

    if (gisColorEyedropperActive_)
    {
        if (message == WM_SETCURSOR)
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return true;
        }
        if (message == WM_MOUSEMOVE)
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            sampleGisColorAtScreenCursor(runtime);
            return true;
        }
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN)
        {
            return true;
        }
        if (message == WM_RBUTTONUP)
        {
            setGisColorEyedropperActive(false);
            updateUi(runtime);
            requestRedraw();
            return true;
        }
        if (message == WM_LBUTTONUP)
        {
            sampleGisColorAtScreenCursor(runtime);
            setGisColorEyedropperActive(false);
            updateUi(runtime);
            requestRedraw();
            return true;
        }
    }

    if (searchPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isSearchInteractionPoint(runtime, point.x, point.y))
        {
            return true;
        }
        if (message == WM_LBUTTONDOWN)
        {
            placeSearch_.cancelPending();
            pendingLocationRequest_.reset();
            searchPopupOpen_ = false;
            updateSearchUi(runtime);
        }
    }

    if (gisOverlayPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isGisOverlayInteractionPoint(runtime, point.x, point.y))
        {
            return true;
        }
        if (message == WM_LBUTTONDOWN)
        {
            gisOverlayPopupOpen_ = false;
            updateUi(runtime);
            requestRedraw();
        }
    }

    if (gisColorPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isGisColorInteractionPoint(runtime, point.x, point.y))
        {
            return true;
        }
        if (message == WM_LBUTTONDOWN)
        {
            gisColorPopupOpen_ = false;
            updateUi(runtime);
            requestRedraw();
        }
    }

    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
        message == WM_KILLFOCUS)
    {
        dragging_ = false;
        gisColorFieldDragging_ = false;
        gisColorHueDragging_ = false;
        mapDragRedrawThrottle_.reset();
        if (message == WM_CANCELMODE && gisColorEyedropperActive_)
        {
            setGisColorEyedropperActive(false);
            updateUi(runtime);
            requestRedraw();
        }
        if (message == WM_KILLFOCUS && !gisColorEyedropperActive_ &&
            (gisOverlayPopupOpen_ || gisColorPopupOpen_))
        {
            gisOverlayPopupOpen_ = false;
            gisColorPopupOpen_ = false;
            updateUi(runtime);
            requestRedraw();
        }
        return false;
    }

    if (message == WM_LBUTTONUP && dragging_)
    {
        dragging_ = false;
        if (GetCapture() == window)
        {
            ReleaseCapture();
        }
        mapDragRedrawThrottle_.reset();
        scheduleImmediateMapRedraw(window);
        mapHoverRefreshThrottle_.reset();
        scheduleMapUiRefresh(window, clientPoint(lParam));
        return true;
    }

    if (message == WM_MOUSEWHEEL)
    {
        POINT point{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
        ScreenToClient(window, &point);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }

        const double wheelSteps = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        renderer_.zoomAt(point.x, point.y, wheelSteps * 0.25);
        updateCursor(runtime, point.x, point.y);
        updateUi(runtime);
        return true;
    }

    if (message == WM_LBUTTONDBLCLK)
    {
        const POINT point = clientPoint(lParam);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }
        renderer_.zoomAt(point.x, point.y, 1.);
        updateUi(runtime);
        return true;
    }

    if (message == WM_LBUTTONDOWN)
    {
        const POINT point = clientPoint(lParam);
        if (!isMapPoint(window, point.x, point.y))
        {
            dragging_ = false;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            return false;
        }
        dragging_ = true;
        mapDragRedrawThrottle_.reset();
        mapHoverRefreshThrottle_.reset();
        lastDragPoint_ = point;
        SetCapture(window);
        return true;
    }

    if (message == WM_MOUSEMOVE)
    {
        const POINT point = clientPoint(lParam);
        if (dragging_)
        {
            const double deltaX = static_cast<double>(point.x - lastDragPoint_.x);
            const double deltaY = static_cast<double>(point.y - lastDragPoint_.y);
            lastDragPoint_ = point;
            if (renderer_.panBy(deltaX, deltaY))
            {
                scheduleImmediateMapRedraw(window);
            }
            if (mapHoverRefreshThrottle_.shouldRefresh(
                    MapHoverRefreshThrottle::Clock::now()))
            {
                scheduleMapUiRefresh(window, point);
            }
            return true;
        }

        if (isMapPoint(window, point.x, point.y) &&
            mapHoverRefreshThrottle_.shouldRefresh(
                MapHoverRefreshThrottle::Clock::now()))
        {
            scheduleMapUiRefresh(window, point);
        }
    }

    return false;
}

bool DomDownToolPreview::PreviewController::isMapPoint(HWND window, int x, int y) const
{
    if (page_ != Page::Preview)
    {
        return false;
    }
    RECT client{};
    GetClientRect(window, &client);
    return x >= 0 && x < client.right && static_cast<double>(y) >= renderer_.toolbarHeight() &&
           static_cast<double>(y) <
               static_cast<double>(client.bottom) - renderer_.statusBarHeight();
}

bool DomDownToolPreview::PreviewController::isSearchInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.effectiveScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    const bool insideControl = logicalX >= kSearchControlLeft &&
        logicalX <= kSearchControlLeft + kSearchControlWidth && logicalY >= 0. && logicalY <= 52.;
    const bool insidePopup = searchPopupOpen_ && logicalX >= kSearchControlLeft &&
        logicalX <= kSearchControlLeft + kSearchPopupWidth && logicalY >= kSearchPopupTop &&
        logicalY <= kSearchPopupTop + kSearchPopupHeight;
    return insideControl || insidePopup;
}

bool DomDownToolPreview::PreviewController::isGisOverlayInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.effectiveScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    return isPointInside(logicalX, logicalY, kGisOverlayPanelLeft, kGisOverlayPanelTop,
                         kGisOverlayPanelWidth, kGisOverlayPanelHeight);
}

bool DomDownToolPreview::PreviewController::isGisColorInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.effectiveScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    return isPointInside(logicalX, logicalY, kGisColorPanelLeft, kGisColorPanelTop,
                         kGisColorPanelWidth, kGisColorPanelHeight);
}

void DomDownToolPreview::PreviewController::submitPlaceSearch(skui::Runtime& runtime)
{
    if (searchQuery_.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        clearPlaceSearch(runtime);
        return;
    }

    searchResults_.clear();
    highlightedSearchResult_.reset();
    searchMessage_.clear();
    pendingLocationRequest_.reset();
    searchState_ = SearchState::Loading;
    searchPopupOpen_ = true;
    const uint64_t requestId = placeSearch_.submitSearch(searchQuery_, renderer_.center());
    writeSearchTrace("Controller", "search_requested",
                     fmt::format("id={} query={}", requestId, searchQuery_));
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::clearPlaceSearch(skui::Runtime& runtime)
{
    writeSearchTrace("Controller", "search_cleared");
    placeSearch_.cancelPending();
    searchQuery_.clear();
    searchMessage_.clear();
    searchResults_.clear();
    highlightedSearchResult_.reset();
    pendingLocationRequest_.reset();
    searchState_ = SearchState::Idle;
    searchPopupOpen_ = false;
    renderer_.setLocationMarker(std::nullopt);
    runtime.setValueById("place-search-input", "");
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::selectPlaceSearchResult(skui::Runtime& runtime,
                                                                    size_t index)
{
    if (index >= searchResults_.size())
    {
        return;
    }

    const PlaceSearchResult& result = searchResults_[index];
    if (!result.coordinateWgs84.has_value())
    {
        const std::string resultId = result.id;
        const std::string resultName = result.name;
        const std::string resolutionQuery =
            result.name + (result.address.empty() ? "" : " " + result.address);
        searchQuery_ = result.name;
        runtime.setValueById("place-search-input", searchQuery_);
        searchResults_.clear();
        highlightedSearchResult_.reset();
        searchMessage_.clear();
        searchState_ = SearchState::Loading;
        searchPopupOpen_ = true;
        const uint64_t requestId =
            placeSearch_.submitSearch(resolutionQuery, renderer_.center());
        pendingLocationRequest_ = PendingLocationRequest{requestId, resultId, resultName};
        writeSearchTrace("Controller", "location_resolution_requested",
                         fmt::format("id={} query={} expected_id={} expected_name={}",
                                     requestId, resolutionQuery, resultId, resultName));
        updateSearchUi(runtime);
        requestRedraw();
        return;
    }

    pendingLocationRequest_.reset();
    renderer_.setView(*result.coordinateWgs84, result.recommendedZoom);
    renderer_.setLocationMarker(result.coordinateWgs84);
    placeSearch_.cancelPending();
    searchQuery_ = result.name;
    runtime.setValueById("place-search-input", searchQuery_);
    searchPopupOpen_ = false;
    writeSearchTrace(
        "Controller", "location_applied",
        fmt::format("name={} lon={:.6f} lat={:.6f} zoom={:.2f}", result.name,
                    result.coordinateWgs84->lon, result.coordinateWgs84->lat,
                    result.recommendedZoom));
    updateSearchUi(runtime);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::moveSearchHighlight(skui::Runtime& runtime,
                                                                 int direction)
{
    if (searchResults_.empty())
    {
        return;
    }

    const size_t resultCount = searchResults_.size();
    size_t nextIndex = highlightedSearchResult_.value_or(direction > 0 ? resultCount - 1 : 0);
    if (direction > 0)
    {
        nextIndex = (nextIndex + 1) % resultCount;
    }
    else
    {
        nextIndex = nextIndex == 0 ? resultCount - 1 : nextIndex - 1;
    }

    highlightedSearchResult_ = nextIndex;
    searchPopupOpen_ = true;
    updateSearchUi(runtime);
    runtime.scrollIntoViewById("search-result-" + std::to_string(nextIndex));
    requestRedraw();
}

void DomDownToolPreview::PreviewController::processPlaceSearchResponses(skui::Runtime& runtime)
{
    const std::vector<PlaceSearchResponse> responses = placeSearch_.takeCompleted();
    if (responses.empty())
    {
        return;
    }

    const PlaceSearchResponse& response = responses.back();
    writeSearchTrace(
        "Controller", "response_processing",
        fmt::format("id={} kind={} results={} error={}", response.requestId,
                    response.kind == PlaceSearchRequestKind::Suggestions ? "suggestions"
                                                                         : "search",
                    response.results.size(), response.error));
    const bool applyResolvedLocation = pendingLocationRequest_.has_value() &&
        pendingLocationRequest_->requestId == response.requestId;
    const std::optional<PendingLocationRequest> locationRequest =
        applyResolvedLocation ? pendingLocationRequest_ : std::nullopt;
    if (applyResolvedLocation)
    {
        pendingLocationRequest_.reset();
    }
    searchResults_ = response.results;
    searchMessage_ = response.error;
    if (applyResolvedLocation && response.succeeded())
    {
        const std::optional<size_t> resultIndex = findExactPlaceResultIndex(
            searchResults_, locationRequest->resultId, locationRequest->resultName);
        if (resultIndex.has_value())
        {
            writeSearchTrace("Controller", "location_resolution_complete",
                             fmt::format("id={} result_index={}", response.requestId,
                                         *resultIndex));
            selectPlaceSearchResult(runtime, *resultIndex);
            return;
        }

        writeSearchTrace(
            "Controller", "location_resolution_rejected",
            fmt::format("id={} expected_id={} expected_name={} candidates={}",
                        response.requestId, locationRequest->resultId,
                        locationRequest->resultName, searchResults_.size()));
        searchResults_.clear();
        searchMessage_ = "无法确认所选地点的坐标，请选择更具体的候选项。";
        searchState_ = SearchState::Error;
        highlightedSearchResult_.reset();
        searchPopupOpen_ = true;
        updateSearchUi(runtime);
        requestRedraw();
        return;
    }
    if (!response.succeeded())
    {
        searchState_ = SearchState::Error;
        highlightedSearchResult_.reset();
    }
    else if (searchResults_.empty())
    {
        searchState_ = SearchState::Empty;
        highlightedSearchResult_.reset();
    }
    else
    {
        searchState_ = SearchState::Results;
        highlightedSearchResult_ = 0;
    }
    searchPopupOpen_ = true;
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::updateSearchUi(skui::Runtime& runtime)
{
    runtime.setVisibleById("place-search-results", searchPopupOpen_);
    if (!searchPopupOpen_)
    {
        return;
    }

    std::string html;
    if (searchState_ == SearchState::Loading)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state\">正在搜索...</div>";
    }
    else if (searchState_ == SearchState::Empty)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state\">没有找到匹配地点</div>";
    }
    else if (searchState_ == SearchState::Error)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state search-error\">" +
            escapeHtml(searchMessage_) + "</div>";
    }
    else
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-result-list\">";
        for (size_t index = 0; index < searchResults_.size(); ++index)
        {
            const PlaceSearchResult& result = searchResults_[index];
            const std::string subtitle = result.district +
                (result.address.empty() ? "" : " · " + result.address);
            html += fmt::format(
                "<button id=\"search-result-{}\" class=\"search-result{}\" type=\"button\" "
                "data-action=\"select-place-result\">"
                "<span class=\"search-result-name\">{}</span>"
                "<span class=\"search-result-address\">{}</span></button>",
                index,
                highlightedSearchResult_.has_value() && *highlightedSearchResult_ == index
                    ? " selected"
                    : "",
                escapeHtml(result.name), escapeHtml(subtitle));
        }
        html += "</div>";
    }
    const bool replaced = runtime.replaceHtmlById("place-search-results-content", html);
    writeSearchTrace(
        "Controller", "search_ui_replaced",
        fmt::format("success={} state={} results={}", replaced,
                    static_cast<int>(searchState_), searchResults_.size()));
}

void DomDownToolPreview::PreviewController::selectGisColorCategory(
    GisOverlayCategory category)
{
    if (category == GisOverlayCategory::None || category == GisOverlayCategory::All)
    {
        return;
    }
    gisColorPaletteCategory_ = category;
    const HsvColor hsv = toHsv(renderer_.GetGisOverlayColor(category));
    gisColorHue_ = hsv.hue;
    gisColorSaturation_ = hsv.saturation;
    gisColorBrightness_ = hsv.brightness;
}

void DomDownToolPreview::PreviewController::applyGisColorPicker(skui::Runtime& runtime)
{
    const DownTool::AmapVectorColor color =
        fromHsv(HsvColor{gisColorHue_, gisColorSaturation_, gisColorBrightness_});
    renderer_.SetGisOverlayColor(gisColorPaletteCategory_, color);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::setGisColorEyedropperActive(bool active)
{
    gisColorEyedropperActive_ = active;
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window)
    {
        return;
    }
    if (active)
    {
        if (SetTimer(window, kPreviewEyedropperTimerId,
                     kPreviewEyedropperTimerIntervalMilliseconds, nullptr) == 0)
        {
            gisColorEyedropperActive_ = false;
        }
        return;
    }
    KillTimer(window, kPreviewEyedropperTimerId);
}

bool DomDownToolPreview::PreviewController::sampleGisColorAtScreenCursor(skui::Runtime& runtime)
{
    POINT screenPoint{};
    if (!GetCursorPos(&screenPoint))
    {
        return false;
    }
    const std::optional<DownTool::AmapVectorColor> color = screenPixelColor(screenPoint);
    if (!color.has_value())
    {
        return false;
    }

    const DownTool::AmapVectorColor currentColor =
        renderer_.GetGisOverlayColor(gisColorPaletteCategory_);
    if (currentColor.red == color->red && currentColor.green == color->green &&
        currentColor.blue == color->blue && currentColor.alpha == color->alpha)
    {
        return false;
    }

    renderer_.SetGisOverlayColor(gisColorPaletteCategory_, *color);
    selectGisColorCategory(gisColorPaletteCategory_);
    updateUi(runtime);
    requestRedraw();
    return true;
}

void DomDownToolPreview::PreviewController::updateUi(skui::Runtime& runtime)
{
    runtime.setVisibleById("preview-page", page_ == Page::Preview);
    runtime.setVisibleById("export-page", page_ == Page::Export);
    runtime.setVisibleById("task-page", page_ == Page::Tasks);
    if (page_ == Page::Export)
    {
        UpdateExportUi(runtime);
        return;
    }
    if (page_ == Page::Tasks)
    {
        UpdateTaskUi(runtime);
        return;
    }

    const MapSourceSummary source = renderer_.activeSource();
    const DownTool::LonLat center = renderer_.center();
    const MapRenderStats stats = renderer_.stats();
    const double memoryMegabytes =
        static_cast<double>(stats.cache.cpuBytes + stats.cache.gpuBytes) / (1024. * 1024.);

    runtime.setValueById("provider-select", source.id);
    runtime.setValueById("export-radius", previewRadiusValue_);
    runtime.setTextsById(
        {{"active-provider", source.name},
         {"coordinate-system", source.coordinateSystem},
         {"zoom-value", fmt::format("Z {:.2f}", renderer_.zoom())},
         {"view-span-value", fmt::format("视野 {:.1f}x", renderer_.GetViewSpan())},
         {"center-coordinate", fmt::format("{:.6f}, {:.6f}", center.lon, center.lat)},
         {"cache-path", pathToUtf8(source.cacheRoot)},
         {"tile-stats",
         fmt::format("可见 {}  已载入 {}  读取 {}  下载 {}/{}  失败 {}  缓存 {:.1f} MB",
                      stats.visibleTiles, stats.readyTiles, stats.cache.pendingTiles,
                      stats.downloads.activeTiles, stats.downloads.queuedTiles,
                      stats.downloads.failedTiles, memoryMegabytes)}});
    const GisOverlayCategories gisCategories = renderer_.GetGisOverlayCategories();
    if (!lastUiGisCategories_.has_value() || *lastUiGisCategories_ != gisCategories)
    {
        writeSearchTrace("Controller", "gis_ui_sync",
                         fmt::format("categories={}", gisCategoriesValue(gisCategories)));
        syncGisCheckboxes(runtime, gisCategories);
        lastUiGisCategories_ = gisCategories;
    }
    runtime.setVisibleById("gis-overlay-panel", gisOverlayPopupOpen_);
    runtime.setTextById("gis-overlay-arrow", gisOverlayPopupOpen_ ? "^" : "v");
    runtime.setAttributeById("gis-overlay-toggle", "aria-expanded",
                             gisOverlayPopupOpen_ ? "true" : "false");

    std::string_view paletteTargetLabel = kGisCategoryControls.front().label;
    for (size_t index = 0; index < kGisCategoryControls.size(); ++index)
    {
        const GisCategoryControl& control = kGisCategoryControls[index];
        const std::string categoryValue(control.value);
        const std::string colorValue =
            gisColorHex(renderer_.GetGisOverlayColor(control.category));
        if (lastUiGisColorValues_[index] != colorValue)
        {
            runtime.setValueById("gis-color-" + categoryValue, colorValue);
            runtime.setStyleById("gis-color-target-" + categoryValue,
                                 "background-color: " + colorValue + ";");
            lastUiGisColorValues_[index] = colorValue;
        }
        runtime.setAttributeById("gis-color-target-" + categoryValue, "aria-pressed",
                                 control.category == gisColorPaletteCategory_ ? "true"
                                                                             : "false");
        if (control.category == gisColorPaletteCategory_)
        {
            paletteTargetLabel = control.label;
        }
    }
    runtime.setTextById("gis-color-palette-target", paletteTargetLabel);
    const bool eyedropperActive = gisColorEyedropperActive_;
    runtime.setAttributeById("gis-color-eyedropper", "aria-pressed",
                             eyedropperActive ? "true" : "false");
    runtime.setAttributeById("gis-color-eyedropper", "aria-label",
                             eyedropperActive ? "取消屏幕取色" : "从屏幕吸取颜色");
    runtime.setAttributeById("gis-color-eyedropper", "title",
                             eyedropperActive ? "取消屏幕取色" : "从屏幕吸取颜色");
    const DownTool::AmapVectorColor pickerColor =
        fromHsv(HsvColor{gisColorHue_, gisColorSaturation_, gisColorBrightness_});
    const std::string pickerHex = gisColorHex(pickerColor);
    const std::string hueHex =
        gisColorHex(fromHsv(HsvColor{gisColorHue_, 1., 1.}));
    const std::string pickerState = fmt::format(
        "{}:{:.6f}:{:.6f}:{:.6f}", static_cast<int>(gisColorPaletteCategory_),
        gisColorHue_, gisColorSaturation_, gisColorBrightness_);
    if (lastUiGisPickerState_ != pickerState)
    {
        runtime.setStyleById(
            "gis-color-picker-field",
            "background-color: " + hueHex + "; background-image: "
                "linear-gradient(to bottom, transparent 0%, #000000 100%), "
                "linear-gradient(to right, #ffffff 0%, transparent 100%);");
        runtime.setStyleById("gis-color-current",
                             "background-color: " + pickerHex + ";");
        runtime.setStyleById(
            "gis-color-picker-marker",
            fmt::format("left: {:.1f}px; top: {:.1f}px;",
                        gisColorSaturation_ * kGisColorFieldWidth - 5.,
                        (1. - gisColorBrightness_) * kGisColorFieldHeight - 5.));
        runtime.setStyleById(
            "gis-color-hue-marker",
            fmt::format("top: {:.1f}px;", gisColorHue_ / 360. * kGisColorHueHeight - 2.));
        runtime.setTextById("gis-color-hue-value",
                            fmt::format("{:.0f}°", gisColorHue_));
        runtime.setTextById("gis-color-saturation-value",
                            fmt::format("{:.0f}%", gisColorSaturation_ * 100.));
        runtime.setTextById("gis-color-brightness-value",
                            fmt::format("{:.0f}%", gisColorBrightness_ * 100.));
        lastUiGisPickerState_ = pickerState;
    }
    runtime.setVisibleById("gis-color-panel", gisColorPopupOpen_);
}

void DomDownToolPreview::PreviewController::schedulePageChange(Page page,
                                                                skui::Runtime& runtime)
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (window && PostMessageW(window, kPreviewPageChangeMessage,
                               static_cast<WPARAM>(page), 0))
    {
        return;
    }

    switch (page)
    {
    case Page::Preview:
        OpenPreviewPage(runtime);
        break;
    case Page::Export:
        OpenExportPage(runtime);
        break;
    case Page::Tasks:
        OpenTaskPage(runtime);
        break;
    }
}

void DomDownToolPreview::PreviewController::OpenExportPage(skui::Runtime& runtime)
{
    InitializeExportDraft(runtime);
    page_ = Page::Export;
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::OpenTaskPage(skui::Runtime& runtime)
{
    page_ = Page::Tasks;
    UpdateTaskUi(runtime);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::OpenPreviewPage(skui::Runtime& runtime)
{
    page_ = Page::Preview;
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::InitializeExportDraft(skui::Runtime& runtime)
{
    const MapSourceSummary source = renderer_.activeSource();
    const DownTool::LonLat center = renderer_.center();
    exportLongitudeValue_ = fmt::format("{:.6f}", center.lon);
    exportLatitudeValue_ = fmt::format("{:.6f}", center.lat);
    exportRadiusValue_ = fmt::format("{:.0f}", renderer_.GetExportRadiusMeters());
    exportZoomValue_ = fmt::format("{:.0f}", std::round(renderer_.zoom()));
    exportMapSourceId_ = source.id;
    exportSatelliteOutputValue_ = source.cacheRoot.empty() ?
        std::string{} : pathToUtf8(
            source.cacheRoot / std::filesystem::u8path("Google_墨卡托"));
    exportValidationMessage_.clear();
    exportSatelliteTiles_ = false;
    exportSatelliteMergedImage_ = source.id != "none";
    exportGisTiles_ = false;
    exportFbx_ = false;
    exportFbxSource_ = ExportFbxSource::Satellite;
    exportBuildingHeightColors_ = false;

    exportGisGroups_.clear();
    const GisOverlayCategories categories = renderer_.GetGisOverlayCategories();
    exportGisMergedImage_ = !categories.empty();
    for (const DownTool::AmapVectorRenderCategory category : kExportCategories)
    {
        const GisOverlayCategory previewCategoryValue = previewCategory(category);
        exportGisGroups_.push_back(
            ExportGisUiGroup{
                nextExportGisGroupId_++, {category},
                categories.contains(previewCategoryValue)});
    }

    runtime.setValueById("export-lon", exportLongitudeValue_);
    runtime.setValueById("export-lat", exportLatitudeValue_);
    runtime.setValueById("export-config-radius", exportRadiusValue_);
    runtime.setValueById("export-zoom", exportZoomValue_);
    runtime.setValueById("export-provider-select", exportMapSourceId_);
    runtime.setValueById("export-satellite-output", exportSatelliteOutputValue_);
    runtime.setValueById("export-gis-output", exportGisOutputValue_);
    runtime.setValueById("export-merged-format", exportMergedFormat_);
    runtime.setValueById("export-merged-resolution", exportMergedResolutionValue_);
    runtime.setValueById("export-fbx-size", exportFbxSizeValue_);
    runtime.setValueById("export-fbx-source", "satellite");
    setCheckboxChecked(runtime, "export-satellite-tiles", exportSatelliteTiles_);
    setCheckboxChecked(runtime, "export-satellite-merged", exportSatelliteMergedImage_);
    setCheckboxChecked(runtime, "export-gis-tiles", exportGisTiles_);
    setCheckboxChecked(runtime, "export-gis-merged", exportGisMergedImage_);
    setCheckboxChecked(runtime, "export-fbx", exportFbx_);
    setCheckboxChecked(runtime, "export-building-height-colors", exportBuildingHeightColors_);
    UpdateExportUi(runtime);
}

void DomDownToolPreview::PreviewController::UpdateExportUi(skui::Runtime& runtime)
{
    const std::optional<double> radius = parseFiniteDouble(exportRadiusValue_);
    runtime.setTextById(
        "export-config-summary",
        fmt::format("中心 {:.5f}, {:.5f} · z{} · 半径 {} 米",
                    parseFiniteDouble(exportLongitudeValue_).value_or(0.),
                    parseFiniteDouble(exportLatitudeValue_).value_or(0.),
                    exportZoomValue_, radius.value_or(0.)));
    runtime.setTextById("export-validation-message", exportValidationMessage_);

    std::string groupsHtml;
    for (const ExportGisUiGroup& group : exportGisGroups_)
    {
        std::string labels;
        for (const DownTool::AmapVectorRenderCategory category : group.categories)
        {
            if (!labels.empty())
            {
                labels += "、";
            }
            labels += exportCategoryLabel(category);
        }
        const bool merged = group.categories.size() > 1;
        groupsHtml += fmt::format(
            "<button id=\"export-gis-group-{}\" class=\"gis-group-item{}\" "
            "type=\"button\" data-action=\"toggle-export-gis-group\" "
            "aria-pressed=\"{}\" "
            "title=\"{}\">{}{}<span class=\"gis-group-count\">{}</span></button>",
            group.id, merged ? " merged" : "", group.selected ? "true" : "false",
            merged ? "右键拆分此组合" : "选择后右键可合并", escapeHtml(labels),
            merged ? " · 合并" : "", group.categories.size());
    }
    if (groupsHtml.empty())
    {
        groupsHtml = "<div class=\"task-empty\">预览页没有选中的 GIS 分类</div>";
    }
    if (groupsHtml != lastExportGroupsHtml_)
    {
        runtime.replaceHtmlById("export-gis-groups", groupsHtml);
        lastExportGroupsHtml_ = groupsHtml;
    }
}

void DomDownToolPreview::PreviewController::UpdateTaskUi(skui::Runtime& runtime)
{
    const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
    size_t queuedCount = 0;
    size_t runningCount = 0;
    size_t succeededCount = 0;
    size_t failedCount = 0;
    std::string listHtml;
    for (const ExportTaskRecord& record : records)
    {
        switch (record.state)
        {
        case ExportTaskState::Queued:
            ++queuedCount;
            break;
        case ExportTaskState::Running:
            ++runningCount;
            break;
        case ExportTaskState::Succeeded:
            ++succeededCount;
            break;
        case ExportTaskState::Failed:
        case ExportTaskState::Canceled:
            ++failedCount;
            break;
        }
        const std::string stateValue =
            record.state == ExportTaskState::Queued ? "queued" :
            record.state == ExportTaskState::Running ? "running" :
            record.state == ExportTaskState::Succeeded ? "succeeded" :
            record.state == ExportTaskState::Canceled ? "canceled" : "failed";
        const std::string progressText = record.total == 0 ?
            fmt::format("{:.0f}%", record.progress * 100.) :
            fmt::format("{:.0f}% · {}/{}", record.progress * 100.,
                        record.completed, record.total);
        const std::string action = record.state == ExportTaskState::Queued ||
                record.state == ExportTaskState::Running ?
            fmt::format("<button id=\"task-cancel-{}\" class=\"danger-command\" "
                        "type=\"button\" data-action=\"cancel-export-task\">"
                        "取消</button>", escapeHtml(record.id)) : "";
        listHtml += fmt::format(
            "<article class=\"task-row\"><div><div class=\"task-title\">{}</div>"
            "<div class=\"task-detail\">{} · {}</div><div class=\"task-output\">{}</div>"
            "<div class=\"task-error\">{}</div></div><div><div class=\"task-stage\">"
            "<span>{}</span><span>{}</span></div><div class=\"progress-track\"><div "
            "class=\"progress-value\" style=\"width: {:.2f}%;\"></div></div></div>"
            "<div><span class=\"task-state {}\">{}</span><div class=\"task-time\">{}</div></div>"
            "<div class=\"task-row-actions\">{}</div></article>",
            escapeHtml(record.title), escapeHtml(record.detail), escapeHtml(record.createdAt),
            escapeHtml(record.outputPath), escapeHtml(record.error), escapeHtml(record.stage),
            progressText, record.progress * 100., stateValue,
            ExportTaskStateName(record.state), escapeHtml(record.finishedAt), action);
    }
    if (listHtml.empty())
    {
        listHtml = "<div class=\"task-empty\">暂无导出任务</div>";
    }
    if (listHtml != lastTaskListHtml_)
    {
        runtime.replaceHtmlById("export-task-list", listHtml);
        lastTaskListHtml_ = listHtml;
    }
    runtime.setTextsById({
        {"task-count-queued", std::to_string(queuedCount)},
        {"task-count-running", std::to_string(runningCount)},
        {"task-count-succeeded", std::to_string(succeededCount)},
        {"task-count-failed", std::to_string(failedCount)}});
}

void DomDownToolPreview::PreviewController::ToggleExportGisGroup(uint64_t groupId)
{
    const auto group = std::find_if(
        exportGisGroups_.begin(), exportGisGroups_.end(),
        [groupId](const ExportGisUiGroup& item) { return item.id == groupId; });
    if (group == exportGisGroups_.end())
    {
        return;
    }
    group->selected = !group->selected;
}

void DomDownToolPreview::PreviewController::HandleExportGisGroupContextAction(uint64_t groupId)
{
    const auto group = std::find_if(
        exportGisGroups_.begin(), exportGisGroups_.end(),
        [groupId](const ExportGisUiGroup& item) { return item.id == groupId; });
    if (group == exportGisGroups_.end())
    {
        return;
    }
    if (group->categories.size() > 1)
    {
        std::vector<ExportGisUiGroup> splitGroups;
        for (const DownTool::AmapVectorRenderCategory category : group->categories)
        {
            splitGroups.push_back(
                ExportGisUiGroup{nextExportGisGroupId_++, {category}, group->selected});
        }
        const size_t offset = static_cast<size_t>(std::distance(exportGisGroups_.begin(), group));
        exportGisGroups_.erase(group);
        exportGisGroups_.insert(exportGisGroups_.begin() + static_cast<ptrdiff_t>(offset),
                                splitGroups.begin(), splitGroups.end());
        return;
    }

    std::vector<size_t> selectedIndexes;
    for (size_t index = 0; index < exportGisGroups_.size(); ++index)
    {
        if (exportGisGroups_[index].selected || index ==
            static_cast<size_t>(std::distance(exportGisGroups_.begin(), group)))
        {
            selectedIndexes.push_back(index);
        }
    }
    if (selectedIndexes.size() < 2)
    {
        group->selected = true;
        return;
    }

    ExportGisUiGroup merged;
    merged.id = nextExportGisGroupId_++;
    merged.selected = true;
    for (const size_t index : selectedIndexes)
    {
        merged.categories.insert(exportGisGroups_[index].categories.begin(),
                                 exportGisGroups_[index].categories.end());
    }
    for (auto iterator = selectedIndexes.rbegin(); iterator != selectedIndexes.rend(); ++iterator)
    {
        exportGisGroups_.erase(exportGisGroups_.begin() + static_cast<ptrdiff_t>(*iterator));
    }
    exportGisGroups_.push_back(std::move(merged));
}

std::optional<DomDownToolPreview::ExportConfiguration>
DomDownToolPreview::PreviewController::BuildExportConfiguration(std::string& error) const
{
    const std::optional<double> lon = parseFiniteDouble(exportLongitudeValue_);
    const std::optional<double> lat = parseFiniteDouble(exportLatitudeValue_);
    const std::optional<double> radius = parseFiniteDouble(exportRadiusValue_);
    const std::optional<uint32_t> zoom = parseUint32(exportZoomValue_);
    const std::optional<uint32_t> mergedResolution = parseUint32(exportMergedResolutionValue_);
    const std::optional<uint32_t> fbxSize = parseUint32(exportFbxSizeValue_);
    if (!lon || *lon < -180. || *lon > 180. || !lat || *lat < -85. || *lat > 85.)
    {
        error = "中心经纬度无效。";
        return std::nullopt;
    }
    if (!radius || *radius <= 0.)
    {
        error = "导出半径必须大于 0 米。";
        return std::nullopt;
    }
    if (!zoom || *zoom > 30 || !mergedResolution || *mergedResolution == 0 || !fbxSize)
    {
        error = "层级、合并大图尺寸或 FBX 贴图尺寸无效。";
        return std::nullopt;
    }

    ExportConfiguration configuration;
    const std::vector<MapSourceSummary> sources = renderer_.sources();
    const auto source = std::find_if(
        sources.begin(), sources.end(),
        [this](const MapSourceSummary& item) { return item.id == exportMapSourceId_; });
    if (source == sources.end())
    {
        error = "卫星图来源无效。";
        return std::nullopt;
    }
    configuration.centerWgs84 = {*lon, *lat};
    configuration.radiusMeters = *radius;
    configuration.zoom = *zoom;
    configuration.mapSourceId = exportMapSourceId_;
    configuration.mapSourceName = source->name;
    configuration.satelliteCachePath = source->cacheRoot;
    configuration.satelliteOutputPath = std::filesystem::u8path(exportSatelliteOutputValue_);
    configuration.gisOutputPath = std::filesystem::u8path(exportGisOutputValue_);
    configuration.exportSatelliteTiles = exportSatelliteTiles_;
    configuration.exportSatelliteMergedImage = exportSatelliteMergedImage_;
    configuration.exportGisTiles = exportGisTiles_;
    configuration.exportGisMergedImage = exportGisMergedImage_;
    configuration.exportFbx = exportFbx_;
    configuration.fbxSource = exportFbxSource_;
    configuration.colorBuildingsByHeight = exportBuildingHeightColors_;
    configuration.mergedImageResolution = *mergedResolution;
    configuration.mergedImageFormat = exportMergedFormat_ == "jpg" ? "jpg" : "png";
    configuration.fbxTextureSize = *fbxSize;
    for (const ExportGisUiGroup& group : exportGisGroups_)
    {
        if (!group.selected)
        {
            continue;
        }
        configuration.gisGroups.push_back(
            GisExportGroup{ {}, group.categories });
    }
    for (const DownTool::AmapVectorRenderCategory category : kExportCategories)
    {
        const DownTool::AmapVectorColor color = renderer_.GetGisOverlayColor(
            previewCategory(category));
        configuration.gisColors[category] = color;
    }
    if (!configuration.exportSatelliteTiles && !configuration.exportSatelliteMergedImage &&
        !configuration.exportGisTiles && !configuration.exportGisMergedImage &&
        !configuration.exportFbx)
    {
        error = "至少选择一种输出内容。";
        return std::nullopt;
    }
    const bool satelliteRequested = configuration.exportSatelliteTiles ||
        configuration.exportSatelliteMergedImage ||
        (configuration.exportFbx && configuration.fbxSource == ExportFbxSource::Satellite);
    if (satelliteRequested && source->id == "none")
    {
        error = "卫星图导出需要选择地图来源。";
        return std::nullopt;
    }
    if (satelliteRequested && configuration.satelliteOutputPath.empty())
    {
        error = "卫星输出目录不能为空。";
        return std::nullopt;
    }
    const bool gisRequested = configuration.exportGisTiles ||
        configuration.exportGisMergedImage ||
        (configuration.exportFbx && configuration.fbxSource == ExportFbxSource::Gis);
    if (gisRequested && configuration.gisGroups.empty())
    {
        error = "GIS 导出至少需要选择一个 GIS 输出组。";
        return std::nullopt;
    }
    if (gisRequested && configuration.gisOutputPath.empty())
    {
        error = "GIS 输出目录不能为空。";
        return std::nullopt;
    }
    return configuration;
}

void DomDownToolPreview::PreviewController::SubmitExport(skui::Runtime& runtime)
{
    std::string error;
    const std::optional<ExportConfiguration> configuration =
        BuildExportConfiguration(error);
    if (!configuration.has_value())
    {
        exportValidationMessage_ = error;
        UpdateExportUi(runtime);
        requestRedraw();
        return;
    }
    exportTaskManager_.Enqueue(*configuration);
    exportValidationMessage_.clear();
    schedulePageChange(Page::Tasks, runtime);
}

bool DomDownToolPreview::PreviewController::updateCursor(skui::Runtime& runtime, double screenX,
                                                         double screenY)
{
    const std::optional<DownTool::LonLat> coordinate = renderer_.lonLatAt(screenX, screenY);
    if (!coordinate.has_value())
    {
        return false;
    }

    const std::string coordinateText =
        fmt::format("{:.6f}, {:.6f}", coordinate->lon, coordinate->lat);
    if (coordinateText == lastCursorCoordinate_)
    {
        return false;
    }

    runtime.setTextById("cursor-coordinate", coordinateText);
    lastCursorCoordinate_ = coordinateText;
    return true;
}

void DomDownToolPreview::PreviewController::scheduleImmediateMapRedraw(HWND window)
{
    if (immediateMapRedrawPosted_ ||
        !mapDragRedrawThrottle_.shouldRefresh(MapHoverRefreshThrottle::Clock::now()))
    {
        return;
    }

    immediateMapRedrawPosted_ = true;
    if (!PostMessageW(window, kPreviewImmediateMapRedrawMessage, 0, 0))
    {
        immediateMapRedrawPosted_ = false;
    }
}

void DomDownToolPreview::PreviewController::scheduleMapUiRefresh(HWND window, const POINT& point)
{
    pendingMapUiPoint_ = point;
    if (mapUiRefreshPosted_)
    {
        return;
    }

    mapUiRefreshPosted_ = true;
    if (!PostMessageW(window, kPreviewMapUiRefreshMessage, 0, 0))
    {
        mapUiRefreshPosted_ = false;
    }
}

void DomDownToolPreview::PreviewController::updateMapInteractionUi(
    skui::Runtime& runtime, const POINT& point)
{
    const bool cursorChanged = updateCursor(runtime, point.x, point.y);
    const std::string centerText =
        fmt::format("{:.6f}, {:.6f}", renderer_.center().lon, renderer_.center().lat);
    const std::string zoomText = fmt::format("Z {:.2f}", renderer_.zoom());
    bool statusChanged = false;
    if (centerText != lastCenterCoordinate_)
    {
        runtime.setTextById("center-coordinate", centerText);
        lastCenterCoordinate_ = centerText;
        statusChanged = true;
    }
    if (zoomText != lastZoomValue_)
    {
        runtime.setTextById("zoom-value", zoomText);
        lastZoomValue_ = zoomText;
        statusChanged = true;
    }

    if (cursorChanged || statusChanged)
    {
        requestRedraw();
    }
}

void DomDownToolPreview::PreviewController::scheduleUiRefresh()
{
    const MapRenderStats stats = renderer_.stats();
    if (scheduledStats_.has_value() && equalStats(*scheduledStats_, stats))
    {
        return;
    }

    const HWND window = window_.load(std::memory_order_relaxed);
    if (window && PostMessageW(window, kPreviewUiRefreshMessage, 0, 0))
    {
        scheduledStats_ = stats;
    }
}

void DomDownToolPreview::PreviewController::scheduleSearchRefresh() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window)
    {
        writeSearchTrace("Controller", "search_refresh_post_failed", "reason=no_window");
        return;
    }

    const BOOL posted = PostMessageW(window, kPreviewSearchRefreshMessage, 0, 0);
    writeSearchTrace("Controller", posted ? "search_refresh_posted"
                                           : "search_refresh_post_failed",
                     posted ? "" : fmt::format("win32_error={}", GetLastError()));
}

void DomDownToolPreview::PreviewController::scheduleExportTaskRefresh()
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window || exportTaskRefreshPosted_.exchange(true, std::memory_order_relaxed))
    {
        return;
    }
    if (!PostMessageW(window, kPreviewExportTaskRefreshMessage, 0, 0))
    {
        exportTaskRefreshPosted_.store(false, std::memory_order_relaxed);
    }
}

void DomDownToolPreview::PreviewController::requestRedraw() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (window)
    {
        PostMessageW(window, kPreviewRedrawMessage, 0, 0);
    }
}
