#include "PostProcessTestSupport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/core/mat.hpp"
#include "preview/PreviewInputRouting.h"
#include "preview/ExportDistanceLodUi.h"
#include "skui_runtime.h"

namespace
{
constexpr int kWidth = 1440;
constexpr int kHeight = 900;

void sendMouse(skui::Runtime& runtime, skui::EventType type, float x, float y)
{
    skui::Event event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.button = skui::MouseButton::Left;
    runtime.handleEvent(event);
}

bool setExportComposerFixture(skui::Runtime& runtime, bool sourceMenuOpen = false)
{
    const std::string groups =
        "<div id=\"export-group-list\" class=\"export-group-list\">"
        "<section id=\"export-group-satellite\" class=\"export-group-card is-only-group\">"
        "<header class=\"export-group-head\"><label class=\"export-group-check\">"
        "<span class=\"export-check-control\">"
        "<input id=\"export-act-toggle-group__g1\" type=\"checkbox\" checked>"
        "<span class=\"export-check-indicator is-checked\">&#10003;</span></span>"
        "<input id=\"export-act-rename-group__g1\" type=\"text\" value=\"卫星底图\" "
        "aria-label=\"内容组名称\"></label></header>"
        "<div id=\"export-act-select-component__c1\" class=\"export-component-row\" "
        "data-action=\"export-composer\">"
        "<span class=\"export-drag-handle\">&#10303;</span>"
        "<strong>大图</strong><span>高德卫星、百度卫星 · PNG · 16384</span>"
        "<button id=\"export-act-open-params__c1\" "
        "class=\"export-quiet-button export-component-action\" type=\"button\" "
        "data-action=\"export-composer\">参数</button>"
        "<button id=\"export-act-remove-component__c1\" "
        "class=\"export-quiet-button export-component-action\" type=\"button\" "
        "data-action=\"export-composer\">移除</button></div>"
        "<div id=\"export-act-select-component__c2\" class=\"export-component-row\" "
        "data-action=\"export-composer\">"
        "<span class=\"export-drag-handle\">&#10303;</span>"
        "<strong>原始瓦片</strong><span>高德卫星、百度卫星</span>"
        "<button id=\"export-act-open-params__c2\" "
        "class=\"export-quiet-button export-component-action\" type=\"button\" "
        "data-action=\"export-composer\">参数</button>"
        "<button id=\"export-act-remove-component__c2\" "
        "class=\"export-quiet-button export-component-action\" type=\"button\" "
        "data-action=\"export-composer\">移除</button></div>"
        "<div id=\"export-act-select-component__c3\" "
        "class=\"export-component-row is-selected\" data-action=\"export-composer\">"
        "<span class=\"export-drag-handle\">&#10303;</span>"
        "<strong>标准 XYZ</strong><span>高德卫星 · 512</span>"
        "<button id=\"export-act-open-params__c3\" "
        "class=\"export-quiet-button export-component-action is-active\" "
        "type=\"button\" data-action=\"export-composer\">参数</button>"
        "<button id=\"export-act-remove-component__c3\" "
        "class=\"export-quiet-button export-component-action\" type=\"button\" "
        "data-action=\"export-composer\">移除</button></div>"
        "<div class=\"export-add-row\"><button id=\"export-act-open-components__g1\" "
        "class=\"export-quiet-button export-add-component-button\" "
        "type=\"button\" data-action=\"export-composer\">添加组件</button></div></section>"
        "</div>";
    const std::string sourceMenu = sourceMenuOpen ?
        "<div id=\"export-source-menu\" class=\"export-menu\">"
        "<button type=\"button\" data-action=\"export-composer\">高德卫星</button>"
        "<button type=\"button\" data-action=\"export-composer\">高德电子</button>"
        "<button type=\"button\" data-action=\"export-composer\">高德路网</button>"
        "<button type=\"button\" data-action=\"export-composer\">百度卫星</button>"
        "<button type=\"button\" data-action=\"export-composer\">天地图影像</button>"
        "<button type=\"button\" data-action=\"export-composer\">天地图多时相</button>"
        "<button type=\"button\" data-action=\"export-composer\">Google 卫星</button>"
        "<button type=\"button\" data-action=\"export-composer\">Bing 卫星</button>"
        "<button type=\"button\" data-action=\"export-composer\">ArcGIS 影像</button>"
        "<button type=\"button\" data-action=\"export-composer\">道路</button>"
        "<button type=\"button\" data-action=\"export-composer\">建筑</button>"
        "<button type=\"button\" data-action=\"export-composer\">水</button>"
        "<button type=\"button\" data-action=\"export-composer\">点</button>"
        "<button type=\"button\" data-action=\"export-composer\">绿地</button>"
        "<button type=\"button\" data-action=\"export-composer\">边界</button>"
        "<button type=\"button\" data-action=\"export-composer\">高速</button>"
        "<button type=\"button\" data-action=\"export-composer\">地铁</button>"
        "<button type=\"button\" data-action=\"export-composer\">其他</button></div>" :
        "";
    std::string panel =
        "<aside id=\"export-param-panel\" class=\"export-param-panel\">"
        "<header class=\"export-panel-head\"><h2>标准 XYZ</h2>"
        "<button id=\"export-act-close-panel\" class=\"export-icon-button\" type=\"button\" "
        "data-action=\"export-composer\" aria-label=\"关闭参数\">×</button></header>"
        "<div id=\"export-panel-body\" class=\"export-panel-body\">"
        "<div class=\"export-panel-scroll-content\">"
        "<div class=\"export-panel-section\">"
        "<span class=\"export-panel-label\">图源</span>"
        "<div class=\"export-source-list\"><div class=\"export-source-row\">"
        "<span>高德卫星</span><button id=\"export-act-remove-source__c3__0\" "
        "class=\"export-icon-button\" "
        "type=\"button\" data-action=\"export-composer\" "
        "aria-label=\"移除图源\">×</button></div></div>"
        "<button id=\"export-act-open-sources__c3\" class=\"export-link-button\" type=\"button\" "
        "data-action=\"export-composer\">添加图源</button>";
    panel += sourceMenu;
    panel +=
        "</div>"
        "<div class=\"export-panel-section\"><span class=\"export-panel-label\">输出</span>"
        "<label class=\"export-field-row\"><span>分辨率</span>"
        "<select id=\"export-xyz-resolution\" class=\"export-select\">"
        "<option value=\"256\">256</option><option value=\"512\" selected>512</option>"
        "<option value=\"1024\">1024</option></select></label></div></div></div>"
        "<button id=\"export-act-done-panel\" class=\"primary-command export-panel-done\" "
        "type=\"button\" "
        "data-action=\"export-composer\">完成</button>"
        "</aside>";
    return runtime.replaceHtmlById("export-group-list", groups) &&
        runtime.replaceHtmlById("export-param-panel", panel) &&
        runtime.setVisibleById("export-footer", false);
}

bool setExportComponentMenuOverflowFixture(skui::Runtime& runtime)
{
    std::string rows;
    constexpr std::array<std::string_view, 5> kComponentNames = {
        "大图", "建筑模型", "GeoJSON", "大图", "底板模型"};
    for (size_t index = 0; index < kComponentNames.size(); ++index)
    {
        const std::string componentId = "menu-c" + std::to_string(index + 1);
        rows +=
            "<div id=\"export-act-select-component__" + componentId +
            "\" class=\"export-component-row\" data-action=\"export-composer\">"
            "<span class=\"export-drag-handle\">&#10303;</span><strong>" +
            std::string(kComponentNames[index]) +
            "</strong><span>测试组件</span>"
            "<button class=\"export-quiet-button export-component-action\" "
            "type=\"button\" data-action=\"export-composer\">参数</button>"
            "<button class=\"export-quiet-button export-component-action\" "
            "type=\"button\" data-action=\"export-composer\">移除</button></div>";
    }

    const std::string groups =
        "<div id=\"export-group-list\" class=\"export-group-list\">"
        "<section id=\"export-group-overflow\" class=\"export-group-card is-only-group\">"
        "<header class=\"export-group-head\"><label class=\"export-group-check\">"
        "<span class=\"export-check-control\">"
        "<input type=\"checkbox\" checked>"
        "<span class=\"export-check-indicator is-checked\">&#10003;</span></span>"
        "<input type=\"text\" value=\"当前预览\" aria-label=\"内容组名称\">"
        "</label></header>" +
        rows +
        "<div class=\"export-add-row\">"
        "<button class=\"export-quiet-button export-add-component-button\" "
        "type=\"button\" data-action=\"export-composer\">添加组件</button>"
        "<div id=\"export-component-menu\" class=\"export-menu\">"
        "<button id=\"export-act-add-component__g1__merged-image\" type=\"button\" "
        "data-action=\"export-composer\">大图</button>"
        "<button id=\"export-act-add-component__g1__standard-xyz\" type=\"button\" "
        "data-action=\"export-composer\">标准 XYZ</button>"
        "<button id=\"export-act-add-component__g1__raw-tiles\" type=\"button\" "
        "data-action=\"export-composer\">原始瓦片</button>"
        "<button id=\"export-act-add-component__g1__base-plate\" type=\"button\" "
        "data-action=\"export-composer\">底板模型</button>"
        "<button id=\"export-act-add-component__g1__building-model\" type=\"button\" "
        "data-action=\"export-composer\">建筑模型</button>"
        "<button id=\"export-act-add-component__g1__geojson\" type=\"button\" "
        "data-action=\"export-composer\">GeoJSON</button></div></div></section></div>";
    return runtime.replaceHtmlById("export-group-list", groups);
}

std::string exportBasePlatePanelScrollContentFixture(bool withDistanceLod = false)
{
    DomDownToolPreview::ExportComponent component;
    component.id = "c-base";
    component.exportElevation = true;
    component.SetDistanceLod(DownTool::DefaultFbxDistanceLod());
    const std::string lodHtml = withDistanceLod ?
        DomDownToolPreview::ExportDistanceLodHtml(component) : "";
    return
        "<div id=\"export-panel-scroll-content\" class=\"export-panel-scroll-content\">"
        "<div class=\"export-panel-section\"><span class=\"export-panel-label\">图源</span>"
        "<button class=\"export-link-button\" type=\"button\" "
        "data-action=\"export-composer\">添加图源</button></div>"
        "<div class=\"export-panel-section\"><span class=\"export-panel-label\">贴图</span>"
        "<label class=\"export-field-row\"><span>贴图尺寸</span>"
        "<select class=\"export-select\"><option>4096</option></select></label>"
        "<label class=\"export-field-row\"><span>贴图格式</span>"
        "<select class=\"export-select\"><option>PNG</option></select></label></div>"
        "<div class=\"export-panel-section\"><span class=\"export-panel-label\">形态</span>"
        "<div class=\"export-segment\">"
        "<button id=\"export-act-set-shape__c-base__plane\" "
        "class=\"export-segment-button\" type=\"button\" "
        "data-action=\"export-composer\">平面</button>"
        "<button id=\"export-act-set-shape__c-base__sphere\" "
        "class=\"export-segment-button is-active\" type=\"button\" "
        "data-action=\"export-composer\">球面</button></div></div>"
        "<label class=\"export-switch-row\"><span>导出高度</span>"
        "<input id=\"export-act-toggle-elevation__c-base\" "
        "class=\"export-switch\" type=\"checkbox\"></label>"
        "<label class=\"export-field-row is-disabled\"><span>采样率（米）</span>"
        "<input id=\"export-act-set-sample__c-base\" type=\"number\" "
        "value=\"30\" disabled></label>"
        "<label class=\"export-field-row is-disabled\"><span class=\"export-terrain-budget-label\">"
        "<span>每块三角面上限</span><span>（0不限）</span></span>"
        "<input id=\"export-act-set-terrain-budget__c-base\" type=\"number\" "
        "value=\"8192\" disabled></label>"
        "<label class=\"export-field-row is-disabled\"><span>网格细分</span>"
        "<input id=\"export-act-set-subdivision__c-base\" type=\"number\" "
        "value=\"32\" disabled></label>" + lodHtml +
        "<p class=\"export-hint\">零面取范围内最低高程。</p></div>";
}

bool setExportBasePlatePanelFixture(skui::Runtime& runtime, bool withDistanceLod = false)
{
    const std::string panel =
        "<aside id=\"export-param-panel\" class=\"export-param-panel\">"
        "<header class=\"export-panel-head\"><h2>底板模型</h2>"
        "<button id=\"export-act-close-panel\" class=\"export-icon-button\" type=\"button\" "
        "data-action=\"export-composer\" aria-label=\"关闭参数\">×</button></header>"
        "<div id=\"export-panel-body\" class=\"export-panel-body\">" +
        exportBasePlatePanelScrollContentFixture(withDistanceLod) +
        "</div>"
        "<button id=\"export-act-done-panel\" class=\"primary-command export-panel-done\" "
        "type=\"button\" data-action=\"export-composer\">完成</button></aside>";
    return runtime.replaceHtmlById("export-param-panel", panel);
}

bool setExportBuildingPanelFixture(skui::Runtime& runtime)
{
    const std::string panel =
        "<aside id=\"export-param-panel\" class=\"export-param-panel\">"
        "<header class=\"export-panel-head\"><h2>建筑模型</h2>"
        "<button id=\"export-act-close-panel\" class=\"export-icon-button\" type=\"button\" "
        "data-action=\"export-composer\" aria-label=\"关闭参数\">×</button></header>"
        "<div id=\"export-panel-body\" class=\"export-panel-body\">"
        "<div id=\"export-panel-scroll-content\" class=\"export-panel-scroll-content\">"
        "<div class=\"export-panel-section\"><span class=\"export-panel-label\">图源</span>"
        "<div class=\"export-source-list\"><div class=\"export-source-row is-fixed\">"
        "<span class=\"export-source-name\">建筑</span></div></div></div>"
        "<div class=\"export-field-row export-building-mode-row\"><span>形态</span>"
        "<div class=\"export-segment\">"
        "<button id=\"export-act-set-shape__c-building__plane\" "
        "class=\"export-segment-button\" type=\"button\" "
        "data-action=\"export-composer\">平面</button>"
        "<button id=\"export-act-set-shape__c-building__sphere\" "
        "class=\"export-segment-button is-active\" type=\"button\" "
        "data-action=\"export-composer\">球面</button></div></div>"
        "<label class=\"export-field-row\"><span>分块边长 (米)</span>"
        "<input id=\"export-act-set-building-chunk-size__c-building\" type=\"number\" "
        "min=\"1\" step=\"1\" value=\"2200\"></label>"
        "<div class=\"export-field-row export-building-mode-row\"><span>墙体模式</span>"
        "<div class=\"export-segment\">"
        "<button id=\"export-act-set-wall-mode__c-building__scattered\" "
        "class=\"export-segment-button\" type=\"button\" "
        "data-action=\"export-composer\">散图</button>"
        "<button id=\"export-act-set-wall-mode__c-building__merged-atlas\" "
        "class=\"export-segment-button is-active\" type=\"button\" "
        "data-action=\"export-composer\">合并贴图</button></div></div>"
        "<div class=\"export-field-row export-building-mode-row\"><span>屋顶模式</span>"
        "<div class=\"export-segment\">"
        "<button id=\"export-act-set-roof-mode__c-building__scattered\" "
        "class=\"export-segment-button\" type=\"button\" "
        "data-action=\"export-composer\">散图</button>"
        "<button id=\"export-act-set-roof-mode__c-building__merged-atlas\" "
        "class=\"export-segment-button is-active\" type=\"button\" "
        "data-action=\"export-composer\">合并贴图</button>"
        "<button id=\"export-act-set-roof-mode__c-building__satellite\" "
        "class=\"export-segment-button\" type=\"button\" "
        "data-action=\"export-composer\">卫星图</button></div></div>"
        "<label class=\"export-field-row\"><span>合并格式</span>"
        "<select class=\"export-select\"><option>PNG</option>"
        "<option selected>JPG</option></select></label>"
        "<label class=\"export-field-row\"><span>楼层高度</span>"
        "<input type=\"number\" value=\"45\"></label>"
        "<label class=\"export-field-row\"><span>墙体材质上限</span>"
        "<input type=\"number\" value=\"1\"></label>"
        "<label class=\"export-field-row\"><span>随机种子</span>"
        "<input type=\"number\" value=\"0\"></label>"
        "<label class=\"export-switch-row\"><span>跟随底板高程</span>"
        "<input class=\"export-switch\" type=\"checkbox\"></label>"
        "<div class=\"export-field-row export-directory-row\">"
        "<span>墙体贴图目录</span><div class=\"export-directory-control\">"
        "<input id=\"export-act-set-wall-path__c-building\" type=\"text\" "
        "value=\"贴图/墙体\">"
        "<button id=\"export-act-browse-wall-path__c-building\" "
        "class=\"export-directory-button\" type=\"button\" "
        "data-action=\"export-composer\" title=\"选择墙体贴图目录\" "
        "aria-label=\"选择墙体贴图目录\"><svg viewBox=\"0 0 24 24\" "
        "aria-hidden=\"true\"><path fill=\"none\" stroke=\"#c9cfcc\" "
        "stroke-width=\"1.6\" stroke-linecap=\"round\" "
        "stroke-linejoin=\"round\" d=\"M3 7a2 2 0 0 1 2-2h4l2 2.5h8"
        "a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\">"
        "</path></svg></button></div></div>"
        "<div class=\"export-field-row export-directory-row\">"
        "<span>屋顶贴图目录</span><div class=\"export-directory-control\">"
        "<input id=\"export-act-set-roof-path__c-building\" type=\"text\" "
        "value=\"贴图/房顶\">"
        "<button id=\"export-act-browse-roof-path__c-building\" "
        "class=\"export-directory-button\" type=\"button\" "
        "data-action=\"export-composer\" title=\"选择屋顶贴图目录\" "
        "aria-label=\"选择屋顶贴图目录\"><svg viewBox=\"0 0 24 24\" "
        "aria-hidden=\"true\"><path fill=\"none\" stroke=\"#c9cfcc\" "
        "stroke-width=\"1.6\" stroke-linecap=\"round\" "
        "stroke-linejoin=\"round\" d=\"M3 7a2 2 0 0 1 2-2h4l2 2.5h8"
        "a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\">"
        "</path></svg></button></div></div></div></div>"
        "<button id=\"export-act-done-panel\" class=\"primary-command export-panel-done\" "
        "type=\"button\" data-action=\"export-composer\">完成</button></aside>";
    return runtime.replaceHtmlById("export-param-panel", panel);
}

bool writeRuntimeScreenshot(skui::Runtime& runtime,
                            const std::filesystem::path& outputPath,
                            int width,
                            int height)
{
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    if (!postprocess_test::Require(
            runtime.renderToBgraPixels(pixels.data(), width, height,
                                       static_cast<size_t>(width) * sizeof(uint32_t), 1.F),
            "UI 离屏渲染失败：" + postprocess_test::PathToUtf8String(outputPath)))
    {
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(outputPath.parent_path(), directoryError);
    if (!postprocess_test::Require(
            !directoryError,
            "无法创建 UI 截图目录：" + postprocess_test::PathToUtf8String(outputPath)))
    {
        return false;
    }

    cv::Mat image(height, width, CV_8UC4, pixels.data());
    return postprocess_test::Require(
        cv::imwrite(outputPath.string(), image),
        "无法写入 UI 截图：" + postprocess_test::PathToUtf8String(outputPath));
}

bool runTest()
{
    const std::filesystem::path sourceRoot = DOMDOWNTOOL_SOURCE_DIR;
    const std::filesystem::path documentPath =
        sourceRoot / "resources" / "preview" / "index.html";
    const std::filesystem::path controllerPath =
        sourceRoot / "src" / "preview" / "PreviewController.cpp";
    const std::filesystem::path composerPath =
        sourceRoot / "src" / "preview" / "ExportComposer.cpp";
    const std::filesystem::path groupedExportPath =
        sourceRoot / "src" / "preview" / "ExportGroupedExport.cpp";
    const std::filesystem::path exportTaskManagerHeaderPath =
        sourceRoot / "include" / "preview" / "ExportTaskManager.h";
    const std::filesystem::path mapRendererPath =
        sourceRoot / "src" / "preview" / "MapRenderer.cpp";
    skui::RuntimeOptions options;
    options.assetRoot = documentPath.parent_path().generic_string();
    skui::Runtime runtime(std::move(options));
    runtime.resize(kWidth, kHeight, 1.F);
    if (!postprocess_test::Require(runtime.loadDocument(documentPath.generic_string()),
                                   "无法加载预览 HTML：" + runtime.lastError()))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(
                documentPath, "fill=\"none\" stroke=\"#c9cfcc\" stroke-width=\"1.6\""),
            "输出目录按钮的 SVG 没有显式设置透明填充和浅色描边"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(composerPath, "\"tianditu.multidate\"") &&
                postprocess_test::ContainsText(composerPath, "\"天地图多时相\"") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "{{\"256\", \"256\"}, {\"512\", \"512\"}, {\"1024\", \"1024\"}}") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"export-group-list\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"export-param-panel\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tianditu-multidate-panel\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tianditu-multidate-login\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tianditu-multidate-refresh\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tianditu-multidate-year\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "style=\"display: none\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "历史时相需要登录后刷新") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tile-debug-toggle\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tile-debug-panel\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"tile-debug-content\""),
            "预览页缺少天地图多时相入口、登录刷新控件或瓦片诊断控件"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(
                mapRendererPath,
                "std::move(multidateProjection), TileYAxisDirection::Down, 1., 18.)") &&
                !postprocess_test::ContainsText(
                    mapRendererPath,
                    "std::move(multidateProjection), TileYAxisDirection::Down, 17., 18.)"),
            "天地图多时相预览不应把卫星缩放层级限制到 17 以上"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(
                controllerPath, "if (selectHtml != lastTianDiTuMultidateSelectHtml_)") &&
                postprocess_test::ContainsText(
                    controllerPath,
                    "lastTianDiTuMultidateSelectedValue_.clear();") &&
                postprocess_test::ContainsText(
                    controllerPath, "RefreshTianDiTuMultidateAfterViewChange") &&
                postprocess_test::ContainsText(
                    controllerPath, "export-provider-tianditu-multidate") &&
                postprocess_test::ContainsText(
                    controllerPath, "tianDiTuMultidateExportCacheRoot") &&
                postprocess_test::ContainsText(
                    controllerPath, "buildExportTianDiTuMultidateYearsHtml") &&
                postprocess_test::ContainsText(
                    controllerPath, "isExportTianDiTuMultidateYearControl") &&
                postprocess_test::ContainsText(
                    controllerPath, "exportTianDiTuMultidateSelectedValues_") &&
                postprocess_test::ContainsText(
                    controllerPath, "selectedExportTianDiTuMultidateValuesInDisplayOrder") &&
                postprocess_test::ContainsText(
                    controllerPath, "EnsureExportTianDiTuMultidateYears") &&
                postprocess_test::ContainsText(
                    controllerPath, "GetSelectedExportTianDiTuMultidateYear") &&
                postprocess_test::ContainsText(
                    controllerPath, "LoginTianDiTuMultidate(runtime, preferredYear)") &&
                postprocess_test::ContainsText(
                    controllerPath, "class=\\\"check-grid export-year-grid\\\"") &&
                postprocess_test::ContainsText(
                    controllerPath, "tianDiTuMultidateYearPickerActive_") &&
                postprocess_test::ContainsText(
                    controllerPath, "!tianDiTuMultidateYearPickerActive_") &&
                postprocess_test::ContainsText(controllerPath, "multidateLayerStatusText") &&
                postprocess_test::ContainsText(controllerPath, "历史叠加") &&
                postprocess_test::ContainsText(controllerPath, "BuildTileDebugHtml") &&
                postprocess_test::ContainsText(controllerPath, "TileDebugAt") &&
                postprocess_test::ContainsText(mapRendererPath,
                                               "multidateHistoryEmptyTiles") &&
                postprocess_test::ContainsText(mapRendererPath, "服务端空白占位") &&
                postprocess_test::ContainsText(mapRendererPath, "请求失败") &&
                postprocess_test::ContainsText(
                    mapRendererPath, "HasExhaustedPersistentFailures") &&
                postprocess_test::ContainsText(
                    mapRendererPath, "ClearPersistentFailureStatesUnder") &&
                postprocess_test::ContainsText(
                    mapRendererPath, "forceRetryKnownTileFailures_") &&
                postprocess_test::ContainsText(mapRendererPath, "最新底图") &&
                postprocess_test::ContainsText(mapRendererPath, "drawTileDebugOverlay") &&
                postprocess_test::ContainsText(mapRendererPath, "drawString") &&
                postprocess_test::ContainsText(
                    mapRendererPath, "SkFont labelFont(tileDebugTypeface(), 16.F)") &&
                postprocess_test::ContainsText(
                    mapRendererPath, "SkFont::Edging::kSubpixelAntiAlias") &&
                postprocess_test::ContainsText(mapRendererPath, " empty"),
            "天地图多时相年份下拉框、历史叠加状态和瓦片诊断网格不应丢失"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(documentPath, ".param-row") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: repeat(3, minmax(0, 1fr))") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "flex-wrap: wrap") &&
                postprocess_test::ContainsText(
                    documentPath, ".export-sub-options") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-year-grid .check-item") &&
                !postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: repeat(auto-fit, minmax(250px, 1fr))") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: minmax(0, 1fr)") &&
                postprocess_test::ContainsText(documentPath, ".param-field select") &&
                postprocess_test::ContainsText(documentPath, "id=\"export-submit\"") &&
                postprocess_test::ContainsText(documentPath, "min-width: 0"),
            "导出参数行没有提供窗口变窄时的换行和收缩布局"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            runtime.setVisibleById("preview-page", true) &&
                runtime.setVisibleById("export-page", false) &&
                runtime.setVisibleById("task-page", false) &&
                runtime.setVisibleById("fbx-page", false) &&
                runtime.setVisibleById("task-clear-confirm", false) &&
                runtime.setVisibleById("task-delete-confirm", false),
            "预览、导出配置或导出任务页面结构缺失") ||
        !postprocess_test::Require(
            runtime.setValueById("export-radius", "2000") &&
                runtime.setValueById("export-config-radius", "2000"),
            "预览页或导出配置页缺少半径输入"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            runtime.setVisibleById("tile-debug-panel", true) &&
                runtime.setAttributeById("tile-debug-toggle", "aria-pressed", "true") &&
                runtime.replaceHtmlById(
                    "tile-debug-content",
                    "<div id=\"tile-debug-content\"><div class=\"tile-debug-title\">"
                    "瓦片诊断</div><div class=\"tile-debug-layer\">"
                    "<div class=\"tile-debug-layer-head\"><strong>历史叠加</strong>"
                    "<span>x=107941 y=18261 z=17</span><em>已显示</em></div>"
                    "<div class=\"tile-debug-path\">缓存：cache/天地图多时相/2024"
                    "</div><div class=\"tile-debug-url\">请求：https://image.tianditu.gov.cn"
                    "</div></div></div>"),
            "瓦片诊断面板缺少可更新的显示区域或开关状态"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(documentPath, "id=\"fbx-page\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-preview-back\"") &&
                postprocess_test::ContainsText(
                    documentPath, "id=\"fbx-preview-mode-textured\" class=\"fbx-preview-mode-button\"") &&
                postprocess_test::ContainsText(
                    documentPath, "id=\"fbx-preview-mode-wireframe\" class=\"fbx-preview-mode-button\"") &&
                postprocess_test::ContainsText(
                    documentPath, "id=\"fbx-preview-mode-both\" class=\"fbx-preview-mode-button\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-reset-view\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-preview-fps\"") &&
                postprocess_test::ContainsText(documentPath, "FPS --") &&
                postprocess_test::ContainsText(documentPath, "data-action=\"reset-fbx-view\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-rotate-speed\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-zoom-speed\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-pan-speed\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"fbx-move-speed\"") &&
                postprocess_test::ContainsText(documentPath, "aria-label=\"预览显示模式\"") &&
                postprocess_test::ContainsText(
                    documentPath, "data-action=\"set-fbx-preview-mode\"") &&
                runtime.setValueById("fbx-rotate-speed", "1.0") &&
                runtime.setValueById("fbx-zoom-speed", "1.0") &&
                runtime.setValueById("fbx-pan-speed", "1.0") &&
                runtime.setValueById("fbx-move-speed", "1.0") &&
                runtime.setTextsById({
                    {"fbx-preview-title", "测试 FBX"},
                    {"fbx-preview-directory", "tmp/export"},
                    {"fbx-preview-stats",
                     "4 个顶点 · 当前面数 2 · 总面数 2 · 当前 DrawCall 1 · 总 DrawCall 1"},
                    {"fbx-preview-fps", "FPS 60"},
                    {"fbx-rotate-speed-value", "1.0x"},
                    {"fbx-zoom-speed-value", "1.0x"},
                    {"fbx-pan-speed-value", "1.0x"},
                    {"fbx-move-speed-value", "1.0x"}}),
            "FBX 网格预览页缺少页面结构或动态状态字段"))
    {
        return false;
    }

    const std::vector<std::string> expectedTileStatChildren = {
        "tile-stat-visible-item", "tile-stat-ready-item", "tile-stat-pending-item",
        "tile-stat-downloads-item", "tile-stat-failed-item", "tile-stat-cache-item"};
    if (!postprocess_test::Require(
            runtime.childElementIdsById("tile-stats") == expectedTileStatChildren,
            "状态栏统计分栏结构缺失") ||
        !postprocess_test::Require(
            runtime.setTextsById(
                {{"tile-stat-visible", "96"}, {"tile-stat-ready", "93"},
                 {"tile-stat-pending", "3"}, {"tile-stat-downloads", "1/2"},
                 {"tile-stat-failed", "0"}, {"tile-stat-cache", "52.0 MB"}}),
            "无法更新状态栏统计数值") ||
        !postprocess_test::Require(
            runtime.childElementIdsById("tile-stats") == expectedTileStatChildren,
            "动态更新统计数值后破坏了状态栏分栏结构"))
    {
        return false;
    }

    const std::vector<std::string> statusbarChildren =
        runtime.childElementIdsById("preview-statusbar");
    if (!postprocess_test::Require(
            std::ranges::find(statusbarChildren, "cursor-coordinate") ==
                    statusbarChildren.end(),
            "状态栏仍包含无用的鼠标位置经纬度") ||
        !postprocess_test::Require(
            std::ranges::find(statusbarChildren, "center-coordinate") !=
                    statusbarChildren.end(),
            "状态栏缺少可编辑的中心经纬度"))
    {
        return false;
    }

    const std::filesystem::path dx12FbxRendererPath =
        sourceRoot / "src" / "preview" / "Dx12FbxRenderer.cpp";
    const std::filesystem::path cmakePath = sourceRoot / "CMakeLists.txt";
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(controllerPath, "disableFbxInputMethod") &&
                postprocess_test::ContainsText(controllerPath, "restoreFbxInputMethod") &&
                postprocess_test::ContainsText(controllerPath, "ImmAssociateContext") &&
                postprocess_test::ContainsText(cmakePath, "imm32"),
            "FBX 预览没有在页面生命周期内禁用并恢复输入法"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(controllerPath,
                                           "scheduleFbxKeyboardPanFrame(window)") &&
                postprocess_test::ContainsText(controllerPath, "isFbxKeyboardPanKey") &&
                postprocess_test::ContainsText(controllerPath, "nextWDown") &&
                postprocess_test::ContainsText(controllerPath, "nextADown") &&
                postprocess_test::ContainsText(controllerPath, "nextSDown") &&
                postprocess_test::ContainsText(controllerPath, "nextDDown") &&
                postprocess_test::ContainsText(controllerPath,
                                               "dx12FbxRenderer_.keyboardPanBy") &&
                postprocess_test::ContainsText(controllerPath, "fbx-move-speed") &&
                postprocess_test::ContainsText(controllerPath, "moveSpeed") &&
                postprocess_test::ContainsText(
                    controllerPath, "kPreviewFbxKeyboardPanFrameIntervalMilliseconds") &&
                postprocess_test::ContainsText(controllerPath, "startFbxPreviewFrameLoop") &&
                postprocess_test::ContainsText(controllerPath, "kPreviewFbxFrameTimerId") &&
                postprocess_test::ContainsText(controllerPath,
                                               "buildFbxPreviewStatsText") &&
                postprocess_test::ContainsText(controllerPath,
                                               "updateFbxPreviewFrameStatsUi(runtime)") &&
                postprocess_test::ContainsText(controllerPath, "nextQDown") &&
                postprocess_test::ContainsText(controllerPath, "nextEDown") &&
                postprocess_test::ContainsText(dx12FbxRendererPath,
                                               "forward.y * forwardUnits") &&
                !postprocess_test::ContainsText(controllerPath,
                                                "kPreviewFbxVerticalPanTimerId"),
            "FBX WASD/QE 连续平移没有使用独立帧调度器"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(documentPath, "id=\"gis-overlay-arrow-icon\"") &&
                postprocess_test::ContainsText(documentPath,
                                               "id=\"gis-overlay-arrow-path\"") &&
                !postprocess_test::ContainsText(
                    controllerPath, "setTextById(\"gis-overlay-arrow\""),
            "GIS 下拉箭头没有保持单一静态 SVG"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(controllerPath, "task-config-") &&
                postprocess_test::ContainsText(controllerPath, "open-export-task-config") &&
                postprocess_test::ContainsText(controllerPath, "task-resume-") &&
                postprocess_test::ContainsText(controllerPath, "resume-export-task") &&
                postprocess_test::ContainsText(controllerPath, "taskDeclaresFbxOutput") &&
                postprocess_test::ContainsText(controllerPath, "record.detail.find(\"FBX\")") &&
                postprocess_test::ContainsText(documentPath, "task-reopen-config") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr) 125px 240px;") &&
                postprocess_test::ContainsText(controllerPath, "task-edit-") &&
                postprocess_test::ContainsText(controllerPath, "task-rename-") &&
                postprocess_test::ContainsText(documentPath, "task-rename-input"),
            "导出任务缺少按当前参数重新打开配置的按钮结构"))
    {
        return false;
    }

    const std::vector<std::string> exportHeaderActionChildren =
        runtime.childElementIdsById("export-header-actions");
    const std::vector<std::string> taskHeaderChildren =
        runtime.childElementIdsById("task-page-header");
    const std::vector<std::string> taskClearConfirmChildren =
        runtime.childElementIdsById("task-clear-confirm");
    const std::vector<std::string> taskClearDialogChildren =
        runtime.childElementIdsById("task-clear-dialog");
    const std::vector<std::string> taskDeleteConfirmChildren =
        runtime.childElementIdsById("task-delete-confirm");
    const std::vector<std::string> taskDeleteDialogChildren =
        runtime.childElementIdsById("task-delete-dialog");
    const std::vector<std::string> taskDeleteTargetChildren =
        runtime.childElementIdsById("task-delete-target");
    const auto exportTasksButton =
        std::ranges::find(exportHeaderActionChildren, "export-open-tasks");
    const auto exportBackButton =
        std::ranges::find(exportHeaderActionChildren, "export-back-preview");
    const auto taskClearButton =
        std::ranges::find(taskHeaderChildren, "tasks-clear-finished");
    const auto taskNewButton =
        std::ranges::find(taskHeaderChildren, "tasks-new-export");
    const auto taskBackButton =
        std::ranges::find(taskHeaderChildren, "tasks-back-preview");
    if (!postprocess_test::Require(
            exportTasksButton != exportHeaderActionChildren.end() &&
                exportBackButton == std::next(exportTasksButton),
            "导出配置页的返回按钮没有紧跟在导出任务按钮后") ||
        !postprocess_test::Require(
            taskClearButton != taskHeaderChildren.end() &&
                taskNewButton == std::next(taskClearButton) &&
                taskNewButton != taskHeaderChildren.end() &&
                taskBackButton == std::next(taskNewButton),
            "导出任务页的返回按钮没有紧跟在新建导出按钮后") ||
        !postprocess_test::Require(
            taskClearConfirmChildren == std::vector<std::string>{"task-clear-dialog"} &&
                taskClearDialogChildren ==
                    std::vector<std::string>{"task-clear-title", "task-clear-message",
                                             "task-clear-actions"} &&
                runtime.setVisibleById("task-clear-confirm", true) &&
                runtime.setVisibleById("task-clear-confirm", false),
            "移除已结束任务缺少可取消的确认界面") ||
        !postprocess_test::Require(
            taskDeleteConfirmChildren ==
                    std::vector<std::string>{"task-delete-dialog"} &&
                taskDeleteDialogChildren ==
                    std::vector<std::string>{"task-delete-title", "task-delete-message",
                                             "task-delete-target", "task-delete-actions"} &&
                taskDeleteTargetChildren ==
                    std::vector<std::string>{"task-delete-title-value",
                                             "task-delete-path-value"} &&
                runtime.setVisibleById("task-delete-confirm", true) &&
                runtime.setVisibleById("task-delete-confirm", false),
            "永久删除导出文件缺少独立确认界面"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            postprocess_test::ContainsText(documentPath, "id=\"export-group-list\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"export-param-panel\"") &&
                postprocess_test::ContainsText(documentPath, "class=\"export-workspace\"") &&
                postprocess_test::ContainsText(documentPath, "id=\"export-submit\"") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: repeat(2, minmax(0, 1fr));") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-panel-scroll-content") &&
                postprocess_test::ContainsText(documentPath,
                                               "margin-right: 24px;") &&
                postprocess_test::ContainsText(documentPath,
                                               "flex: 0 1 180px;") &&
                postprocess_test::ContainsText(documentPath,
                                               "flex: 1 1 0;") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-field-row.is-disabled") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-field-row input:disabled") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-source-name") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-building-mode-row") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-directory-row") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-directory-control") &&
                postprocess_test::ContainsText(documentPath,
                                               ".export-directory-button") &&
                postprocess_test::ContainsText(
                    documentPath,
                    ".export-building-mode-row > .export-segment") &&
                postprocess_test::ContainsText(documentPath,
                                               "min-width: 240px;") &&
                postprocess_test::ContainsText(composerPath, "ResetExportComposer") &&
                postprocess_test::ContainsText(
                    composerPath, "export-act-set-building-chunk-size__") &&
                postprocess_test::ContainsText(
                    composerPath, "component->SetBuildingChunkSizeMeters(*size);") &&
                postprocess_test::ContainsText(composerPath, "RenderExportComposer") &&
                postprocess_test::ContainsText(composerPath, "dom-geotiff") &&
                postprocess_test::ContainsText(composerPath, "dem-geotiff") &&
                postprocess_test::ContainsText(composerPath,
                    "export-act-toggle-cesium-mipmaps__") &&
                postprocess_test::ContainsText(composerPath,
                    "component->SetCesiumTextureMipmaps(checked);") &&
                postprocess_test::ContainsText(composerPath, "EPSG:3857") &&
                postprocess_test::ContainsText(composerPath, "原始瓦片分辨率") &&
                postprocess_test::ContainsText(composerPath,
                                               "export-panel-scroll-content") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "class=\\\"export-segment-button{}\\\" type=\\\"button\\\" ") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "const bool subdivisionEnabled = !component.exportElevation &&") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "component.shape != DownTool::FbxPlanetShape::Plane;") &&
                !postprocess_test::ContainsText(
                    composerPath,
                    "SegmentButton(\"export-act-set-shape__\" + component.id + "
                    "\"__both\"") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "class=\\\"export-source-row is-fixed\\\"") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "class=\\\"export-source-name\\\"") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "export-field-row export-building-mode-row") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "export-act-browse-wall-path__") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "export-act-browse-roof-path__") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "SelectExportDirectory()") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "id=\\\"export-panel-scroll-content\\\"") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "selected->id == lastExportParamPanelComponentId_;") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "replaceHtmlById(") &&
                postprocess_test::ContainsText(
                    composerPath,
                    "\"export-panel-scroll-content\", panelScrollContentHtml") &&
                postprocess_test::ContainsText(
                    exportTaskManagerHeaderPath,
                    "shape = DownTool::FbxPlanetShape::Sphere;") &&
                postprocess_test::ContainsText(
                    groupedExportPath,
                    "JsonString(json, \"shape\", \"sphere\")") &&
                postprocess_test::ContainsText(
                    groupedExportPath,
                    "step.configuration.executionFbxShape = component.shape;") &&
                !postprocess_test::ContainsText(composerPath,
                                                "export-act-add-group") &&
                !postprocess_test::ContainsText(composerPath,
                                                "verb == \"add-group\"") &&
                !postprocess_test::ContainsText(composerPath,
                                                "添加内容组") &&
                postprocess_test::ContainsText(composerPath,
                                               "export-act-select-component__") &&
                postprocess_test::ContainsText(composerPath,
                                               "verb == \"select-component\"") &&
                postprocess_test::ContainsText(controllerPath, "HandleExportComposerEvent"),
            "export composer is missing its group list or parameter panel"))
    {
        return false;
    }
    const std::filesystem::path exportManagerPath =
        sourceRoot / "src" / "preview" / "ExportTaskManager.cpp";
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(exportManagerPath, "RunBuildingFbxExport") &&
                postprocess_test::ContainsText(exportManagerPath, "BuildingFbxModeName") &&
                postprocess_test::ContainsText(exportManagerPath,
                                               "ExportAmapBuildingsToFbx") &&
                postprocess_test::ContainsText(
                    exportManagerPath,
                    "exportOptions.shape = configuration.executionFbxShape.value_or(") &&
                postprocess_test::ContainsText(groupedExportPath,
                                               "ExportGroupedRunner::Run") &&
                postprocess_test::ContainsText(groupedExportPath,
                                               "GroupedExportError"),
            "建筑 FBX 没有独立导出调度入口"))
    {
        return false;
    }
    const std::filesystem::path dx12RendererPath =
        sourceRoot / "src" / "preview" / "Dx12FbxRenderer.cpp";
    const std::filesystem::path skiaFbxRendererPath =
        sourceRoot / "src" / "preview" / "FbxPreviewRenderer.cpp";
    if (!postprocess_test::Require(
                postprocess_test::ContainsText(dx12RendererPath,
                                               "D3D12_TEXTURE_ADDRESS_MODE_WRAP") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "textureWorkerCountFor") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "kReservedTextureWorkerCoreCount") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "kMaximumTextureWorkerCount = 32") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "kMaximumTextureUploadsPerFrame") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "kMaximumTextureUploadBytesPerFrame") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "std::atomic_size_t nextTextureIndex") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "std::vector<std::jthread> textureWorkers") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "kRGBA_8888_SkColorType") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "kAtlasTextureTransformFlag") &&
                postprocess_test::ContainsText(dx12RendererPath,
                                               "lerp(input.atlasOffset, input.atlasMax") &&
                postprocess_test::ContainsText(skiaFbxRendererPath,
                                               "SkTileMode::kRepeat") &&
                !postprocess_test::ContainsText(dx12RendererPath,
                                                "kMaximumVertices") &&
                !postprocess_test::ContainsText(dx12RendererPath,
                                                "kMaximumTriangles") &&
                !postprocess_test::ContainsText(skiaFbxRendererPath,
                                                "kMaximumPreviewVertices") &&
                !postprocess_test::ContainsText(skiaFbxRendererPath,
                                                "kMaximumPreviewTriangles"),
            "FBX 预览不应截断顶点/三角形，墙体贴图 U 方向应重复采样"))
    {
        return false;
    }

    std::string lastInputId;
    std::string lastInputValue;
    std::string lastMouseDownId;
    std::string lastMouseUpId;
    std::string lastClickId;
    std::string lastChangeId;
    std::string lastChangeValue;
    bool lastChangeChecked = false;
    bool gisPanelOpen = false;
    std::string activePage = "preview";
    bool navigationSucceeded = false;
    bool rerenderBasePlateOnShapeClick = false;
    bool basePlateRerenderSucceeded = false;
    runtime.setElementEventCallback(
        [&](const skui::ElementEvent& event)
        {
            if (event.type == skui::ElementEventType::Input)
            {
                lastInputId = event.id;
                lastInputValue = event.value;
            }
            else if (event.type == skui::ElementEventType::MouseDown)
            {
                lastMouseDownId = event.id;
            }
            else if (event.type == skui::ElementEventType::MouseUp)
            {
                lastMouseUpId = event.id;
            }
            else if (event.type == skui::ElementEventType::Click)
            {
                lastClickId = event.id;
                if (rerenderBasePlateOnShapeClick &&
                    event.id.starts_with("export-act-set-shape__c-base__"))
                {
                    basePlateRerenderSucceeded = runtime.replaceHtmlById(
                        "export-panel-scroll-content",
                        exportBasePlatePanelScrollContentFixture());
                }
                else if (event.id == "gis-overlay-toggle")
                {
                    gisPanelOpen = !gisPanelOpen;
                    runtime.setVisibleById("gis-overlay-panel", gisPanelOpen);
                }
                else if (event.id == "open-export-page")
                {
                    navigationSucceeded = runtime.setVisibleById("preview-page", false) &&
                        runtime.setVisibleById("export-page", true);
                    activePage = "export";
                }
                else if (event.id == "open-task-page")
                {
                    navigationSucceeded = runtime.setVisibleById("preview-page", false) &&
                        runtime.setVisibleById("task-page", true);
                    activePage = "task";
                }
                else if (event.id == "export-back-preview" ||
                         event.id == "export-cancel" ||
                         event.id == "tasks-back-preview")
                {
                    navigationSucceeded = runtime.setVisibleById("preview-page", true) &&
                        runtime.setVisibleById("export-page", false) &&
                        runtime.setVisibleById("task-page", false);
                    activePage = "preview";
                }
            }
            else if (event.type == skui::ElementEventType::Change)
            {
                lastChangeId = event.id;
                lastChangeValue = event.value;
                lastChangeChecked = event.checked;
            }
        });

    std::string centerCoordinateKeyValue;
    runtime.setElementKeyDownCallback(
        [&](const skui::ElementEvent& event)
        {
            if (event.id == "center-coordinate" && event.key == VK_RETURN)
            {
                centerCoordinateKeyValue = event.value;
                return true;
            }
            return false;
        });

    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
    if (!postprocess_test::Require(
            runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight,
                                       static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "无法布局可编辑的中心经纬度") ||
        !postprocess_test::Require(
            runtime.setValueById("center-coordinate", "121.473701, 31.230416"),
            "无法写入中心经纬度编辑框"))
    {
        return false;
    }
    sendMouse(runtime, skui::EventType::MouseDown, 80.F, 886.F);
    sendMouse(runtime, skui::EventType::MouseUp, 80.F, 886.F);
    skui::Event enterEvent;
    enterEvent.type = skui::EventType::KeyDown;
    enterEvent.key = VK_RETURN;
    runtime.handleEvent(enterEvent);
    if (!postprocess_test::Require(
            centerCoordinateKeyValue == "121.473701, 31.230416",
            "中心经纬度输入框没有在回车时提交编辑值"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            !DomDownToolPreview::shouldConsumeSearchPopupPointerMessage(
                WM_MOUSEWHEEL, true),
            "搜索结果框内的滚轮消息不应被地图控制器消费") ||
        !postprocess_test::Require(
            DomDownToolPreview::shouldConsumeSearchPopupPointerMessage(
                WM_LBUTTONDOWN, true),
            "搜索结果框内的点击消息应由地图控制器隔离"))
    {
        return false;
    }

    if (!postprocess_test::Require(runtime.setVisibleById("place-search-results", true),
                                   "无法显示搜索结果面板") ||
        !postprocess_test::Require(
            runtime.replaceHtmlById(
                "place-search-results-content",
                "<div id=\"place-search-results-content\" "
                "class=\"search-state\">正在搜索...</div>"),
            "无法注入搜索加载状态") ||
        !postprocess_test::Require(
            runtime.replaceHtmlById(
                "place-search-results-content",
                "<div id=\"place-search-results-content\" "
                "class=\"search-result-list\">"
                "<button id=\"search-result-0\" class=\"search-result selected\" "
                "type=\"button\" data-action=\"select-place-result\">"
                "<span class=\"search-result-name\">天安门广场</span>"
                "<span class=\"search-result-address\">北京市东城区 · 东长安街</span></button>"
                "<button id=\"search-result-1\" class=\"search-result\" type=\"button\" "
                "data-action=\"select-place-result\">"
                "<span class=\"search-result-name\">天安门</span>"
                "<span class=\"search-result-address\">北京市东城区</span></button>"
                "<button class=\"search-result\" type=\"button\">候选地点 3</button>"
                "<button class=\"search-result\" type=\"button\">候选地点 4</button>"
                "<button class=\"search-result\" type=\"button\">候选地点 5</button>"
                "<button class=\"search-result\" type=\"button\">候选地点 6</button>"
                "<button class=\"search-result\" type=\"button\">候选地点 7</button>"
                "<button class=\"search-result\" type=\"button\">候选地点 8</button>"
                "<button class=\"search-result\" type=\"button\">候选地点 9</button>"
                "</div>"),
            "无法注入搜索候选结果"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight,
                                       static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "无法布局可滚动搜索结果") ||
        !postprocess_test::Require(
            runtime.scrollStateById("place-search-results-content").has_value() &&
                runtime.scrollStateById("place-search-results-content")->maxScrollY > 0.F,
            "搜索结果数量超出弹框后没有形成可滚动区域"))
    {
        return false;
    }
    skui::Event wheelEvent;
    wheelEvent.type = skui::EventType::MouseWheel;
    wheelEvent.x = 400.F;
    wheelEvent.y = 300.F;
    wheelEvent.wheelDelta = -120.F;
    runtime.handleEvent(wheelEvent);
    const std::optional<skui::ScrollState> searchScrollState =
        runtime.scrollStateById("place-search-results-content");
    if (!postprocess_test::Require(
            searchScrollState.has_value() && searchScrollState->scrollY > 0.F,
            "搜索结果框没有响应鼠标滚轮"))
    {
        return false;
    }
    runtime.setScrollOffsetById("place-search-results-content", 0.F, 0.F);

    sendMouse(runtime, skui::EventType::MouseDown, 260.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseUp, 260.F, 26.F);
    skui::Event textEvent;
    textEvent.type = skui::EventType::TextInput;
    textEvent.text = "天安门";
    runtime.handleEvent(textEvent);
    if (!postprocess_test::Require(lastInputId == "place-search-input" &&
                                       lastInputValue == "天安门",
                                   "搜索输入框没有接收中文输入"))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 520.F, 76.F);
    sendMouse(runtime, skui::EventType::MouseUp, 520.F, 76.F);
    if (!postprocess_test::Require(lastClickId == "search-result-0",
                                   "候选结果没有铺满弹层宽度，实际命中：" + lastClickId))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 260.F, 132.F);
    sendMouse(runtime, skui::EventType::MouseUp, 260.F, 132.F);
    if (!postprocess_test::Require(lastClickId == "search-result-1",
                                   "第二条候选结果无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 357.F - 150.F, 887.F);
    sendMouse(runtime, skui::EventType::MouseUp, 357.F - 150.F, 887.F);
    if (!postprocess_test::Require(lastClickId == "view-span-decrease",
                                   "视野缩小按钮无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 451.F - 150.F, 887.F);
    sendMouse(runtime, skui::EventType::MouseUp, 451.F - 150.F, 887.F);
    if (!postprocess_test::Require(lastClickId == "view-span-increase",
                                   "视野扩大按钮无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 800.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseUp, 800.F, 26.F);
    if (!postprocess_test::Require(lastClickId == "gis-overlay-toggle" && gisPanelOpen,
                                   "GIS multi-select trigger did not open the popup"))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 756.F, 124.F);
    sendMouse(runtime, skui::EventType::MouseUp, 756.F, 124.F);
    if (!postprocess_test::Require(lastChangeId == "gis-overlay-road" &&
                                       lastChangeValue == "road" && lastChangeChecked,
                                   "GIS road checkbox did not receive the popup click"))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 756.F, 152.F);
    sendMouse(runtime, skui::EventType::MouseUp, 756.F, 152.F);
    if (!postprocess_test::Require(lastChangeId == "gis-overlay-water" &&
                                       lastChangeValue == "water" && lastChangeChecked,
                                   "GIS water checkbox did not support multiple selection"))
    {
        return false;
    }

    runtime.setVisibleById("gis-overlay-panel", false);
    if (!postprocess_test::Require(runtime.setVisibleById("gis-color-panel", true),
                                   "GIS color picker panel could not be shown"))
    {
        return false;
    }
    const std::vector<std::string> pickerChildren =
        runtime.childElementIdsById("gis-color-picker-body");
    if (!postprocess_test::Require(
            std::ranges::find(pickerChildren, "gis-color-picker-field") !=
                    pickerChildren.end() &&
                std::ranges::find(pickerChildren, "gis-color-picker-hue") !=
                    pickerChildren.end(),
            "GIS color picker is missing the saturation/value field or hue strip"))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 840.F, 500.F);
    sendMouse(runtime, skui::EventType::MouseUp, 840.F, 500.F);
    if (!postprocess_test::Require(lastMouseDownId == "gis-color-picker-field" &&
                                       lastClickId == "gis-color-picker-field",
                                   "GIS saturation/value field did not receive pointer input"))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 940.F, 500.F);
    sendMouse(runtime, skui::EventType::MouseUp, 940.F, 500.F);
    if (!postprocess_test::Require(lastMouseDownId == "gis-color-picker-hue" &&
                                       lastClickId == "gis-color-picker-hue",
                                   "GIS hue strip did not receive pointer input"))
    {
        return false;
    }

    const std::vector<std::string> readoutChildren =
        runtime.childElementIdsById("gis-color-picker-readout");
    if (!postprocess_test::Require(
            std::ranges::find(readoutChildren, "gis-color-picker-actions") !=
                readoutChildren.end(),
            "GIS color picker is missing its tool button group"))
    {
        return false;
    }
    const std::vector<std::string> actionChildren =
        runtime.childElementIdsById("gis-color-picker-actions");
    if (!postprocess_test::Require(
            std::ranges::find(actionChildren, "gis-color-eyedropper") !=
                    actionChildren.end() &&
                std::ranges::find(actionChildren, "gis-color-reset") !=
                    actionChildren.end(),
            "GIS color picker is missing an eyedropper or reset button"))
    {
        return false;
    }
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseDown, 970.F, 568.F);
    sendMouse(runtime, skui::EventType::MouseUp, 970.F, 568.F);
    if (!postprocess_test::Require(lastClickId == "gis-color-eyedropper",
                                   "GIS screen eyedropper button did not receive pointer input, actual hit: " +
                                       lastClickId))
    {
        return false;
    }
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseDown, 1008.F, 568.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1008.F, 568.F);
    if (!postprocess_test::Require(lastClickId == "gis-color-reset",
                                   "GIS color reset button did not receive pointer input"))
    {
        return false;
    }

    runtime.setValueById("center-coordinate", "116.397389, 39.908722");
    if (!postprocess_test::Require(
            runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight,
                                       static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "搜索 UI 离屏渲染失败"))
    {
        return false;
    }

    size_t nonTransparentPixels = 0;
    for (const uint32_t pixel : pixels)
    {
        if ((pixel >> 24U) != 0U)
        {
            ++nonTransparentPixels;
        }
    }
    if (!postprocess_test::Require(nonTransparentPixels > pixels.size() / 4,
                                   "搜索 UI 渲染结果为空"))
    {
        return false;
    }

    const std::filesystem::path outputPath = sourceRoot / "tmp" / "preview-search-ui.png";
    std::error_code directoryError;
    std::filesystem::create_directories(outputPath.parent_path(), directoryError);
    if (!postprocess_test::Require(!directoryError, "无法创建搜索 UI 截图目录"))
    {
        return false;
    }

    cv::Mat image(kHeight, kWidth, CV_8UC4, pixels.data());
    if (!postprocess_test::Require(cv::imwrite(outputPath.string(), image),
                                   "无法写入搜索 UI 截图"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            runtime.setVisibleById("place-search-results", false) &&
                runtime.setVisibleById("gis-overlay-panel", false) &&
                runtime.setVisibleById("gis-color-panel", false) &&
                runtime.renderToBgraPixels(
                    pixels.data(), kWidth, kHeight,
                    static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "无法在无浮层状态下布局预览工具栏"))
    {
        return false;
    }

    lastMouseDownId.clear();
    lastMouseUpId.clear();
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 1214.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1214.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1214.F, 26.F);
    if (!postprocess_test::Require(lastClickId == "open-export-page" &&
                                       activePage == "export" && navigationSucceeded,
                                   "导出按钮的可见文字区域无法打开配置页，MouseDown：" +
                                       lastMouseDownId + "，MouseUp：" + lastMouseUpId +
                                       "，Click：" + lastClickId))
    {
        return false;
    }
    navigationSucceeded = false;
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 1390.F, 28.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1390.F, 28.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1390.F, 28.F);
    if (!postprocess_test::Require(lastClickId == "export-back-preview" &&
                                       activePage == "preview" && navigationSucceeded,
                                   "导出配置页的返回按钮无法恢复预览页，Click：" +
                                       lastClickId) ||
        !postprocess_test::Require(
            runtime.renderToBgraPixels(
                pixels.data(), kWidth, kHeight,
                static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "无法在返回后重新布局预览工具栏"))
    {
        return false;
    }
    navigationSucceeded = false;

    lastMouseDownId.clear();
    lastMouseUpId.clear();
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 1295.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1295.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1295.F, 26.F);
    if (!postprocess_test::Require(lastClickId == "open-task-page" &&
                                       activePage == "task" && navigationSucceeded,
                                   "任务按钮的可见文字区域无法打开任务页，MouseDown：" +
                                       lastMouseDownId + "，MouseUp：" + lastMouseUpId +
                                       "，Click：" + lastClickId))
    {
        return false;
    }
    navigationSucceeded = false;
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 1390.F, 28.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1390.F, 28.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1390.F, 28.F);
    if (!postprocess_test::Require(lastClickId == "tasks-back-preview" &&
                                       activePage == "preview" && navigationSucceeded,
                                   "导出任务页的返回按钮无法恢复预览页，Click：" +
                                       lastClickId))
    {
        return false;
    }
    navigationSucceeded = false;

    if (!postprocess_test::Require(runtime.setVisibleById("preview-page", false),
                                   "无法隐藏预览页面") ||
        !postprocess_test::Require(runtime.setVisibleById("export-page", true),
                                   "无法显示导出配置页面") ||
        !postprocess_test::Require(runtime.setVisibleById("task-page", false),
                                   "无法隐藏导出任务页面") ||
        !postprocess_test::Require(
            runtime.setTextById("export-config-summary",
                                "中心 116.39739, 39.90872 · z15 · 半径 2000 米"),
            "无法填充导出配置摘要") ||
        !postprocess_test::Require(runtime.setValueById("export-lon", "116.397389"),
                                   "无法填充导出中心经度") ||
        !postprocess_test::Require(runtime.setValueById("export-lat", "39.908722"),
                                   "无法填充导出中心纬度") ||
        !postprocess_test::Require(runtime.setValueById("export-config-radius", "2000"),
                                   "无法填充导出半径") ||
        !postprocess_test::Require(runtime.setValueById("export-zoom", "15"),
                                   "无法填充导出缩放级别") ||
        !postprocess_test::Require(runtime.setValueById("export-output", ""),
                                   "无法清空导出目录") ||
        !postprocess_test::Require(setExportComposerFixture(runtime),
                                   "无法填充组件式导出配置") ||
        !postprocess_test::Require(runtime.setValueById("export-xyz-resolution", "1024"),
                                   "标准 XYZ 分辨率选择不可用"))
    {
        return false;
    }
    const std::vector<std::string> composerChildren =
        runtime.childElementIdsById("export-group-list");
    const std::vector<std::string> satelliteGroupChildren =
        runtime.childElementIdsById("export-group-satellite");
    if (!postprocess_test::Require(
            composerChildren ==
                    std::vector<std::string>{"export-group-satellite"} &&
                std::ranges::find(satelliteGroupChildren,
                                  "export-act-select-component__c1") !=
                    satelliteGroupChildren.end() &&
                std::ranges::find(satelliteGroupChildren,
                                  "export-act-select-component__c2") !=
                    satelliteGroupChildren.end() &&
                std::ranges::find(satelliteGroupChildren,
                                  "export-act-select-component__c3") !=
                    satelliteGroupChildren.end(),
            "内容组没有保持组件列表和添加入口"))
    {
        return false;
    }

    const std::filesystem::path exportOutputPath =
        sourceRoot / "tmp" / "preview-export-config-ui.png";
    if (!writeRuntimeScreenshot(runtime, exportOutputPath, kWidth, kHeight) ||
        !writeRuntimeScreenshot(runtime,
                                sourceRoot / "tmp" /
                                    "preview-export-config-ui-component.png",
                                kWidth, kHeight))
    {
        return false;
    }

    if (!postprocess_test::Require(
            runtime.setAttributeById(
                "export-range-mode-center", "aria-pressed", "false") &&
                runtime.setAttributeById(
                    "export-range-mode-kml", "aria-pressed", "true") &&
                runtime.setVisibleById("export-lon-field", false) &&
                runtime.setVisibleById("export-lat-field", false) &&
                runtime.setVisibleById("export-radius-field", false) &&
                runtime.setVisibleById("export-kml-field", true) &&
                runtime.setTextById("export-kml-name", "15.kml") &&
                runtime.setTextById(
                    "export-config-summary", "15.kml · 1 个多边形 · z15"),
            "无法切换到 KML 导出范围界面") ||
        !writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-config-kml-ui.png",
            kWidth,
            kHeight) ||
        !postprocess_test::Require(
            runtime.setAttributeById(
                "export-range-mode-center", "aria-pressed", "true") &&
                runtime.setAttributeById(
                    "export-range-mode-kml", "aria-pressed", "false") &&
                runtime.setVisibleById("export-lon-field", true) &&
                runtime.setVisibleById("export-lat-field", true) &&
                runtime.setVisibleById("export-radius-field", true) &&
                runtime.setVisibleById("export-kml-field", false) &&
                runtime.setTextById(
                    "export-config-summary",
                    "中心 116.39739, 39.90872 · z15 · 半径 2000 米"),
            "无法恢复中心点加半径导出范围界面"))
    {
        return false;
    }

    const std::optional<skui::ScrollState> standardPanelScroll =
        runtime.scrollStateById("export-panel-body");
    const std::optional<skui::ScrollState> standardGroupListScroll =
        runtime.scrollStateById("export-group-list");
    if (!postprocess_test::Require(
            standardPanelScroll.has_value() &&
                standardPanelScroll->maxScrollX <= 0.5F,
            "短参数面板产生了横向溢出") ||
        !postprocess_test::Require(
            standardGroupListScroll.has_value() &&
                standardGroupListScroll->maxScrollY <= 0.5F,
            "组件较少时左侧列表不应出现纵向滚动"))
    {
        return false;
    }

    lastClickId.clear();
    lastMouseDownId.clear();
    lastMouseUpId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 122.F, 338.F);
    sendMouse(runtime, skui::EventType::MouseDown, 122.F, 338.F);
    sendMouse(runtime, skui::EventType::MouseUp, 122.F, 338.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-select-component__c1",
            "点击组件行名称区域无法切换参数，实际命中：" + lastClickId))
    {
        return false;
    }

    lastClickId.clear();
    lastMouseDownId.clear();
    lastMouseUpId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 736.F, 338.F);
    sendMouse(runtime, skui::EventType::MouseDown, 736.F, 338.F);
    sendMouse(runtime, skui::EventType::MouseUp, 736.F, 338.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-open-params__c1",
            "组件参数按钮的可见文字区域无法点击，MouseDown：" +
                lastMouseDownId + "，MouseUp：" + lastMouseUpId +
                "，Click：" + lastClickId))
    {
        return false;
    }

    lastClickId.clear();
    lastMouseDownId.clear();
    lastMouseUpId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 844.F, 338.F);
    sendMouse(runtime, skui::EventType::MouseDown, 844.F, 338.F);
    sendMouse(runtime, skui::EventType::MouseUp, 844.F, 338.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-remove-component__c1",
            "组件移除按钮的可见文字区域无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    lastClickId.clear();
    lastMouseDownId.clear();
    lastMouseUpId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 116.F, 600.F);
    sendMouse(runtime, skui::EventType::MouseDown, 116.F, 600.F);
    sendMouse(runtime, skui::EventType::MouseUp, 116.F, 600.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-open-components__g1",
            "添加组件按钮的可见文字区域无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    lastClickId.clear();
    lastMouseDownId.clear();
    lastMouseUpId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 1000.F, 456.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1000.F, 456.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1000.F, 456.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-open-sources__c3",
            "添加图源按钮的可见文字区域无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    lastClickId.clear();
    lastMouseDownId.clear();
    lastMouseUpId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 1182.F, 822.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1182.F, 822.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1182.F, 822.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-done-panel",
            "参数面板完成按钮的可见文字区域无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    if (!postprocess_test::Require(setExportComposerFixture(runtime, true),
                                   "无法展开完整图源菜单") ||
        !writeRuntimeScreenshot(
            runtime, sourceRoot / "tmp" / "preview-export-source-menu.png",
            kWidth, kHeight))
    {
        return false;
    }
    runtime.resize(1100, kHeight, 1.F);
    if (!writeRuntimeScreenshot(
            runtime, sourceRoot / "tmp" / "preview-export-source-menu-narrow.png",
            1100, kHeight))
    {
        return false;
    }
    runtime.resize(kWidth, kHeight, 1.F);
    if (
        !postprocess_test::Require(setExportComposerFixture(runtime),
                                   "无法恢复导出配置夹具"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            setExportComponentMenuOverflowFixture(runtime),
            "无法填充多组件添加菜单") ||
        !writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-component-menu-overflow.png",
            kWidth, kHeight))
    {
        return false;
    }
    const std::optional<skui::ScrollState> componentMenuListScroll =
        runtime.scrollStateById("export-group-list");
    if (!postprocess_test::Require(
            componentMenuListScroll.has_value() &&
                componentMenuListScroll->maxScrollX <= 0.5F &&
                componentMenuListScroll->maxScrollY > 0.F,
            "组件较多时添加菜单没有进入左侧纵向滚动区"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            runtime.setScrollOffsetById("export-group-list", 0.F,
                                        componentMenuListScroll->maxScrollY),
            "无法滚动到添加组件菜单底部") ||
        !writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-component-menu-scrolled.png",
            kWidth, kHeight))
    {
        return false;
    }
    const std::optional<skui::ScrollState> scrolledComponentMenuList =
        runtime.scrollStateById("export-group-list");
    if (!postprocess_test::Require(
            scrolledComponentMenuList.has_value() &&
                scrolledComponentMenuList->scrollY > 0.F,
            "添加组件菜单无法通过左侧列表滚动完整显示"))
    {
        return false;
    }
    lastClickId.clear();
    sendMouse(runtime, skui::EventType::MouseMove, 680.F, 823.F);
    sendMouse(runtime, skui::EventType::MouseDown, 680.F, 823.F);
    sendMouse(runtime, skui::EventType::MouseUp, 680.F, 823.F);
    if (!postprocess_test::Require(
            lastClickId == "export-act-add-component__g1__geojson",
            "滚动到底部后 GeoJSON 组件按钮仍无法点击，实际命中：" +
                lastClickId) ||
        !postprocess_test::Require(setExportComposerFixture(runtime),
                                   "无法在菜单滚动测试后恢复导出配置夹具"))
    {
        return false;
    }

    if (!postprocess_test::Require(setExportBasePlatePanelFixture(runtime),
                                   "无法填充底板模型长参数面板") ||
        !writeRuntimeScreenshot(
            runtime, sourceRoot / "tmp" / "preview-export-base-plate-panel.png",
            kWidth, kHeight))
    {
        return false;
    }
    const std::optional<skui::ScrollState> basePlatePanelScroll =
        runtime.scrollStateById("export-panel-body");
    if (!postprocess_test::Require(
            basePlatePanelScroll.has_value() &&
                basePlatePanelScroll->maxScrollX <= 0.5F &&
                basePlatePanelScroll->maxScrollY > 0.F,
            "长参数面板没有保持仅纵向滚动"))
    {
        return false;
    }
    struct ShapeButtonHit
    {
        float x;
        const char* id;
    };
    constexpr std::array<ShapeButtonHit, 2> shapeButtonHits = {{
        {1066.F, "export-act-set-shape__c-base__plane"},
        {1273.F, "export-act-set-shape__c-base__sphere"},
    }};
    for (const ShapeButtonHit& button : shapeButtonHits)
    {
        lastClickId.clear();
        sendMouse(runtime, skui::EventType::MouseMove, button.x, 653.F);
        sendMouse(runtime, skui::EventType::MouseDown, button.x, 653.F);
        sendMouse(runtime, skui::EventType::MouseUp, button.x, 653.F);
        if (!postprocess_test::Require(
                lastClickId == button.id,
                std::string("底板形态按钮无法点击，预期：") + button.id +
                    "，实际：" + lastClickId))
        {
            return false;
        }
    }
    const float basePlateScrollTarget = basePlatePanelScroll->maxScrollY;
    if (!postprocess_test::Require(
            runtime.setScrollOffsetById("export-panel-body", 0.F, basePlateScrollTarget),
            "无法滚动到底板参数面板底部") ||
        !writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-base-plate-panel-scrolled.png",
            kWidth, kHeight))
    {
        return false;
    }
    rerenderBasePlateOnShapeClick = true;
    basePlateRerenderSucceeded = false;
    lastClickId.clear();
    const float scrolledShapeY = 653.F - basePlateScrollTarget;
    sendMouse(runtime, skui::EventType::MouseMove, 1273.F, scrolledShapeY);
    sendMouse(runtime, skui::EventType::MouseDown, 1273.F, scrolledShapeY);
    sendMouse(runtime, skui::EventType::MouseUp, 1273.F, scrolledShapeY);
    rerenderBasePlateOnShapeClick = false;
    if (!postprocess_test::Require(
            lastClickId == "export-act-set-shape__c-base__sphere" &&
                basePlateRerenderSucceeded,
            "形态按钮点击回调没有完成参数内容更新"))
    {
        return false;
    }
    const std::optional<skui::ScrollState> restoredBasePlatePanelScroll =
        runtime.scrollStateById("export-panel-body");
    if (!postprocess_test::Require(
            restoredBasePlatePanelScroll.has_value() &&
                restoredBasePlatePanelScroll->scrollY > 0.F,
            "底板参数面板重建后滚动位置仍被重置到顶部") ||
        !postprocess_test::Require(
            runtime.setScrollOffsetById("export-panel-body", 0.F, 0.F),
            "无法复位底板参数面板滚动位置"))
    {
        return false;
    }
    runtime.resize(1100, kHeight, 1.F);
    if (!writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-base-plate-panel-narrow.png",
            1100, kHeight))
    {
        return false;
    }
    const std::optional<skui::ScrollState> narrowBasePlatePanelScroll =
        runtime.scrollStateById("export-panel-body");
    if (!postprocess_test::Require(
            narrowBasePlatePanelScroll.has_value() &&
                narrowBasePlatePanelScroll->maxScrollX <= 0.5F &&
                narrowBasePlatePanelScroll->maxScrollY > 0.F,
            "窄窗口长参数面板没有保持仅纵向滚动"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            runtime.setScrollOffsetById("export-panel-body", 0.F,
                                       narrowBasePlatePanelScroll->maxScrollY),
            "无法滚动到窄窗口地形预算字段") ||
        !writeRuntimeScreenshot(
            runtime, sourceRoot / "tmp" / "preview-export-base-plate-budget-narrow.png",
            1100, kHeight))
    {
        return false;
    }
    runtime.resize(kWidth, kHeight, 1.F);
    if (
        !postprocess_test::Require(setExportComposerFixture(runtime),
                                   "无法恢复导出配置夹具"))
    {
        return false;
    }

    for (int width : {1440, 1100})
    {
        runtime.resize(width, kHeight, 1.F);
        if (!postprocess_test::Require(setExportBasePlatePanelFixture(runtime, true),
                "无法填充距离档位面板"))
        {
            return false;
        }
        const auto scroll = runtime.scrollStateById("export-panel-body");
        if (!postprocess_test::Require(
                scroll && scroll->maxScrollX <= 0.5F &&
                    runtime.setScrollOffsetById("export-panel-body", 0.F, scroll->maxScrollY),
                "距离档位面板发生横向裁切或无法滚动") ||
            !writeRuntimeScreenshot(runtime,
                sourceRoot / "tmp" / ("preview-distance-lod-" + std::to_string(width) + ".png"),
                width, kHeight))
        {
            return false;
        }
    }
    runtime.resize(kWidth, kHeight, 1.F);
    if (!postprocess_test::Require(setExportBuildingPanelFixture(runtime),
                                   "无法填充建筑模型参数面板") ||
        !writeRuntimeScreenshot(
            runtime, sourceRoot / "tmp" / "preview-export-building-panel.png",
            kWidth, kHeight))
    {
        return false;
    }
    const std::optional<skui::ScrollState> buildingPanelScroll =
        runtime.scrollStateById("export-panel-body");
    if (!postprocess_test::Require(
            buildingPanelScroll.has_value() &&
                buildingPanelScroll->maxScrollX <= 0.5F &&
                buildingPanelScroll->maxScrollY > 0.F,
            "建筑模型参数面板没有保持仅纵向滚动"))
    {
        return false;
    }
    constexpr std::array<ShapeButtonHit, 2> buildingShapeButtonHits = {{
        {1154.F, "export-act-set-shape__c-building__plane"},
        {1302.F, "export-act-set-shape__c-building__sphere"},
    }};
    for (const ShapeButtonHit& button : buildingShapeButtonHits)
    {
        lastClickId.clear();
        sendMouse(runtime, skui::EventType::MouseMove, button.x, 457.F);
        sendMouse(runtime, skui::EventType::MouseDown, button.x, 457.F);
        sendMouse(runtime, skui::EventType::MouseUp, button.x, 457.F);
        if (!postprocess_test::Require(
                lastClickId == button.id,
                std::string("建筑形态按钮无法点击，预期：") + button.id +
                    "，实际：" + lastClickId))
        {
            return false;
        }
    }
    if (!postprocess_test::Require(
            runtime.setScrollOffsetById("export-panel-body", 0.F,
                                        buildingPanelScroll->maxScrollY),
            "无法滚动到建筑贴图目录") ||
        !writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-building-panel-scrolled.png",
            kWidth, kHeight))
    {
        return false;
    }
    struct DirectoryButtonHit
    {
        float y;
        const char* id;
    };
    constexpr std::array<DirectoryButtonHit, 2> directoryButtonHits = {{
        {663.F, "export-act-browse-wall-path__c-building"},
        {727.F, "export-act-browse-roof-path__c-building"},
    }};
    for (const DirectoryButtonHit& button : directoryButtonHits)
    {
        lastClickId.clear();
        sendMouse(runtime, skui::EventType::MouseMove, 1348.F, button.y);
        sendMouse(runtime, skui::EventType::MouseDown, 1348.F, button.y);
        sendMouse(runtime, skui::EventType::MouseUp, 1348.F, button.y);
        if (!postprocess_test::Require(
                lastClickId == button.id,
                std::string("建筑贴图目录按钮无法点击，预期：") + button.id +
                    "，实际：" + lastClickId))
        {
            return false;
        }
    }
    if (!postprocess_test::Require(
            runtime.setScrollOffsetById("export-panel-body", 0.F, 0.F),
            "无法复位建筑模型参数面板"))
    {
        return false;
    }
    runtime.resize(1100, kHeight, 1.F);
    if (!writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-building-panel-narrow.png",
            1100, kHeight))
    {
        return false;
    }
    const std::optional<skui::ScrollState> narrowBuildingPanelScroll =
        runtime.scrollStateById("export-panel-body");
    if (!postprocess_test::Require(
            narrowBuildingPanelScroll.has_value() &&
                narrowBuildingPanelScroll->maxScrollX <= 0.5F,
            "窄窗口建筑模型参数面板产生了横向裁切"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            runtime.setScrollOffsetById("export-panel-body", 0.F,
                                        narrowBuildingPanelScroll->maxScrollY),
            "窄窗口无法滚动到建筑贴图目录") ||
        !writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" /
                "preview-export-building-panel-narrow-scrolled.png",
            1100, kHeight))
    {
        return false;
    }
    runtime.resize(kWidth, kHeight, 1.F);
    if (!postprocess_test::Require(setExportComposerFixture(runtime),
                                   "无法再次恢复导出配置夹具"))
    {
        return false;
    }

    runtime.resize(1100, kHeight, 1.F);
    if (
        !writeRuntimeScreenshot(
            runtime, sourceRoot / "tmp" / "preview-export-config-ui-component-narrow.png",
            1100, kHeight))
    {
        return false;
    }
    runtime.resize(1100, 1400, 1.F);
    if (!writeRuntimeScreenshot(
            runtime,
            sourceRoot / "tmp" / "preview-export-config-ui-component-narrow-tall.png",
            1100, 1400))
    {
        return false;
    }
    runtime.resize(kWidth, kHeight, 1.F);

    if (!postprocess_test::Require(
            runtime.setVisibleById("export-page", false) &&
                runtime.setVisibleById("task-page", true) &&
                runtime.replaceHtmlById(
                    "export-task-list-items",
                    "<div id=\"export-task-list-items\" class=\"task-list-items\">"
                    "<article class=\"task-row\"><div><div class=\"task-title\">"
                    "z15 2000 米 高德卫星 + 高德 GIS</div>"
                    "<div class=\"task-detail\">合并大图 · GIS 合并 · 2026-08-10 10:00:00"
                    "</div><div class=\"task-output\">各下载器默认目录</div></div><div>"
                    "<div class=\"task-stage\"><span>渲染 GIS PNG · 水+绿地</span>"
                    "<span>48% · 24/50</span></div><div class=\"progress-track\">"
                    "<div class=\"progress-value\" style=\"width:48%\"></div></div>"
                    "</div><div><span class=\"task-state running\">导出中</span>"
                    "</div><div class=\"task-row-actions\">"
                    "<button id=\"task-open-running\" class=\"task-open-output\">"
                    "<svg viewBox=\"0 0 24 24\"><path d=\"M3 7h18v12H3z\" "
                    "fill=\"none\" stroke=\"#cbd4d0\"/></svg></button>"
                    "<button id=\"task-config-running\" class=\"task-reopen-config\">"
                    "<svg viewBox=\"0 0 24 24\"><path d=\"M4 6h16M4 12h16M4 18h16\" "
                    "fill=\"none\" stroke=\"#88d9be\"/></svg></button>"
                    "<button class=\"danger-command\">"
                    "取消</button></div></article>"
                    "<article class=\"task-row\"><div><div class=\"task-title\">"
                    "z14 5000 米 高德卫星</div><div class=\"task-detail\">"
                    "XYZ 瓦片 · 2026-08-10 09:00:00</div><div class=\"task-output\">"
                    "D:/exports/task-2</div></div><div><div class=\"task-stage\">"
                    "<span>导出完成</span><span>100% · 56/56</span></div>"
                    "<div class=\"progress-track\"><div class=\"progress-value\" "
                    "style=\"width:100%\"></div></div></div><div>"
                    "<span class=\"task-state succeeded\">已完成</span></div>"
                    "<div class=\"task-row-actions\"><button id=\"task-open-done\" "
                    "class=\"task-open-output\"><svg viewBox=\"0 0 24 24\">"
                    "<path d=\"M3 7h18v12H3z\" fill=\"none\" "
                    "stroke=\"#cbd4d0\"/></svg></button>"
                    "<button id=\"task-config-done\" class=\"task-reopen-config\">"
                    "<svg viewBox=\"0 0 24 24\"><path d=\"M4 6h16M4 12h16M4 18h16\" "
                    "fill=\"none\" stroke=\"#88d9be\"/></svg></button>"
                    "<button id=\"task-remove-done\" class=\"task-remove-record\">"
                    "<svg viewBox=\"0 0 24 24\"><path d=\"M6 6l12 12M18 6L6 18\" "
                    "fill=\"none\" stroke=\"#cbd4d0\"/></svg></button>"
                    "<button id=\"task-delete-done\" class=\"task-delete-output\">"
                    "<svg viewBox=\"0 0 24 24\"><path d=\"M4 7h16M7 7l1 13h8l1-13\" "
                    "fill=\"none\" stroke=\"#ffaaa2\"/></svg></button>"
                    "</div></article></div>"),
            "无法显示或填充导出任务页面"))
    {
        return false;
    }
    std::fill(pixels.begin(), pixels.end(), 0);
    if (!postprocess_test::Require(
            runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight,
                                       static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "导出任务 UI 离屏渲染失败"))
    {
        return false;
    }
    const std::filesystem::path taskOutputPath =
        sourceRoot / "tmp" / "preview-export-task-ui.png";
    cv::Mat taskImage(kHeight, kWidth, CV_8UC4, pixels.data());
    if (!postprocess_test::Require(cv::imwrite(taskOutputPath.string(), taskImage),
                                   "无法写入导出任务 UI 截图"))
    {
        return false;
    }

    std::cout << "PreviewSearchUiTest passed: " << outputPath << ", "
              << exportOutputPath << ", " << taskOutputPath << '\n';
    return true;
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(runTest);
}
