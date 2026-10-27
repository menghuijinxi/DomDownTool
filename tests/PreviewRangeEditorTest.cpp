#include "PostProcessTestSupport.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <chrono>
#include <iostream>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkSurface.h"
#include "nlohmann/json.hpp"
#include "opencv2/imgcodecs.hpp"
#include "skui_runtime.h"
#include "skui_win32_event_adapter.h"

#include "preview/PreviewController.h"
#include "ShapefileTestSupport.h"

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

bool ClickElement(RangeController& controller, skui::Runtime& runtime,
                  skui::win32::Win32EventAdapter& adapter, HWND window, const std::string& id);

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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions().front().GetId()) ||
        !ClickElement(controller, runtime, adapter, window, "range-role-terrain"))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions().front().GetId()))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions().front().GetId()))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions().front().GetId()))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window, "range-role-exclude"))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-enable-" + controller.GetRegions().front().GetId()))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions()[2].GetId()) ||
        !ClickElement(controller, runtime, adapter, window,
                      "range-focus-" + controller.GetRegions().front().GetId()))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions()[1].GetId()))
    {
        return false;
    }
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
    if (!ClickElement(controller, runtime, adapter, window,
                      "range-select-" + controller.GetRegions().front().GetId()))
    {
        return false;
    }
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

bool WriteTaskScreenshot(skui::Runtime& runtime, const std::filesystem::path& path)
{
    std::vector<uint32_t> pixels(static_cast<size_t>(runtime.width()) * runtime.height());
    return runtime.renderToBgraPixels(pixels.data(), runtime.width(), runtime.height(),
                                      runtime.width() * sizeof(uint32_t), runtime.dpiScale()) &&
           cv::imwrite(path.string(),
                       cv::Mat(runtime.height(), runtime.width(), CV_8UC4, pixels.data()));
}

std::optional<POINT> FindElementCenter(skui::Runtime& runtime, const std::string& id)
{
    // 以渲染后的按钮色块定位，保留生产布局并同时验证按钮在当前窗口中可见。
    if (!runtime.setStyleById(id, "background-color: #fd02ab; border-color: #fd02ab;"))
    {
        return std::nullopt;
    }
    std::vector<uint32_t> pixels(static_cast<size_t>(runtime.width()) * runtime.height());
    runtime.renderToBgraPixels(pixels.data(), runtime.width(), runtime.height(),
                               runtime.width() * sizeof(uint32_t), runtime.dpiScale());
    int left = runtime.width();
    int right = -1;
    int top = runtime.height();
    int bottom = -1;
    for (int y = 0; y < runtime.height(); ++y)
    {
        for (int x = 0; x < runtime.width(); ++x)
        {
            if ((pixels[static_cast<size_t>(y) * runtime.width() + x] & 0xffffff) == 0xfd02ab)
            {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    runtime.setStyleById(id, "");
    if (!postprocess_test::Require(right >= left && bottom >= top, "控件不可见：" + id))
    {
        return std::nullopt;
    }
    return POINT{(left + right) / 2, (top + bottom) / 2};
}

bool ClickElement(RangeController& controller, skui::Runtime& runtime,
                  skui::win32::Win32EventAdapter& adapter, HWND window, const std::string& id)
{
    runtime.scrollIntoViewById(id);
    const auto center = FindElementCenter(runtime, id);
    if (!center)
    {
        return false;
    }
    MapClick(controller, adapter, window, runtime, *center);
    MSG message{};
    while (PeekMessageW(&message, window, WM_APP, 0xbfff, PM_REMOVE))
    {
        controller.handleWindowMessage(window, message.message, message.wParam, message.lParam,
                                       runtime);
    }
    return true;
}

bool TestCopyUsesActiveRegionRole(HWND window, const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!Require(runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                                       "resources/preview/index.html")
                                          .string()),
                 "排除区复制测试文档加载失败"))
    {
        return false;
    }
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    DownTool::DownloadRegion source;
    source.SetId("exclude-source");
    source.SetName("GIS 排除区 1");
    source.SetRole(DownTool::DownloadRegionRole::ExcludeGis);
    source.SetVertices({{116.418123456789, 39.889123456789},
                        {116.419234567891, 39.889234567891},
                        {116.419345678912, 39.888345678912},
                        {116.418456789123, 39.888456789123}});
    DomDownToolPreview::ExportConfiguration configuration;
    configuration.rangeMode = DomDownToolPreview::ExportRangeMode::CustomPolygon;
    configuration.centerWgs84 = {116.41822112345678, 39.88901312345678};
    configuration.zoom = 18;
    configuration.SetDownloadRegions({source});
    for (const int width : {1440, 700})
    {
        runtime.resize(width, width == 1440 ? 900 : 600, 1.F);
        controller.resizeRuntime(runtime);
        controller.LoadExport(runtime, configuration);
        controller.OpenExportEditor(runtime);
        const auto click = [&](const std::string& id) {
            return ClickElement(controller, runtime, adapter, window, id);
        };
        if (!click("range-select-exclude-source"))
        {
            return false;
        }
        const std::array<std::pair<const char*, DownTool::DownloadRegionRole>, 4> targets = {{
            {"include", DownTool::DownloadRegionRole::Include},
            {"exclude", DownTool::DownloadRegionRole::ExcludeGis},
            {"terrain", DownTool::DownloadRegionRole::ExcludeTerrain},
            {"flatten", DownTool::DownloadRegionRole::FlattenTerrain},
        }};
        for (const auto& [action, role] : targets)
        {
            const auto count = controller.GetRegions().size();
            if (!click(std::string("range-role-") + action) ||
                !Require(runtime.textContentById("range-copy-tip") ==
                             std::string("复制为") + DownTool::DownloadRegionRoleName(role),
                         "复制按钮提示没有跟随当前区域类型") ||
                !click("range-copy") ||
                !Require(controller.GetRegions().size() == count + 1 &&
                             controller.GetRegions().back().GetRole() == role &&
                             controller.GetSelectedId() == source.GetId(),
                         "复制类型没有跟随上方选中的区域类型"))
            {
                return false;
            }
            const auto copied = DownTool::DownloadRegionsToJson(controller.GetRegions());
            if (!Require(copied[0] == DownTool::DownloadRegionsToJson({source})[0] &&
                             copied.back()["verticesWgs84"] == copied[0]["verticesWgs84"],
                         "界面复制改变了原区域或边界坐标") ||
                !click("range-undo") ||
                !Require(controller.GetRegions().size() == count, "复制区域无法撤销") ||
                !click("range-redo") ||
                !Require(DownTool::DownloadRegionsToJson(controller.GetRegions()) == copied,
                         "复制区域无法重做"))
            {
                return false;
            }
            const auto center = FindElementCenter(runtime, "range-copy");
            if (!center)
            {
                return false;
            }
            WindowMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, 0,
                          MAKELPARAM(center->x, center->y));
            if (!WriteTaskScreenshot(runtime,
                                     testRoot / (std::string("range-copy-") + action +
                                                 (width == 1440 ? "-desktop.png" : "-narrow.png"))))
            {
                return false;
            }
            if (role != DownTool::DownloadRegionRole::FlattenTerrain && !click("range-undo"))
            {
                return false;
            }
        }
        if (!click("range-select-" + controller.GetRegions().back().GetId()) ||
            !click("range-role-include") || !click("range-copy") ||
            !Require(controller.GetRegions().size() == 3 &&
                         controller.GetRegions().back().GetRole() ==
                             DownTool::DownloadRegionRole::Include,
                     "选中平整区后无法复制为下载区域") ||
            !click("range-return-export") ||
            !Require(controller.GetExportRegions().size() == 3, "应用范围时丢失复制的区域"))
        {
            return false;
        }
    }
    return true;
}

bool TestProviderMenuScrolling(HWND window, const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!Require(runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                                       "resources/preview/index.html")
                                          .string()),
                 "地图菜单测试文档加载失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto center = FindElementCenter(runtime, "provider-select");
    if (!center)
    {
        return false;
    }
    const auto zoom = runtime.textContentById("zoom-value");
    for (const int direction : {-1, 1})
    {
        MapClick(controller, adapter, window, runtime, *center);
        POINT popupPoint{center->x, center->y + 32};
        POINT screenPoint = popupPoint;
        ClientToScreen(window, &screenPoint);
        for (int step = 0; step < 8; ++step)
        {
            WindowMessage(controller, adapter, window, runtime, WM_MOUSEWHEEL,
                          MAKEWPARAM(0, direction * WHEEL_DELTA),
                          MAKELPARAM(screenPoint.x, screenPoint.y));
            controller.resizeRuntime(runtime);
        }
        const std::string expected = direction < 0 ? "none" : "gaode.satellite";
        if (!WriteTaskScreenshot(runtime, testRoot / ("provider-scroll-" + expected + ".png")))
        {
            return false;
        }
        // 菜单每行 30px，向下滚动后选择第八行，向上滚动后选择第一行。
        popupPoint.y += direction < 0 ? 7 * 30 : 0;
        MapClick(controller, adapter, window, runtime, popupPoint);
        if (!Require(runtime.textContentById("active-provider") ==
                         (direction < 0 ? "无" : "高德卫星"),
                     "界面刷新使地图菜单无法滚动到：" + expected) ||
            !Require(runtime.textContentById("zoom-value") == zoom, "菜单滚轮误传给地图缩放"))
        {
            return false;
        }
    }
    return true;
}

bool TestUnrestrictedMapZoom()
{
    using postprocess_test::Require;
    DomDownToolPreview::MapRenderer renderer([] {});
    for (const auto& source : renderer.sources())
    {
        renderer.setView({116.397389, 39.908722}, 25.);
        if (!Require(renderer.selectSource(source.id) && renderer.zoom() == 25.,
                     "切换图源仍限制预览层级：" + source.id))
        {
            return false;
        }
        renderer.setView(renderer.center(), 0.);
        if (!Require(renderer.zoom() == 0., "图源仍有预设最低层级：" + source.id))
        {
            return false;
        }
        renderer.setView(renderer.center(), 23.);
        if (!Require(renderer.zoomAt(0., 0., 1.) && renderer.zoom() == 24.,
                     "图源无法滚轮放大到 24 级：" + source.id))
        {
            return false;
        }
        renderer.setView(renderer.center(), 30.);
        if (!Require(renderer.zoom() == 30., "图源无法设置到 30 级：" + source.id))
        {
            return false;
        }
    }
    return true;
}

bool TestFractionalViewSpanInput(HWND window, const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!Require(runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                                       "resources/preview/index.html")
                                          .string()),
                 "视野倍率测试文档加载失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    const auto type = [&](const std::string& text) {
        skui::Event selectAll;
        selectAll.type = skui::EventType::KeyDown;
        selectAll.key = 'A';
        selectAll.ctrlKey = true;
        runtime.handleEvent(selectAll);
        if (text.empty())
        {
            WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_BACK, 0);
            return;
        }
        skui::Event input;
        input.type = skui::EventType::TextInput;
        input.text = text;
        runtime.handleEvent(input);
    };
    if (!click("view-span-decrease") ||
        !Require(runtime.textContentById("view-span-value") == "0.90", "视野倍率不能按 0.1 缩小") ||
        !click("view-span-value"))
    {
        return false;
    }
    type("1.25");
    controller.resizeRuntime(runtime);
    if (!Require(runtime.textContentById("view-span-value") == "1.25",
                 "界面刷新覆盖了正在编辑的视野倍率"))
    {
        return false;
    }
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    if (!click("view-span-increase") ||
        !Require(runtime.textContentById("view-span-value") == "1.35",
                 "输入小数后视野倍率无法继续按 0.1 调整") ||
        !click("view-span-value"))
    {
        return false;
    }
    type("0.7");
    if (!click("provider-select") || !Require(runtime.textContentById("view-span-value") == "0.70",
                                              "视野倍率失焦时没有应用小数"))
    {
        return false;
    }
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_ESCAPE, 0);
    if (!click("view-span-value"))
    {
        return false;
    }
    type("2.5");
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_ESCAPE, 0);
    if (!Require(runtime.textContentById("view-span-value") == "0.70",
                 "取消编辑没有恢复原视野倍率"))
    {
        return false;
    }
    type("0.65");
    MapClick(controller, adapter, window, runtime, POINT{1000, 500});
    if (!Require(runtime.textContentById("view-span-value") == "0.65",
                 "点击地图没有提交视野倍率") ||
        !click("view-span-value"))
    {
        return false;
    }
    type("1.75");
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_TAB, 0);
    if (!Require(runtime.textContentById("view-span-value") == "1.75",
                 "Tab 没有提交视野倍率"))
    {
        return false;
    }
    for (const auto& [input, expected] :
         {std::pair{"100", "4.00"}, std::pair{"0", "0.10"}, std::pair{"", "0.10"}})
    {
        if (!click("view-span-value"))
        {
            return false;
        }
        type(input);
        WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
        if (!Require(runtime.textContentById("view-span-value") == expected,
                     "视野倍率未正确处理越界或空输入"))
        {
            return false;
        }
    }
    for (const int width : {1440, 700})
    {
        runtime.resize(width, width == 1440 ? 900 : 600, 1.F);
        controller.resizeRuntime(runtime);
        if (!click("view-span-value"))
        {
            return false;
        }
        type("1.25");
        WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
        if (!WriteTaskScreenshot(runtime,
                                 testRoot / ("view-span-" + std::to_string(width) + ".png")))
        {
            return false;
        }
    }
    return true;
}

bool TestElectronicMapSources(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    MapRenderer renderer([] {});
    for (const auto& [id, name, coordinates, cache] :
         {std::array<std::string, 4>{"tencent.electronic", "腾讯电子地图", "GCJ02 / XYZ",
                                     "cache/腾讯地图/电子地图"},
          std::array<std::string, 4>{"baidu.electronic", "百度电子地图", "BD09 / Baidu",
                                     "cache/百度地图/电子地图"}})
    {
        if (!Require(renderer.selectSource(id), "无法选择电子地图：" + id))
        {
            return false;
        }
        const auto selected = renderer.activeSource();
        if (!Require(selected.name == name && selected.coordinateSystem == coordinates &&
                         selected.cacheRoot == std::filesystem::u8path(cache),
                     "电子地图预览的图源或缓存配置错误：" + id))
        {
            return false;
        }
    }

    skui::Runtime runtime;
    if (!Require(runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                                       "resources/preview/index.html")
                                          .string()),
                 "电子地图测试文档加载失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    if (!click("open-export-page"))
    {
        return false;
    }
    ExportConfiguration configuration;
    configuration.centerWgs84 = {116.42, 39.89};
    configuration.radiusMeters = 20000.;
    configuration.zoom = 16;
    configuration.outputRoot = testRoot / "electronic-output";
    ExportContentGroup group;
    group.id = "electronic-group";
    group.name = "电子地图";
    ExportComponent component;
    component.id = "electronic-image";
    component.kind = ExportComponentKind::MergedImage;
    group.components.push_back(component);
    configuration.groups.push_back(group);
    controller.LoadExport(runtime, configuration);
    if (!click("export-act-open-params__electronic-image"))
    {
        return false;
    }
    for (const auto& [id, name] :
         {std::pair{"tencent.electronic", "腾讯电子"}, std::pair{"baidu.electronic", "百度电子"}})
    {
        const std::string action = "export-act-add-source__electronic-image__" + std::string(id);
        if (!click("export-act-open-sources__electronic-image") ||
            !Require(runtime.textContentById(action) == name,
                     "导出菜单缺少电子地图：" + std::string(id)) ||
            !click(action))
        {
            return false;
        }
    }
    std::string error;
    const auto exported = controller.ReadExport(error);
    if (!Require(exported && exported->groups.size() == 1 &&
                     exported->groups[0].components.size() == 1 &&
                     exported->groups[0].components[0].sources.size() == 2 &&
                     exported->groups[0].components[0].sources[0].id == "tencent.electronic" &&
                     exported->groups[0].components[0].sources[1].id == "baidu.electronic",
                 "电子地图没有进入导出配置：" + error) ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "electronic-sources-desktop.png"),
                 "电子地图图源界面截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    return Require(WriteTaskScreenshot(runtime, testRoot / "electronic-sources-narrow.png"),
                   "电子地图窄窗口界面截图失败");
}

bool TestTencentGis(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
            "resources/preview/index.html").string()))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id)
    {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    if (!click("gis-overlay-toggle") || !click("gis-source-select"))
    {
        return false;
    }
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_HOME, 0);
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_DOWN, 0);
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    if (!Require(WriteTaskScreenshot(runtime, testRoot / "tencent-gis-preview-source.png"),
                 "腾讯预览来源截图失败") ||
        !click("open-export-page"))
    {
        return false;
    }
    ExportConfiguration configuration;
    configuration.centerWgs84 = {116.42, 39.89};
    configuration.radiusMeters = 20000.;
    configuration.zoom = 18;
    configuration.outputRoot = testRoot / "tencent-output";
    ExportContentGroup group;
    group.id = "tencent-group";
    ExportComponent component;
    component.id = "tencent-json";
    component.kind = ExportComponentKind::GeoJson;
    ExportSourceEntry road;
    road.id = "gis.road";
    road.name = "道路";
    road.kind = ExportSourceKind::Gis;
    road.gisCategory = DownTool::AmapVectorRenderCategory::Road;
    component.sources = {road};
    group.components = {component};
    configuration.groups = {group};
    controller.LoadExport(runtime, configuration);
    if (!click("export-act-open-components__tencent-group") ||
        !click("export-act-add-component__tencent-group__building-model"))
    {
        return false;
    }
    std::string error;
    const auto withBuilding = controller.ReadExport(error);
    if (!Require(withBuilding && withBuilding->groups[0].components.size() == 2 &&
            withBuilding->groups[0].components[1].sources[0].id == "gis.tencent.building",
            "GIS 来源切换没有影响新建筑组件：" + error) ||
        !click("export-act-remove-component__" + withBuilding->groups[0].components[1].id))
    {
        return false;
    }
    if (!click("export-act-open-params__tencent-json") ||
        !click("export-act-open-sources__tencent-json") ||
        !Require(runtime.textContentById(
            "export-act-add-source__tencent-json__gis.building") == "高德·建筑",
            "导出菜单中的高德建筑缺少平台名称") ||
        !Require(runtime.textContentById(
            "export-act-add-source__tencent-json__gis.tencent.building") == "腾讯·建筑",
            "导出菜单缺少腾讯建筑") ||
        !click("export-act-add-source__tencent-json__gis.tencent.building"))
    {
        return false;
    }
    const auto exported = controller.ReadExport(error);
    if (!Require(exported && exported->groups[0].components[0].sources.size() == 2 &&
            exported->groups[0].components[0].sources[0].id == "gis.road" &&
            exported->groups[0].components[0].sources[0].name == "高德·道路" &&
            exported->groups[0].components[0].sources[1].id == "gis.tencent.building",
            "混合来源没有写入导出配置：" + error) ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "tencent-gis-export-desktop.png"),
                 "腾讯导出来源截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    return Require(runtime.scrollIntoViewById("export-param-panel"), "腾讯窄窗口无法定位参数面板") &&
        Require(WriteTaskScreenshot(runtime, testRoot / "tencent-gis-export-narrow.png"),
                   "腾讯导出窄窗口截图失败");
}

bool TestBaiduGis(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
        "resources/preview/index.html").string()))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    if (!click("gis-overlay-toggle") || !click("gis-source-select"))
    {
        return false;
    }
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_END, 0);
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    if (!click("gis-overlay-road") ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "baidu-gis-preview-source.png"),
        "百度预览来源截图失败") || !click("open-export-page"))
    {
        return false;
    }
    if (!click("export-config-radius"))
    {
        return false;
    }
    skui::Event selectAll;
    selectAll.type = skui::EventType::KeyDown;
    selectAll.key = 'A';
    selectAll.ctrlKey = true;
    runtime.handleEvent(selectAll);
    for (const wchar_t character : std::wstring(L"300"))
    {
        WindowMessage(controller, adapter, window, runtime, WM_CHAR, character, 0);
    }
    std::string error;
    const auto preview = controller.ReadExport(error);
    bool hasBaidu = false;
    if (preview)
    {
        for (const auto& group : preview->groups)
        {
            for (const auto& component : group.components)
            {
                for (const auto& source : component.sources)
                {
                    hasBaidu |= source.id.starts_with("gis.baidu.");
                }
            }
        }
    }
    if (!Require(hasBaidu, "当前预览导出没有使用百度来源：" + error))
    {
        return false;
    }
    ExportConfiguration configuration;
    configuration.centerWgs84 = {116.42, 39.89};
    configuration.radiusMeters = 300.;
    configuration.zoom = 19;
    configuration.outputRoot = testRoot / "baidu-output";
    ExportContentGroup group;
    group.id = "baidu-group";
    ExportComponent component;
    component.id = "baidu-json";
    component.kind = ExportComponentKind::GeoJson;
    ExportSourceEntry source;
    source.id = "gis.road";
    source.kind = ExportSourceKind::Gis;
    source.gisCategory = DownTool::AmapVectorRenderCategory::Road;
    component.sources = {source};
    group.components = {component};
    configuration.groups = {group};
    controller.LoadExport(runtime, configuration);
    if (!click("export-act-open-params__baidu-json") ||
        !click("export-act-open-sources__baidu-json") ||
        !Require(runtime.textContentById("export-act-add-source__baidu-json__gis.baidu.road") ==
            "百度·道路", "导出菜单缺少百度道路") ||
        !click("export-act-add-source__baidu-json__gis.baidu.road"))
    {
        return false;
    }
    const auto exported = controller.ReadExport(error);
    if (!Require(exported && exported->groups[0].components[0].sources.size() == 2 &&
        exported->groups[0].components[0].sources[1].id == "gis.baidu.road",
        "百度混合来源没有写入导出配置：" + error) ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "baidu-gis-export-desktop.png"),
            "百度导出桌面截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    return Require(runtime.scrollIntoViewById("export-param-panel"), "百度窄窗口无法定位参数面板") &&
        Require(WriteTaskScreenshot(runtime, testRoot / "baidu-gis-export-narrow.png"),
            "百度导出窄窗口截图失败");
}

bool TestShapefileComponent(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!Require(runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                                       "resources/preview/index.html")
                                          .string()),
                 "SHP 组件测试文档加载失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    if (!click("open-export-page"))
    {
        return false;
    }
    ExportConfiguration configuration;
    configuration.centerWgs84 = {116.42, 39.89};
    configuration.radiusMeters = 20000.;
    configuration.zoom = 16;
    ExportContentGroup group;
    group.id = "g1";
    group.name = "当前预览";
    configuration.groups = {group};
    controller.LoadExport(runtime, configuration);
    if (!click("export-act-open-components__g1") ||
        !Require(runtime.textContentById("export-act-add-component__g1__shapefile") == "SHP",
                 "添加组件菜单缺少 SHP") ||
        !click("export-act-add-component__g1__shapefile"))
    {
        return false;
    }
    const auto children = runtime.childElementIdsById("export-group__g1");
    constexpr std::string_view kComponentPrefix = "export-act-select-component__";
    const auto row = std::find_if(children.begin(), children.end(),
                                  [&](const auto& id) { return id.starts_with(kComponentPrefix); });
    if (!Require(row != children.end(), "添加 SHP 后没有显示组件行"))
    {
        return false;
    }
    const auto componentId = row->substr(kComponentPrefix.size());
    if (!click("export-act-open-sources__" + componentId) ||
        !Require(
            !runtime.textContentById("export-act-add-source__" + componentId + "__google.satellite")
                 .has_value(),
            "SHP 图源菜单错误提供了影像") ||
        !click("export-act-add-source__" + componentId + "__gis.building") ||
        !click("export-act-toggle-labels__" + componentId))
    {
        return false;
    }
    std::string error;
    auto exported = controller.ReadExport(error);
    if (!Require(exported &&
                     exported->groups[0].components[0].kind == ExportComponentKind::Shapefile &&
                     exported->groups[0].components[0].includeLabels &&
                     exported->groups[0].components[0].includeBaseLayers &&
                     exported->groups[0].components[0].sources.size() == 1 &&
                     exported->groups[0].components[0].sources[0].name == "高德·建筑" &&
                     exported->groups[0].components[0].sources[0].gisCategory ==
                         DownTool::AmapVectorRenderCategory::Building,
                 "SHP 图源或图层选项没有进入导出配置：" + error) ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "shapefile-component-desktop.png"),
                 "SHP 桌面界面截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    if (!runtime.scrollIntoViewById("export-param-panel") ||
        !click("export-act-toggle-base-layers__" + componentId))
    {
        return false;
    }
    exported = controller.ReadExport(error);
    return Require(exported && !exported->groups[0].components[0].includeBaseLayers &&
                       exported->groups[0].components[0].includeLabels,
                   "窄窗口内无法修改 SHP 图层选项") &&
           Require(WriteTaskScreenshot(runtime, testRoot / "shapefile-component-narrow.png"),
                   "SHP 窄窗口界面截图失败") &&
           runtime.scrollIntoViewById("export-act-done-panel") &&
           click("export-act-done-panel");
}

bool TestShapefileMapPreview(const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    const auto path = testRoot / "map-preview.shp";
    if (!Require(shapefile_test::WriteFixture(path), "创建地图预览 SHP 失败"))
    {
        return false;
    }
    DomDownToolPreview::MapRenderer renderer([] {});
    renderer.selectSource("none");
    renderer.setView({116.421, 39.891}, 17.);
    SkBitmap bitmap;
    bitmap.allocN32Pixels(800, 600);
    SkCanvas canvas(bitmap);
    renderer.draw(canvas, 800, 600, 1.F);
    const auto point = renderer.ScreenPointForLonLat({116.42025, 39.89025});
    const auto hole = renderer.ScreenPointForLonLat({116.42125, 39.89125});
    if (!Require(point && hole, "地图预览坐标定位失败"))
    {
        return false;
    }
    const auto colorAt = [&bitmap](const auto& position) {
        return bitmap.getColor(static_cast<int>(position.x), static_cast<int>(position.y));
    };
    const SkColor baseline = colorAt(*point);
    const SkColor holeBaseline = colorAt(*hole);
    DownTool::ShapefileImportOptions source;
    source.SetPath(path);
    const auto waitForRead = [&renderer] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (renderer.IsLocalShapefileLoading() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return !renderer.IsLocalShapefileLoading();
    };
    renderer.SetLocalShapefiles({source});
    if (!Require(waitForRead() && renderer.GetLocalShapefileError().empty(),
                 "本地 SHP 后台预览读取失败：" + renderer.GetLocalShapefileError()))
    {
        return false;
    }
    renderer.draw(canvas, 800, 600, 1.F);
    if (!Require(colorAt(*point) != baseline && colorAt(*hole) == holeBaseline,
                 "SHP 预览未按 WGS84 原坐标绘制，或丢失面内孔洞") ||
        !Require(cv::imwrite((testRoot / "shapefile-map-preview.png").string(),
                             cv::Mat(600, 800, CV_8UC4, bitmap.getPixels())),
                 "SHP 地图预览截图失败"))
    {
        return false;
    }
    renderer.SetLocalShapefiles({});
    renderer.draw(canvas, 800, 600, 1.F);
    if (!Require(!renderer.IsLocalShapefileLoading() && colorAt(*point) == baseline,
                 "删除本地图层后地图仍保留几何"))
    {
        return false;
    }
    for (int index = 0; index < 20; ++index)
    {
        renderer.SetLocalShapefiles({source});
        renderer.SetLocalShapefiles({});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    renderer.draw(canvas, 800, 600, 1.F);
    if (!Require(colorAt(*point) == baseline && !renderer.IsLocalShapefileLoading(),
                 "删除后旧异步任务重新显示了图层"))
    {
        return false;
    }
    source.SetPath(testRoot / "missing.shp");
    renderer.SetLocalShapefiles({source});
    if (!Require(waitForRead() && !renderer.GetLocalShapefileError().empty(),
                 "SHP 读取失败没有保留用户可见的错误"))
    {
        return false;
    }
    renderer.SetLocalShapefiles({});
    return Require(renderer.GetLocalShapefileError().empty(), "删除失败的图层后没有清理错误");
}

bool TestShapefileDrop(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    const auto path = testRoot / std::filesystem::u8path("拖入数据/用户建筑.shp");
    if (!Require(shapefile_test::WriteFixture(path), "创建拖入 SHP 失败"))
    {
        return false;
    }
    skui::Runtime runtime;
    if (!runtime.loadDocument(
            (std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "resources/preview/index.html")
                .string()))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    const auto readTaskCount = [&testRoot]() -> size_t
    {
        std::ifstream input(testRoot / std::filesystem::u8path("cache/导出任务.json"));
        return input ? nlohmann::json::parse(input).at("tasks").size() : 0;
    };
    const auto taskCount = readTaskCount();
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto drop = [&] {
        const auto name = path.wstring();
        const auto bytes = sizeof(DROPFILES) + (name.size() + 2) * sizeof(wchar_t);
        const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!memory)
        {
            return false;
        }
        auto* files = static_cast<DROPFILES*>(GlobalLock(memory));
        if (!files)
        {
            GlobalFree(memory);
            return false;
        }
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        auto* buffer = reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(files) + files->pFiles);
        std::copy(name.begin(), name.end(), buffer);
        GlobalUnlock(memory);
        return controller.handleWindowMessage(window, WM_DROPFILES,
                                              reinterpret_cast<WPARAM>(memory), 0, runtime);
    };
    auto prj = path;
    prj.replace_extension(".prj");
    shapefile_test::WriteText(prj, "PROJCS[\"Unsupported\"]");
    if (!Require(drop() &&
                     runtime.textContentById("shp-preview-message").value_or("").find("暂不支持") !=
                         std::string::npos,
                 "拖入其他坐标系时没有显示暂不支持"))
    {
        return false;
    }
    shapefile_test::WriteText(prj, shapefile_test::kWgs84Prj);
    std::string error;
    if (!Require(drop(), "WGS84 SHP 拖入未处理") ||
        !Require(runtime.textContentById("shp-preview-count") == "本地图层 (1)" &&
                     !controller.ReadExport(error).has_value() &&
                     readTaskCount() == taskCount,
                 "拖入 SHP 应先预览，不应自动创建导出草稿") ||
        !Require(drop() && runtime.textContentById("shp-preview-count") == "本地图层 (1)",
                 "重复拖入不应重复创建本地图层") ||
        !ClickElement(controller, runtime, adapter, window, "shp-focus-shp.1") ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "shapefile-preview-desktop.png"),
                 "SHP 预览桌面截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    if (!Require(WriteTaskScreenshot(runtime, testRoot / "shapefile-preview-narrow.png"),
                 "SHP 预览窄窗口截图失败") ||
        !ClickElement(controller, runtime, adapter, window, "shp-visible-shp.1") ||
        !ClickElement(controller, runtime, adapter, window, "shp-preview-export"))
    {
        return false;
    }
    const auto hasPath = [&path](const ExportConfiguration& config) {
        for (const auto& group : config.groups)
        {
            for (const auto& component : group.components)
            {
                if (std::ranges::any_of(component.sources, [&path](const auto& source) {
                        return source.GetShapefile().GetPath() == path;
                    }))
                {
                    return true;
                }
            }
        }
        return false;
    };
    const auto hiddenConfiguration = controller.ReadExport(error);
    if (!Require(hiddenConfiguration && !hasPath(*hiddenConfiguration),
                 "隐藏的本地图层不应默认加入新建导出") ||
        !ClickElement(controller, runtime, adapter, window, "export-back-preview") ||
        !ClickElement(controller, runtime, adapter, window, "shp-visible-shp.1") ||
        !ClickElement(controller, runtime, adapter, window, "shp-preview-export"))
    {
        return false;
    }
    const auto configuration = controller.ReadExport(error);
    if (!Require(configuration && !configuration->groups.empty() &&
                     std::abs(configuration->centerWgs84.lon - 116.431) < 1.e-9 &&
                     std::abs(configuration->centerWgs84.lat - 39.891) < 1.e-9 &&
                     configuration->radiusMeters > 900.,
                 "点击导出后无法从预览生成 SHP 导出配置：" + error))
    {
        return false;
    }
    const ExportComponent* imported = nullptr;
    size_t importedSourceIndex = 0;
    for (const auto& group : configuration->groups)
    {
        for (const auto& component : group.components)
        {
            const auto source = std::ranges::find_if(component.sources, [&](const auto& entry) {
                return entry.GetShapefile().GetPath() == path;
            });
            if (source != component.sources.end())
            {
                imported = &component;
                importedSourceIndex =
                    static_cast<size_t>(std::distance(component.sources.begin(), source));
            }
        }
    }
    if (!Require(imported != nullptr, "拖入 SHP 没有加入组件数据源"))
    {
        return false;
    }
    const auto id = imported->id;
    const auto sourceIndex = std::to_string(importedSourceIndex);
    if (!Require(runtime.textContentById("export-act-shp-height__" + id + "__" + sourceIndex)
                     .has_value(),
                 "缺少 SHP 高度字段选择") ||
        !Require(WriteTaskScreenshot(runtime, testRoot / "shapefile-import-desktop.png"),
                 "SHP 导入桌面截图失败"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    runtime.scrollIntoViewById("export-param-panel");
    if (!Require(WriteTaskScreenshot(runtime, testRoot / "shapefile-import-narrow.png"),
                 "SHP 导入窄窗口截图失败") ||
        !ClickElement(controller, runtime, adapter, window,
                      "export-act-remove-source__" + id + "__" + sourceIndex) ||
        !ClickElement(controller, runtime, adapter, window, "export-act-open-sources__" + id))
    {
        return false;
    }
    if (!Require(runtime.textContentById("export-act-add-source__" + id + "__shp.1") ==
                     "用户建筑 (SHP)",
                 "移除后本地 SHP 没有保留在图源菜单供其他组件选择") ||
        !ClickElement(controller, runtime, adapter, window,
                      "export-act-add-source__" + id + "__shp.1"))
    {
        return false;
    }
    if (!ClickElement(controller, runtime, adapter, window, "export-back-preview") ||
        !ClickElement(controller, runtime, adapter, window, "shp-remove-shp.1") ||
        !Require(runtime.textContentById("shp-preview-count") == "本地图层 (0)" &&
                     std::filesystem::exists(path),
                 "移除本地图层应清理列表并保留磁盘上的源文件"))
    {
        return false;
    }
    const auto removedConfiguration = controller.ReadExport(error);
    if (!Require(removedConfiguration && !hasPath(*removedConfiguration),
                 "移除本地图层没有清理当前导出草稿引用") ||
        !Require(drop(), "删除后应允许重新拖入 SHP"))
    {
        return false;
    }
    const auto secondPath = testRoot / std::filesystem::u8path("另一任务/用户建筑.shp");
    if (!Require(shapefile_test::WriteFixture(secondPath), "创建另一任务 SHP 失败"))
    {
        return false;
    }
    auto restored = *configuration;
    for (auto& group : restored.groups)
    {
        for (auto& component : group.components)
        {
            for (auto& source : component.sources)
            {
                if (source.GetShapefile().GetPath() == path)
                {
                    auto options = source.GetShapefile();
                    options.SetPath(secondPath);
                    source.SetShapefile(options);
                }
            }
        }
    }
    controller.LoadExport(runtime, restored);
    const auto restoredConfiguration = controller.ReadExport(error);
    if (!Require(restoredConfiguration.has_value(), "另一任务 SHP 参数恢复失败：" + error))
    {
        return false;
    }
    for (const auto& group : restoredConfiguration->groups)
    {
        for (const auto& component : group.components)
        {
            for (const auto& source : component.sources)
            {
                if (source.GetShapefile().GetPath() == secondPath)
                {
                    if (!Require(source.id != "shp.1", "不同任务的 SHP 图源 ID 冲突"))
                    {
                        return false;
                    }
                    runtime.resize(1050, 900, 1.5F);
                    controller.resizeRuntime(runtime);
                    return Require(
                               WriteTaskScreenshot(runtime,
                                                   testRoot / "shapefile-preview-multiple.png"),
                               "多文件和高 DPI 预览截图失败") &&
                           ClickElement(controller, runtime, adapter, window,
                                        "shp-remove-shp.1") &&
                           Require(runtime.textContentById("shp-preview-count") ==
                                       "本地图层 (1)",
                                   "删除第一个文件不应影响另一个本地图层") &&
                           ClickElement(controller, runtime, adapter, window,
                                        "shp-focus-" + source.id) &&
                           ClickElement(controller, runtime, adapter, window,
                                        "shp-remove-" + source.id) &&
                           Require(runtime.textContentById("shp-preview-count") ==
                                           "本地图层 (0)" &&
                                       std::filesystem::exists(path) &&
                                       std::filesystem::exists(secondPath) &&
                                       readTaskCount() == taskCount,
                                   "多文件移除不应删除源文件或自动提交导出任务");
                }
            }
        }
    }
    return Require(false, "恢复参数后丢失另一任务 SHP");
}

bool TestPerTileFbxOption(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!runtime.loadDocument(
            (std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "resources/preview/index.html")
                .string()))
    {
        return false;
    }
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    ExportConfiguration configuration;
    configuration.centerWgs84 = {116.42, 39.89};
    configuration.radiusMeters = 1000.;
    ExportComponent component;
    component.id = "terrain";
    component.kind = ExportComponentKind::BasePlate;
    component.sources.push_back({"google.satellite", "Google 卫星", ExportSourceKind::Imagery,
                                 testRoot / "per-tile-cache"});
    ExportContentGroup group;
    group.id = "g1";
    group.components = {component};
    configuration.groups = {group};
    runtime.resize(1440, 900, 1.F);
    controller.resizeRuntime(runtime);
    if (!click("open-export-page"))
    {
        return false;
    }
    for (const int width : {1440, 700})
    {
        runtime.resize(width, width == 1440 ? 900 : 600, 1.F);
        controller.resizeRuntime(runtime);
        controller.LoadExport(runtime, configuration);
        std::string error;
        auto exported = controller.ReadExport(error);
        if (!Require(exported && !exported->groups[0].components[0].GetFbxPerTile(),
                     "新建底板没有默认合并导出") ||
            !click("export-act-toggle-fbx-per-tile__terrain"))
        {
            return false;
        }
        exported = controller.ReadExport(error);
        if (!Require(exported && exported->groups[0].components[0].GetFbxPerTile(),
                     "独立 FBX 开关没有进入任务配置：" + error) ||
            !Require(WriteTaskScreenshot(runtime,
                                         testRoot / (width == 1440 ? "per-tile-fbx-desktop.png"
                                                                   : "per-tile-fbx-narrow.png")),
                     "独立 FBX 参数界面截图失败"))
        {
            return false;
        }
        controller.LoadExport(runtime, *exported);
        if (!click("export-edit-range") || !click("range-return-export"))
        {
            return false;
        }
        exported = controller.ReadExport(error);
        if (!Require(exported && exported->groups[0].components[0].GetFbxPerTile(),
                     "重开配置或编辑范围丢失了独立 FBX 设置") ||
            !click("export-act-toggle-fbx-per-tile__terrain"))
        {
            return false;
        }
        exported = controller.ReadExport(error);
        if (!Require(exported && !exported->groups[0].components[0].GetFbxPerTile(),
                     "无法恢复合并 FBX 模式"))
        {
            return false;
        }
    }
    configuration.groups.clear();
    configuration.exportFbx = true;
    configuration.SetFbxPerTile(true);
    configuration.satelliteSources = {
        {"google.satellite", "Google 卫星", testRoot / "per-tile-cache"}};
    controller.LoadExport(runtime, configuration);
    std::string error;
    const auto migrated = controller.ReadExport(error);
    return Require(migrated && migrated->groups.size() == 1 &&
                       migrated->groups[0].components[0].GetFbxPerTile(),
                   "旧任务转为组件配置时丢失了独立 FBX 设置");
}

bool TestFlattenRegionEditing(HWND window, const std::filesystem::path& testRoot)
{
    using namespace DomDownToolPreview;
    using postprocess_test::Require;
    skui::Runtime runtime;
    if (!runtime.loadDocument(
            (std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "resources/preview/index.html")
                .string()))
    {
        return false;
    }
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    ExportConfiguration configuration;
    configuration.rangeMode = ExportRangeMode::CustomPolygon;
    configuration.centerWgs84 = {116.41822112345678, 39.88901312345678};
    configuration.zoom = 16;
    configuration.polygonsWgs84 = {
        {{116.4, 39.87}, {116.44, 39.87}, {116.44, 39.91}, {116.4, 39.91}}};
    configuration.SetDownloadRegions(DownTool::MakeDownloadRegions(configuration.polygonsWgs84));
    ExportComponent terrain;
    terrain.id = "terrain";
    terrain.kind = ExportComponentKind::BasePlate;
    terrain.exportElevation = true;
    terrain.sources.push_back(
        {"google.satellite", "Google 卫星", ExportSourceKind::Imagery, testRoot / "flatten-cache"});
    ExportContentGroup group;
    group.id = "terrain-group";
    group.name = "地形";
    group.components = {terrain};
    configuration.groups = {group};
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    for (const int width : {1440, 700})
    {
        runtime.resize(width, width == 1440 ? 900 : 600, 1.F);
        controller.resizeRuntime(runtime);
        controller.LoadExport(runtime, configuration);
        controller.OpenExportEditor(runtime);
        if (!click("range-role-flatten") || !click("range-tool-rectangle"))
        {
            return false;
        }
        MapMessage(controller, adapter, window, runtime, WM_LBUTTONDOWN, {420, 220});
        MapMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, {620, 390});
        MapMessage(controller, adapter, window, runtime, WM_LBUTTONUP, {620, 390});
        if (!Require(controller.GetRegions().size() == 2 &&
                         controller.GetRegions().back().GetRole() ==
                             DownTool::DownloadRegionRole::FlattenTerrain &&
                         controller.GetRegions().back().GetName() == "平整区域 1",
                     "绘制矩形没有生成独立的平整区域") ||
            !click("range-tool-select"))
        {
            return false;
        }
        const auto flattenedId = controller.GetRegions().back().GetId();
        if (!click("range-enable-" + flattenedId) ||
            !Require(DownTool::GetRegionPolygons(controller.GetRegions(),
                                                 DownTool::DownloadRegionRole::FlattenTerrain)
                         .empty(),
                     "平整区停用后仍参与高程处理") ||
            !click("range-undo") || !click("range-select-" + flattenedId))
        {
            return false;
        }
        const auto saved = DownTool::DownloadRegionsToJson(controller.GetRegions());
        if (!Require(WriteScreenshot(runtime, controller.GetRegions(),
                                     testRoot / (width == 1440 ? "flatten-region-desktop.png"
                                                               : "flatten-region-narrow.png"),
                                     width, runtime.height()),
                     "平整区域界面截图失败") ||
            !click("range-return-export"))
        {
            return false;
        }
        std::string error;
        const auto applied = controller.ReadExport(error);
        if (!Require(applied &&
                         DownTool::DownloadRegionsToJson(applied->GetDownloadRegions()) == saved &&
                         applied->polygonsWgs84.size() == 1 &&
                         applied->centerWgs84.lon == configuration.centerWgs84.lon &&
                         applied->centerWgs84.lat == configuration.centerWgs84.lat,
                     "平整区域没有保存到任务，或改变了下载范围与模型中心：" + error))
        {
            return false;
        }
        controller.LoadExport(runtime, *applied);
        controller.OpenExportEditor(runtime);
        if (!Require(DownTool::DownloadRegionsToJson(controller.GetRegions()) == saved,
                     "重新打开配置丢失了平整区域") ||
            !click("range-cancel-export"))
        {
            return false;
        }
    }
    return true;
}

bool TestTaskFbxPreviewLoadsEveryTile(HWND window, const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    using Json = nlohmann::json;
    const auto outputRoot = testRoot / "many-fbx-tiles";
    if (!postprocess_test::ResetDirectory(outputRoot))
    {
        return false;
    }
    const auto fbxRoot = outputRoot / "terrain/FBX";
    postprocess_test::TestPostProcessDown tool;
    tool.Configure(outputRoot / "cache", outputRoot / "XYZ", 18, 32, {131066, 131066, 18},
                   {131077, 131077, 18}, {0., 0.});
    tool.SetPostProcessMaxZoomOnlyEnabled(true);
    tool.SetPostProcessFbxEnabled(true);
    tool.SetFbxPerTile(true);
    tool.SetFbxOutputPath(fbxRoot.string());
    tool.SetFbxTextureSize(64);
    tool.SetFbxMeshSubdivision(2);
    tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
    if (!postprocess_test::WriteSourceTiles(tool))
    {
        return false;
    }
    tool.RunPostProcessForTest();
    size_t fileCount = 0;
    size_t triangleCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fbxRoot))
    {
        if (entry.path().extension() != ".fbx")
        {
            continue;
        }
        Assimp::Importer importer;
        const auto* scene = postprocess_test::ReadFbxScene(importer, entry.path());
        if (!Require(scene && scene->mNumMeshes == 1, "独立底板测试网格生成失败"))
        {
            return false;
        }
        ++fileCount;
        triangleCount += scene->mMeshes[0]->mNumFaces;
    }
    if (!Require(fileCount > 32, "测试没有生成超过 32 个独立 FBX"))
    {
        return false;
    }
    const auto recordsPath = testRoot / std::filesystem::u8path("cache/导出任务.json");
    std::filesystem::create_directories(recordsPath.parent_path());
    const Json task = {{"id", "many-fbx-tiles"},
                       {"title", "多块底板预览"},
                       {"state", "succeeded"},
                       {"stage", "已完成"},
                       {"detail", "底板 FBX"},
                       {"createdAt", "2026-10-22 14:00:00"},
                       {"outputPath", postprocess_test::PathToUtf8String(outputRoot)}};
    std::ofstream(recordsPath) << Json({{"version", 1}, {"tasks", Json::array({task})}}).dump(2);
    skui::Runtime runtime;
    if (!runtime.loadDocument(
            (std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "resources/preview/index.html")
                .string()))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    if (!ClickElement(controller, runtime, adapter, window, "open-task-page") ||
        !ClickElement(controller, runtime, adapter, window, "task-preview-many-fbx-tiles"))
    {
        return false;
    }
    const auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1440, 900));
    if (!surface)
    {
        return false;
    }
    std::string stats;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do
    {
        controller.draw(*surface->getCanvas(), 1440, 900, 1.F);
        stats = runtime.textContentById("fbx-preview-stats").value_or("");
        if (stats.find("总 DrawCall") != std::string::npos)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    const bool loadedAll = Require(
        stats.find("总 DrawCall " + std::to_string(fileCount) + " ·") != std::string::npos &&
            stats.find("总面数 " + std::to_string(triangleCount) + " ·") != std::string::npos,
        "任务预览没有加载全部 " + std::to_string(fileCount) + " 块底板：" + stats);
    return ClickElement(controller, runtime, adapter, window, "fbx-preview-back") && loadedAll;
}

bool TestTaskActions(HWND window, const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    using Json = nlohmann::json;
    const auto recordsPath = testRoot / std::filesystem::u8path("cache/导出任务.json");
    const auto outputRoot = testRoot / "blocked-task-output";
    std::filesystem::create_directories(recordsPath.parent_path());
    std::filesystem::create_directories(outputRoot);
    // 用非目录阻止实际下载，在生产控制器中验证提交和失败返回，测试不访问网络。
    std::ofstream(outputRoot / std::filesystem::u8path("导出任务")) << "blocked";
    const auto outputPath = outputRoot / std::filesystem::u8path("导出任务/task-ui");
    const Json source = {{"id", "google.satellite"}, {"name", "Google 卫星"}, {"kind", "imagery"}};
    const Json component = {
        {"id", "image"}, {"kind", "merged-image"}, {"sources", Json::array({source})}};
    const Json group = {{"id", "g1"},
                        {"name", "卫星底图"},
                        {"enabled", true},
                        {"components", Json::array({component})}};
    const Json configuration = {
        {"centerWgs84", {{"lon", 116.41822112345678}, {"lat", 39.88901312345678}}},
        {"radiusMeters", 20000.},
        {"zoom", 18},
        {"rangeMode", "center-radius"},
        {"outputRoot", postprocess_test::PathToUtf8String(outputRoot)},
        {"groups", Json::array({group})}};
    const Json task = {{"id", "task-ui"},
                       {"title", "北京高精度地形"},
                       {"state", "succeeded"},
                       {"stage", "已完成"},
                       {"detail", "合并大图 · 底板 FBX"},
                       {"createdAt", "2026-10-20 12:00:00"},
                       {"outputPath", postprocess_test::PathToUtf8String(outputPath)},
                       {"configuration", configuration}};
    std::ofstream(recordsPath) << Json({{"version", 1}, {"tasks", Json::array({task})}}).dump(2);

    skui::Runtime runtime;
    if (!Require(runtime.loadDocument((std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                                       "resources/preview/index.html")
                                          .string()),
                 "任务测试文档加载失败"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    RangeController controller;
    controller.attachRuntime(runtime);
    controller.handleWindowMessage(window, WM_NULL, 0, 0, runtime);
    skui::win32::Win32EventAdapter adapter(runtime);
    const auto click = [&](const std::string& id) {
        return ClickElement(controller, runtime, adapter, window, id);
    };
    const auto typeText = [&](const std::wstring& value) {
        skui::Event selectAll;
        selectAll.type = skui::EventType::KeyDown;
        selectAll.key = 'A';
        selectAll.ctrlKey = true;
        runtime.handleEvent(selectAll);
        for (const wchar_t character : value)
        {
            WindowMessage(controller, adapter, window, runtime, WM_CHAR, character, 0);
        }
    };
    const auto verifyTooltip = [&](const std::string& buttonId, const std::string& tooltipId,
                                   const std::string& text, const std::string& screenshot) {
        const auto center = FindElementCenter(runtime, buttonId);
        if (!center || !Require(runtime.textContentById(tooltipId) == text,
                                "任务按钮缺少悬停说明：" + buttonId))
        {
            return false;
        }
        const auto tooltipPixels = [&] {
            std::vector<uint32_t> pixels(static_cast<size_t>(runtime.width()) * runtime.height());
            runtime.renderToBgraPixels(pixels.data(), runtime.width(), runtime.height(),
                                       runtime.width() * sizeof(uint32_t), runtime.dpiScale());
            return std::count_if(pixels.begin(), pixels.end(),
                                 [](uint32_t pixel) { return (pixel & 0xffffff) == 0xfd02ab; });
        };
        runtime.setStyleById(tooltipId, "background-color: #fd02ab;");
        WindowMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, 0, MAKELPARAM(4, 70));
        const bool hidden = tooltipPixels() == 0;
        WindowMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, 0,
                      MAKELPARAM(center->x, center->y));
        const bool visible =
            tooltipPixels() > 4000 * runtime.effectiveScale() * runtime.effectiveScale();
        runtime.setStyleById(tooltipId, "");
        const bool written = WriteTaskScreenshot(runtime, testRoot / screenshot);
        WindowMessage(controller, adapter, window, runtime, WM_MOUSEMOVE, 0, MAKELPARAM(4, 70));
        return Require(hidden && visible && written, "悬停说明没有按鼠标位置显示：" + buttonId);
    };
    const auto verifyTaskTooltips = [&](const std::string& suffix) {
        const std::array<std::pair<const char*, const char*>, 6> tooltips = {{
            {"open", "打开导出目录"},
            {"config", "复制参数新建任务"},
            {"edit", "修改参数并重新下载此任务"},
            {"rename", "重命名任务"},
            {"remove", "移除任务记录（保留导出文件）"},
            {"delete", "永久删除任务记录和导出文件"},
        }};
        for (const auto& [action, text] : tooltips)
        {
            const std::string prefix = std::string("task-") + action;
            if (!verifyTooltip(prefix + "-task-ui", prefix + "-tip-task-ui", text,
                               prefix + "-tooltip" + suffix + ".png"))
            {
                return false;
            }
        }
        return verifyTooltip("tasks-clear-finished", "tasks-clear-finished-tip",
                             "移除已结束记录，保留文件", "tasks-clear-tooltip" + suffix + ".png") &&
               verifyTooltip("tasks-new-export", "tasks-new-export-tip", "新建导出任务",
                             "tasks-new-tooltip" + suffix + ".png") &&
               verifyTooltip("tasks-back-preview", "tasks-back-preview-tip", "返回地图预览",
                             "tasks-back-tooltip" + suffix + ".png");
    };
    if (!click("open-task-page") ||
        !verifyTooltip("task-preview-task-ui", "task-preview-tip-task-ui", "预览 FBX 网格",
                       "task-preview-tooltip.png") ||
        !click("task-rename-task-ui") ||
        !verifyTooltip("task-rename-cancel", "task-rename-cancel-tip", "取消修改任务名称",
                       "task-rename-cancel-tooltip.png") ||
        !verifyTooltip("task-rename-save", "task-rename-save-tip", "保存任务名称",
                       "task-rename-save-tooltip.png"))
    {
        return false;
    }
    typeText(L" ");
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_RETURN, 0);
    if (!Require(runtime.textContentById("task-rename-error") == "任务名称不能为空。",
                 "空任务名称没有显示错误"))
    {
        return false;
    }
    typeText(L"北京外围地形 <低精度>");
    if (!WriteTaskScreenshot(runtime, testRoot / "task-rename.png") || !click("task-rename-save") ||
        !Require(runtime.textContentById("export-task-list-items")->find("北京外围地形 <低精度>") !=
                     std::string::npos,
                 "任务名称没有更新到列表") ||
        !verifyTaskTooltips("") || !click("tasks-clear-finished") ||
        !verifyTooltip("tasks-cancel-clear", "tasks-cancel-clear-tip", "取消移除，保留任务记录",
                       "tasks-cancel-clear-tooltip.png") ||
        !verifyTooltip("tasks-confirm-clear", "tasks-confirm-clear-tip", "移除已结束记录，保留文件",
                       "tasks-confirm-clear-tooltip.png") ||
        !click("tasks-cancel-clear") || !click("task-delete-task-ui") ||
        !verifyTooltip("task-delete-cancel", "task-delete-cancel-tip", "取消删除，保留任务和文件",
                       "task-delete-cancel-tooltip.png") ||
        !verifyTooltip("task-delete-accept", "task-delete-accept-tip", "永久删除任务记录和导出文件",
                       "task-delete-accept-tooltip.png") ||
        !click("task-delete-cancel") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-actions-desktop.png") ||
        !click("task-config-task-ui") ||
        !Require(runtime.textContentById("export-submit") == "加入导出队列" &&
                     runtime.textContentById("export-page-title") == "导出配置",
                 "原有复制配置入口错误地进入了重新下载模式") ||
        !click("export-act-close-panel") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-copy-config.png") ||
        !click("export-open-tasks") || !click("task-edit-task-ui") ||
        !Require(runtime.textContentById("export-submit") == "重新下载",
                 "修改参数按钮没有打开原任务编辑状态") ||
        !click("export-task-name"))
    {
        return false;
    }
    typeText(L"取消的任务名");
    if (!click("export-config-radius"))
    {
        return false;
    }
    typeText(L"40000");
    std::string error;
    auto edited = controller.ReadExport(error);
    if (!Require(edited && edited->radiusMeters == 40000. &&
                     edited->centerWgs84.lon == 116.41822112345678 &&
                     edited->centerWgs84.lat == 39.88901312345678,
                 "修改参数丢失半径或中心点精度：" + error) ||
        !click("export-act-close-panel") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-edit-config.png") ||
        !click("export-cancel") ||
        !Require(runtime.textContentById("export-task-list-items")->find("北京外围地形 <低精度>") !=
                     std::string::npos,
                 "取消修改没有返回任务列表") ||
        !click("task-edit-task-ui") || !click("export-task-name"))
    {
        return false;
    }
    typeText(L"北京外围地形 <修改后>");
    if (!WriteTaskScreenshot(runtime, testRoot / "task-edit-name.png") ||
        !click("export-edit-range") ||
        !Require(runtime.textContentById("open-export-page") == "导出",
                 "地图返回配置入口不应替代配置页的重新下载按钮") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-redownload-range.png") ||
        !click("open-export-page") ||
        !Require(runtime.textContentById("export-submit") == "重新下载",
                 "编辑范围返回配置时丢失了原任务修改状态") ||
        !click("export-config-radius"))
    {
        return false;
    }
    typeText(L"40000");
    if (!click("export-act-close-panel") || !click("export-submit"))
    {
        return false;
    }
    Json persisted;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do
    {
        std::ifstream input(recordsPath);
        persisted = Json::parse(input, nullptr, false);
        if (persisted.is_object() && persisted["tasks"].size() == 1 &&
            persisted["tasks"][0]["state"] == "failed" &&
            persisted["tasks"][0]["configuration"]["radiusMeters"] == 40000.)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!Require(persisted.is_object() && persisted["tasks"].size() == 1 &&
                     persisted["tasks"][0]["id"] == "task-ui" &&
                     persisted["tasks"][0]["title"] == "北京外围地形 <修改后>" &&
                     persisted["tasks"][0]["outputPath"] == task["outputPath"] &&
                     persisted["tasks"][0]["configuration"]["radiusMeters"] == 40000.,
                 "提交修改新增了任务或改变了输出目录"))
    {
        return false;
    }
    controller.resizeRuntime(runtime);
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    if (!verifyTaskTooltips("-narrow") ||
        !verifyTooltip("task-resume-task-ui", "task-resume-tip-task-ui", "继续任务（复用下载缓存）",
                       "task-resume-tooltip-narrow.png") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-actions-narrow.png") ||
        !click("tasks-back-preview"))
    {
        return false;
    }
    runtime.resize(1440, 900, 1.F);
    controller.resizeRuntime(runtime);
    if (!click("open-task-page"))
    {
        return false;
    }
    runtime.resize(700, 600, 1.F);
    controller.resizeRuntime(runtime);
    if (!click("task-rename-task-ui"))
    {
        return false;
    }
    typeText(L"取消的名称");
    WindowMessage(controller, adapter, window, runtime, WM_KEYDOWN, VK_ESCAPE, 0);
    if (!click("task-config-task-ui") ||
        !Require(runtime.textContentById("export-submit") == "加入导出队列" &&
                     runtime.textContentById("open-export-page") == "导出",
                 "复制配置没有保留新建任务的行为") ||
        !click("export-task-name"))
    {
        return false;
    }
    typeText(L"  北京外围地形 <副本>  ");
    if (!click("export-act-close-panel") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-create-name-narrow.png") ||
        !click("export-submit"))
    {
        return false;
    }
    const auto readRecords = [&] {
        std::ifstream input(recordsPath);
        return Json::parse(input, nullptr, false);
    };
    persisted = readRecords();
    if (!Require(persisted["tasks"].size() == 2 &&
                     persisted["tasks"][0]["title"] == "北京外围地形 <副本>" &&
                     persisted["tasks"][0]["id"] != "task-ui" &&
                     persisted["tasks"][1]["title"] == "北京外围地形 <修改后>",
                 "新建副本没有保存自定义名称或修改了原任务") ||
        !click("tasks-new-export") || !click("export-task-name"))
    {
        return false;
    }
    typeText(L"新建地形与建筑");
    if (!click("export-output"))
    {
        return false;
    }
    typeText(outputRoot.wstring());
    if (!click("export-config-radius"))
    {
        return false;
    }
    typeText(L"20000");
    if (!click("export-act-close-panel") ||
        !WriteTaskScreenshot(runtime, testRoot / "task-new-name-narrow.png") ||
        !click("export-submit"))
    {
        return false;
    }
    persisted = readRecords();
    if (!Require(persisted["tasks"].size() == 3 &&
                     persisted["tasks"][0]["title"] == "新建地形与建筑",
                 "正常创建导出任务没有保存输入的名称") ||
        !click("tasks-new-export") || !click("export-output"))
    {
        return false;
    }
    typeText(outputRoot.wstring());
    if (!click("export-config-radius"))
    {
        return false;
    }
    typeText(L"20000");
    if (!click("export-act-close-panel") || !click("export-submit"))
    {
        return false;
    }
    persisted = readRecords();
    if (!Require(persisted["tasks"].size() == 4 &&
                     persisted["tasks"][0]["title"].get<std::string>().starts_with("z") &&
                     persisted["tasks"][0]["title"].get<std::string>().find("20000 米") !=
                         std::string::npos,
                 "新建任务没有清除上次名称或留空时没有自动命名"))
    {
        return false;
    }
    return true;
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
    const bool passed =
        TestProviderMenuScrolling(window, testRoot) && TestUnrestrictedMapZoom() &&
        TestFractionalViewSpanInput(window, testRoot) &&
        RunTest(window, testRoot) && TestCopyUsesActiveRegionRole(window, testRoot) &&
        TestFlattenRegionEditing(window, testRoot) && TestShapefileComponent(window, testRoot) &&
        TestShapefileMapPreview(testRoot) && TestShapefileDrop(window, testRoot) &&
        TestTaskActions(window, testRoot) && TestPerTileFbxOption(window, testRoot) &&
        TestTaskFbxPreviewLoadsEveryTile(window, testRoot) &&
        TestElectronicMapSources(window, testRoot) && TestTencentGis(window, testRoot) &&
        TestBaiduGis(window, testRoot);
    DestroyWindow(window);
    CoUninitialize();
    if (!passed)
    {
        return 1;
    }
    std::cout << "PreviewRangeEditorTest passed\n";
    return 0;
}
