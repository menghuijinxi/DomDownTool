#include "PostProcessTestSupport.h"

#include <filesystem>
#include <cmath>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include <objbase.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"
#include "nlohmann/json.hpp"
#include "opencv2/imgcodecs.hpp"
#include "skui_runtime.h"
#include "skui_win32_event_adapter.h"

#include "preview/PreviewController.h"

namespace
{
class RangeController : public DomDownToolPreview::PreviewController
{
  public:
    void OpenExportEditor(skui::Runtime& runtime) { BeginExportRangeEditing(runtime); }
    void LoadExport(skui::Runtime& runtime,
                    const DomDownToolPreview::ExportConfiguration& configuration)
    {
        InitializeExportDraft(runtime, &configuration);
    }
    std::optional<DomDownToolPreview::ExportConfiguration> ReadExport(std::string& error) const
    {
        return BuildExportConfiguration(error);
    }
    const DownTool::DownloadRegions& GetRegions() const { return rangeEditor_.GetRegions(); }
    const DownTool::DownloadRegions& GetExportRegions() const { return exportDownloadRegions_; }
    const std::string& GetSelectedId() const { return rangeEditor_.GetSelectedId(); }
    const std::string& GetMessage() const { return rangeEditorMessage_; }
    std::optional<double> GetSquareRadius() const
    {
        return rangeEditor_.GetSelectedSquareRadiusMeters();
    }
    bool GetPanelOpen() const { return rangePanelOpen_; }
    void RestoreExportRegions(const DownTool::DownloadRegions& regions)
    {
        exportDownloadRegions_ = regions;
    }
};

void WindowMessage(RangeController& controller, skui::win32::Win32EventAdapter& adapter,
                   HWND window, skui::Runtime& runtime, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!adapter.handleMessage(window, message, wParam, lParam))
    {
        controller.handleWindowMessage(window, message, wParam, lParam, runtime);
    }
}

void MapMessage(RangeController& controller, skui::win32::Win32EventAdapter& adapter, HWND window,
                skui::Runtime& runtime, UINT message, POINT point)
{
    const WPARAM buttons = message == WM_LBUTTONDOWN || message == WM_MOUSEMOVE ? MK_LBUTTON : 0;
    WindowMessage(controller, adapter, window, runtime, message, buttons,
                  MAKELPARAM(point.x, point.y));
}

void MapClick(RangeController& controller, skui::win32::Win32EventAdapter& adapter, HWND window,
              skui::Runtime& runtime, POINT point)
{
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, point);
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, point);
}

void UiClick(RangeController& controller, skui::win32::Win32EventAdapter& adapter, HWND window,
             skui::Runtime& runtime, float x, float y)
{
    std::vector<uint32_t> pixels(static_cast<size_t>(runtime.width()) * runtime.height());
    runtime.renderToBgraPixels(pixels.data(), runtime.width(), runtime.height(),
                               runtime.width() * sizeof(uint32_t), runtime.dpiScale());
    const float scale = runtime.effectiveScale();
    const POINT point{static_cast<LONG>(x * scale), static_cast<LONG>(y * scale)};
    MapClick(controller, adapter, window, runtime, point);
}

bool WriteScreenshot(skui::Runtime& runtime, const DownTool::DownloadRegions& regions,
                     const std::filesystem::path& path, int width, int height)
{
    const auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface)
    {
        return false;
    }
    DomDownToolPreview::MapRenderer renderer([] {});
    renderer.selectSource("none");
    const auto polygons =
        DownTool::GetRegionPolygons(regions, DownTool::DownloadRegionRole::Include);
    const auto bounds = DownTool::CalculateGeographicBounds(polygons);
    auto center = DownTool::GeographicBoundsCenter(*bounds);
    const double span = std::max(
        bounds->east - bounds->west,
        (bounds->north - bounds->south) / std::cos(center.lat * std::numbers::pi / 180.));
    const double availablePixels = std::min(width - 340., height - 150.);
    const double zoom =
        std::clamp(std::log2(availablePixels * 360. / (256. * span)), 1., 18.);
    center.lon -= 150. * 360. / (256. * std::exp2(zoom));
    renderer.setView(center, zoom);
    renderer.SetExportPolygonsWgs84(polygons);
    renderer.SetDownloadRegions(regions, regions.back().GetId());
    renderer.draw(*surface->getCanvas(), width, height, 1.F);
    skui::RuntimeRenderRegion overlay;
    overlay.drawBackground = false;
    runtime.renderInto(*surface->getCanvas(), overlay);
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    if (!surface->readPixels(SkImageInfo::MakeN32Premul(width, height), pixels.data(),
                             width * sizeof(uint32_t), 0, 0))
    {
        return false;
    }
    return cv::imwrite(path.string(), cv::Mat(height, width, CV_8UC4, pixels.data()));
}

bool TestCopyTerrainExclusion(RangeController& controller, skui::Runtime& runtime,
                              skui::win32::Win32EventAdapter& adapter, HWND window,
                              const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    runtime.resize(1440, 900, 1.F);
    DomDownToolPreview::ExportConfiguration configuration;
    configuration.centerWgs84 = {116.41822112345678, 39.88901312345678};
    configuration.radiusMeters = 20000.;
    configuration.zoom = 18;
    const auto bounds = DomDownToolPreview::calculateExportGeoBounds(
        configuration.centerWgs84, configuration.radiusMeters);
    const DownTool::Wgs84Ring square = {
        {bounds->minimumLongitude, bounds->minimumLatitude},
        {bounds->maximumLongitude, bounds->minimumLatitude},
        {bounds->maximumLongitude, bounds->maximumLatitude},
        {bounds->minimumLongitude, bounds->maximumLatitude}};
    configuration.rangeMode = DomDownToolPreview::ExportRangeMode::CustomPolygon;
    configuration.polygonsWgs84 = {square};
    configuration.SetDownloadRegions(DownTool::MakeDownloadRegions({square}));
    DomDownToolPreview::ExportContentGroup group;
    group.id = "terrain-group";
    group.name = "地形";
    DomDownToolPreview::ExportComponent component;
    component.id = "terrain";
    component.kind = DomDownToolPreview::ExportComponentKind::BasePlate;
    component.sources.push_back({
        "google.satellite", "Google 卫星", DomDownToolPreview::ExportSourceKind::Imagery,
        testRoot / "imagery-cache"});
    group.components.push_back(component);
    configuration.groups.push_back(group);
    controller.LoadExport(runtime, configuration);
    std::string error;
    const auto restored = controller.ReadExport(error);
    if (!Require(restored &&
                     restored->centerWgs84.lon == configuration.centerWgs84.lon &&
                     restored->centerWgs84.lat == configuration.centerWgs84.lat,
                 "复制配置丢失了原始中心坐标精度：" + error))
    {
        return false;
    }
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 100.F, 319.F);
    const auto selected = controller.GetSelectedId();
    const auto original = DownTool::DownloadRegionsToJson(controller.GetRegions());
    UiClick(controller, adapter, window, runtime, 220.F, 734.F);
    if (!Require(controller.GetRegions().size() == 2 &&
                     controller.GetSelectedId() == selected &&
                     controller.GetRegions()[1].GetRole() ==
                         DownTool::DownloadRegionRole::ExcludeTerrain,
                 "复制按钮没有生成底板排除区或改变了原下载区选择"))
    {
        return false;
    }
    const auto copied = DownTool::DownloadRegionsToJson(controller.GetRegions());
    if (!Require(copied[0] == original[0] &&
                     copied[1]["verticesWgs84"] == original[0]["verticesWgs84"],
                 "界面复制没有保留原始精度的边界"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    if (!Require(controller.GetRegions().size() == 1, "复制操作不能一次撤销"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 184.F, 214.F);
    if (!Require(controller.GetRegions().size() == 2, "复制操作不能重做"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 200.F, 774.F);
    skui::Event selectAll;
    selectAll.type = skui::EventType::KeyDown;
    selectAll.key = 'A';
    selectAll.ctrlKey = true;
    runtime.handleEvent(selectAll);
    for (const char digit : std::string("40000"))
    {
        WindowMessage(controller, adapter, window, runtime, WM_CHAR, digit, 0);
    }
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    const auto expanded = DownTool::DownloadRegionsToJson(controller.GetRegions());
    const auto expandedBounds = DownTool::CalculateGeographicBounds(
        DownTool::GetRegionPolygons(controller.GetRegions(), DownTool::DownloadRegionRole::Include));
    if (!Require(controller.GetSquareRadius().has_value() &&
                     std::abs(*controller.GetSquareRadius() - 40000.) < 1.e-6 &&
                     expanded[1] == copied[1] && expandedBounds &&
                     std::abs(DownTool::GeographicBoundsCenter(*expandedBounds).lon -
                              configuration.centerWgs84.lon) < 1.e-12 &&
                     std::abs(DownTool::GeographicBoundsCenter(*expandedBounds).lat -
                              configuration.centerWgs84.lat) < 1.e-12,
                 "扩大到 40 公里后内圈边界或下载区中心发生变化"))
    {
        return false;
    }
    if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                 testRoot / "range-editor-terrain-exclusion.png", 1440, 900),
                 "底板排除界面截图失败"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 232.F, 828.F);
    const auto applied = controller.ReadExport(error);
    if (!Require(applied &&
                     applied->centerWgs84.lon == configuration.centerWgs84.lon &&
                     applied->centerWgs84.lat == configuration.centerWgs84.lat,
                 "复制底板排除区和应用范围改变了模型中心：" + error))
    {
        return false;
    }
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 100.F, 319.F);
    if (!Require(controller.GetRegions().size() == 2 &&
                     DownTool::DownloadRegionsToJson(controller.GetRegions()) == expanded,
                 "应用和重新打开丢失底板排除区"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                  testRoot / "range-editor-terrain-exclusion-narrow.png", 700, 600),
                 "底板排除窄窗口截图失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    configuration.rangeMode = DomDownToolPreview::ExportRangeMode::CenterRadius;
    configuration.polygonsWgs84.clear();
    configuration.SetDownloadRegions({});
    controller.LoadExport(runtime, configuration);
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 232.F, 828.F);
    const auto radiusApplied = controller.ReadExport(error);
    return Require(radiusApplied &&
                       radiusApplied->centerWgs84.lon == configuration.centerWgs84.lon &&
                       radiusApplied->centerWgs84.lat == configuration.centerWgs84.lat,
                   "中心半径模式应用范围时丢失中心坐标精度：" + error);
}

bool TestSquareRadiusInput(RangeController& controller, skui::Runtime& runtime,
                           skui::win32::Win32EventAdapter& adapter, HWND window,
                           const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    runtime.resize(1440, 900, 1.F);
    const auto bounds = DomDownToolPreview::calculateExportGeoBounds({116.4, 39.9}, 1000.);
    const DownTool::Wgs84Ring square = {{bounds->minimumLongitude, bounds->minimumLatitude},
                                        {bounds->maximumLongitude, bounds->minimumLatitude},
                                        {bounds->maximumLongitude, bounds->maximumLatitude},
                                        {bounds->minimumLongitude, bounds->maximumLatitude}};
    auto regions = DownTool::MakeDownloadRegions(
        {square, {{116.399, 39.899}, {116.401, 39.899}, {116.4, 39.901}}});
    regions.back().SetRole(DownTool::DownloadRegionRole::ExcludeGis);
    const auto before = DownTool::DownloadRegionsToJson(regions);
    controller.RestoreExportRegions(regions);
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 210.F, 134.F);
    UiClick(controller, adapter, window, runtime, 100.F, 319.F);
    if (!Require(controller.GetSquareRadius().has_value() &&
                     std::abs(*controller.GetSquareRadius() - 1000.) < 1.e-6,
                 "选中旧正方形区域后没有恢复半径参数"))
    {
        return false;
    }
    const auto replaceFirstRadiusDigit = [&]
    {
        const int width = runtime.width();
        const int height = runtime.height();
        const float scale = runtime.effectiveScale();
        const float inputY = static_cast<float>(height) / scale - 126.F;
        std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
        runtime.renderToBgraPixels(pixels.data(), width, height, width * sizeof(uint32_t),
                                   runtime.dpiScale());
        // 从实际字形取点击坐标，避免只验证选区偏移而漏掉文字与选区的绘制错位。
        std::optional<int> firstDigitX;
        for (int x = static_cast<int>(98.F * scale);
             x < static_cast<int>(266.F * scale) && !firstDigitX; ++x)
        {
            for (int y = static_cast<int>((inputY - 6.F) * scale);
                 y < static_cast<int>((inputY + 7.F) * scale); ++y)
            {
                const auto pixel = pixels[static_cast<size_t>(y) * width + x];
                if ((pixel & 0xFF) > 180 && ((pixel >> 8) & 0xFF) > 180 &&
                    ((pixel >> 16) & 0xFF) > 180)
                {
                    firstDigitX = x;
                    break;
                }
            }
        }
        if (!Require(firstDigitX.has_value(), "半径输入框没有绘制数值"))
        {
            return false;
        }
        const float textX = static_cast<float>(*firstDigitX) / scale;
        UiClick(controller, adapter, window, runtime, textX - 2.F, inputY);
        const auto cursor = runtime.selection();
        const auto caret = runtime.editingCaretRect();
        if (!Require(cursor.anchorNodeId == "range-region-radius" &&
                         cursor.focusNodeId == "range-region-radius" && cursor.anchorOffset == 0 &&
                         cursor.focusOffset == 0 && caret &&
                         std::abs(caret->x - (textX - 2.F)) < 5.F,
                     "点击半径首位数字前方后光标与文字错位"))
        {
            return false;
        }
        const POINT start{static_cast<LONG>((textX - 2.F) * scale),
                          static_cast<LONG>(inputY * scale)};
        const POINT end{static_cast<LONG>((textX + 6.F) * scale), start.y};
        MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, start);
        MapMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, end);
        MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, end);
        const auto selection = runtime.selection();
        if (!Require(selection.anchorNodeId == "range-region-radius" &&
                         selection.focusNodeId == "range-region-radius" &&
                         selection.anchorOffset == 0 && selection.focusOffset == 1,
                     "鼠标拖选半径首位数字后没有保留正确选区"))
        {
            return false;
        }
        if (scale > 1.F &&
            !Require(WriteScreenshot(runtime, controller.GetRegions(),
                                     testRoot / "range-editor-radius-selection-150.png", width,
                                     height),
                     "半径文本选区截图失败"))
        {
            return false;
        }
        WindowMessage(controller, adapter, window, runtime, WM_CHAR, '2', 0);
        WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
        if (!Require(controller.GetSquareRadius().has_value() &&
                         std::abs(*controller.GetSquareRadius() - 2000.) < 1.e-6 &&
                         DownTool::DownloadRegionsToJson(controller.GetRegions())[1] == before[1],
                     "拖选后输入没有只替换首位数字，或意外修改了排除区"))
        {
            return false;
        }
        UiClick(controller, adapter, window, runtime, 148.F, 214.F);
        return Require(controller.GetSquareRadius().has_value() &&
                           std::abs(*controller.GetSquareRadius() - 1000.) < 1.e-6,
                       "鼠标局部替换半径后无法一次撤销");
    };
    if (!replaceFirstRadiusDigit())
    {
        return false;
    }
    runtime.resize(2160, 1350, 1.5F);
    if (!replaceFirstRadiusDigit())
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    if (!replaceFirstRadiusDigit())
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    const auto typeRadius = [&](const std::string& value)
    {
        UiClick(controller, adapter, window, runtime, 200.F, 774.F);
        skui::Event selectAll;
        selectAll.type = skui::EventType::KeyDown;
        selectAll.key = 'A';
        selectAll.ctrlKey = true;
        runtime.handleEvent(selectAll);
        for (const char digit : value)
        {
            WindowMessage(controller, adapter, window, runtime, WM_CHAR, digit, 0);
        }
    };
    typeRadius("2000");
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    if (!Require(controller.GetSquareRadius().has_value() &&
                     std::abs(*controller.GetSquareRadius() - 2000.) < 1.e-6 &&
                     DownTool::DownloadRegionsToJson(controller.GetRegions())[1] == before[1],
                 "半径输入和回车没有修改下载区，或改动了排除区：" + controller.GetMessage()))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    if (!Require(std::abs(*controller.GetSquareRadius() - 1000.) < 1.e-6,
                 "半径输入无法通过按钮一次撤销"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 184.F, 214.F);
    if (!Require(std::abs(*controller.GetSquareRadius() - 2000.) < 1.e-6,
                 "半径输入无法通过按钮重做"))
    {
        return false;
    }
    typeRadius("2500");
    UiClick(controller, adapter, window, runtime, 40.F, 214.F);
    if (!Require(controller.GetSquareRadius().has_value() &&
                     std::abs(*controller.GetSquareRadius() - 2500.) < 1.e-6,
                 "半径输入框失去焦点后没有应用修改"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    const auto validRegions = DownTool::DownloadRegionsToJson(controller.GetRegions());
    typeRadius("0");
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    if (!Require(DownTool::DownloadRegionsToJson(controller.GetRegions()) == validRegions &&
                     !controller.GetMessage().empty(),
                 "无效半径没有保留原区域并提示错误"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 40.F, 214.F);
    if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                 testRoot / "range-editor-square-radius.png", 1440, 900),
                 "半径参数截图失败"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 232.F, 828.F);
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 100.F, 319.F);
    if (!Require(controller.GetSquareRadius().has_value() &&
                     std::abs(*controller.GetSquareRadius() - 2000.) < 1.e-6,
                 "应用并重新打开后没有保留修改后的半径"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    return Require(WriteScreenshot(runtime, controller.GetRegions(),
                                   testRoot / "range-editor-square-radius-narrow.png", 700, 600),
                   "半径参数窄窗口截图失败");
}

bool RunTest(HWND window, const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    skui::Runtime runtime;
    const auto document =
        std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "resources/preview/index.html";
    if (!Require(runtime.loadDocument(document.string()), "预览文档加载失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    std::vector<uint32_t> layoutPixels(1440 * 900);
    runtime.renderToBgraPixels(layoutPixels.data(), 1440, 900, 1440 * sizeof(uint32_t), 1.F);
    UiClick(controller, adapter, window, runtime, 1405.F, 80.F);
    if (!Require(controller.GetPanelOpen(), "地图右上角的范围编辑入口无法点击"))
    {
        return false;
    }
    runtime.renderToBgraPixels(layoutPixels.data(), 1440, 900, 1440 * sizeof(uint32_t), 1.F);
    UiClick(controller, adapter, window, runtime, 255.F, 93.F);
    if (!Require(!controller.GetPanelOpen(), "点击范围编辑面板的关闭按钮没有响应"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 1405.F, 80.F);
    runtime.renderToBgraPixels(layoutPixels.data(), 1440, 900, 1440 * sizeof(uint32_t), 1.F);
    UiClick(controller, adapter, window, runtime, 210.F, 134.F);
    UiClick(controller, adapter, window, runtime, 76.F, 214.F);
    MapClick(controller, adapter, window, runtime, {420, 180});
    MapClick(controller, adapter, window, runtime, {850, 180});
    MapClick(controller, adapter, window, runtime, {850, 620});
    MapClick(controller, adapter, window, runtime, {420, 620});
    UiClick(controller, adapter, window, runtime, 220.F, 214.F);
    if (!Require(controller.GetRegions().size() == 1, "地图绘制没有创建下载区"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 174.F);
    UiClick(controller, adapter, window, runtime, 112.F, 214.F);
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, {500, 300});
    MapMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, {650, 450});
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, {650, 450});
    if (!Require(controller.GetRegions().size() == 2 &&
                     controller.GetRegions()[1].GetRole() ==
                         DownTool::DownloadRegionRole::ExcludeGis,
                 "矩形 GIS 排除区绘制失败：" + controller.GetMessage()))
    {
        return false;
    }
    const auto beforeDrag = DownTool::DownloadRegionsToJson(controller.GetRegions());
    UiClick(controller, adapter, window, runtime, 40.F, 214.F);
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, {500, 300});
    MapMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, {490, 290});
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, {490, 290});
    if (!Require(DownTool::DownloadRegionsToJson(controller.GetRegions()) != beforeDrag,
                 "排除区的顶点没有随拖动更新"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    if (!Require(DownTool::DownloadRegionsToJson(controller.GetRegions()) == beforeDrag,
                 "撤销顶点拖动没有恢复坐标"))
    {
        return false;
    }
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, {500, 300});
    MapMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, {490, 290});
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_ESCAPE, 0);
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, {490, 290});
    if (!Require(DownTool::DownloadRegionsToJson(controller.GetRegions()) == beforeDrag,
                 "Esc 取消拖动后仍提交了顶点修改"))
    {
        return false;
    }
    MapClick(controller, adapter, window, runtime, {575, 300});
    if (!Require(controller.GetRegions()[1].GetVertices().size() == 5,
                 "点击边界没有插入可编辑顶点"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    UiClick(controller, adapter, window, runtime, 112.F, 214.F);
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, {400, 700});
    MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, {200, 720});
    if (!Require(controller.GetRegions().size() == 3, "矩形拖入面板后松手没有正确提交"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    UiClick(controller, adapter, window, runtime, 76.F, 214.F);
    MapClick(controller, adapter, window, runtime, {900, 250});
    MapClick(controller, adapter, window, runtime, {1050, 250});
    MapClick(controller, adapter, window, runtime, {1050, 450});
    MapClick(controller, adapter, window, runtime, {900, 450});
    UiClick(controller, adapter, window, runtime, 80.F, 174.F);
    // 切换区域类型会取消草稿，避免把未完成的排除区误存为下载区。
    if (!Require(controller.GetRegions().size() == 2, "切换区域类型意外提交了草稿"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 76.F, 214.F);
    MapClick(controller, adapter, window, runtime, {900, 250});
    MapClick(controller, adapter, window, runtime, {1050, 250});
    MapClick(controller, adapter, window, runtime, {1050, 450});
    UiClick(controller, adapter, window, runtime, 220.F, 214.F);
    if (!Require(controller.GetRegions().size() == 3, "连续绘制覆盖了已有区域"))
    {
        return false;
    }
    if (!Require(runtime.textContentById("preview-kml-name").value_or("") == "下载区 2 · 排除区 1",
                 "工具栏区域统计没有随绘制刷新"))
    {
        return false;
    }
    if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                 testRoot / "range-editor-desktop.png", 1440, 900),
                 "范围编辑桌面截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                 testRoot / "range-editor-narrow.png", 700, 600),
                 "范围编辑窄窗口截图失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    runtime.renderToBgraPixels(layoutPixels.data(), 1440, 900, 1440 * sizeof(uint32_t), 1.F);
    UiClick(controller, adapter, window, runtime, 35.F, 319.F);
    if (!Require(!controller.GetRegions()[0].GetEnabled(), "区域启用复选框没有更新模型"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    UiClick(controller, adapter, window, runtime, 184.F, 214.F);
    if (!Require(!controller.GetRegions()[0].GetEnabled(), "点击重做没有恢复区域停用状态"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    UiClick(controller, adapter, window, runtime, 100.F, 355.F);
    UiClick(controller, adapter, window, runtime, 255.F, 319.F);
    if (!Require(controller.GetSelectedId() == controller.GetRegions()[0].GetId(),
                 "点击定位按钮没有选中对应区域"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 100.F, 774.F);
    WindowMessage(controller, adapter, window, runtime, WM_CHAR, L'X', 0);
    if (!Require(controller.GetRegions()[0].GetName().find('X') != std::string::npos,
                 "区域名称输入没有更新模型"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 148.F, 214.F);
    const auto originalPreview = DownTool::DownloadRegionsToJson(controller.GetRegions());
    controller.RestoreExportRegions(controller.GetRegions());
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 210.F, 134.F);
    UiClick(controller, adapter, window, runtime, 40.F, 214.F);
    UiClick(controller, adapter, window, runtime, 100.F, 425.F);
    if (!Require(controller.GetSelectedId() == controller.GetRegions()[1].GetId(),
                 "点击区域列表没有选中 GIS 排除区"))
    {
        return false;
    }
    if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                 testRoot / "range-editor-export.png", 1440, 900),
                 "导出范围编辑截图失败"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 255.F, 774.F);
    if (!Require(controller.GetRegions().size() == 2, "点击删除按钮没有删除所选区域"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 232.F, 828.F);
    if (!Require(controller.GetExportRegions().size() == 2 &&
                     DownTool::DownloadRegionsToJson(controller.GetRegions()) == originalPreview,
                 "返回导出后没有应用修改，或覆盖了原地图范围"))
    {
        return false;
    }
    controller.OpenExportEditor(runtime);
    if (!Require(controller.GetRegions().size() == 2, "重新进入编辑时没有恢复上次保存的区域"))
    {
        return false;
    }
    UiClick(controller, adapter, window, runtime, 100.F, 319.F);
    UiClick(controller, adapter, window, runtime, 255.F, 774.F);
    UiClick(controller, adapter, window, runtime, 148.F, 828.F);
    if (!Require(controller.GetExportRegions().size() == 2 && !controller.GetPanelOpen(),
                 "取消编辑没有返回导出页，或修改了导出配置"))
    {
        return false;
    }
    runtime.resize(2160, 1350, 1.5F);
    controller.OpenExportEditor(runtime);
    UiClick(controller, adapter, window, runtime, 255.F, 93.F);
    if (!Require(!controller.GetPanelOpen(), "150% 缩放下关闭范围编辑按钮没有响应"))
    {
        return false;
    }
    return TestSquareRadiusInput(controller, runtime, adapter, window, testRoot) &&
        TestCopyTerrainExclusion(controller, runtime, adapter, window, testRoot);
}
} // namespace

int main()
{
    const auto testRoot =
        std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "tmp/preview-range-editor-test";
    std::filesystem::create_directories(testRoot);
    std::filesystem::current_path(testRoot);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RECT windowBounds{0, 0, 1440, 900};
    AdjustWindowRect(&windowBounds, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowExW(0, L"STATIC", L"Range editor test", WS_OVERLAPPEDWINDOW, 0, 0,
                                  windowBounds.right - windowBounds.left,
                                  windowBounds.bottom - windowBounds.top, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (!window)
    {
        return 1;
    }
    const bool passed = RunTest(window, testRoot);
    DestroyWindow(window);
    CoUninitialize();
    if (!passed)
    {
        return 1;
    }
    std::cout << "PreviewRangeEditorTest passed\n";
    return 0;
}
