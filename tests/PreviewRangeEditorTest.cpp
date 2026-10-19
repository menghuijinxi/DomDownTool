#include "PostProcessTestSupport.h"

#include <filesystem>
#include <iostream>
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
    const DownTool::DownloadRegions& GetRegions() const { return rangeEditor_.GetRegions(); }
    const DownTool::DownloadRegions& GetExportRegions() const { return exportDownloadRegions_; }
    const std::string& GetSelectedId() const { return rangeEditor_.GetSelectedId(); }
    const std::string& GetMessage() const { return rangeEditorMessage_; }
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
    renderer.setView(DownTool::GeographicBoundsCenter(*bounds), 15.);
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
    UiClick(controller, adapter, window, runtime, 210.F, 174.F);
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
    return Require(!controller.GetPanelOpen(), "150% 缩放下关闭范围编辑按钮没有响应");
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
