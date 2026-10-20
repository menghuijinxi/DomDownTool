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
    const bool passed = RunTest(window, testRoot) && TestTaskActions(window, testRoot);
    DestroyWindow(window);
    CoUninitialize();
    if (!passed)
    {
        return 1;
    }
    std::cout << "PreviewRangeEditorTest passed\n";
    return 0;
}
