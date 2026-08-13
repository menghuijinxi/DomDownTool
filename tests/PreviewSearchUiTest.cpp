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

bool runTest()
{
    const std::filesystem::path sourceRoot = DOMDOWNTOOL_SOURCE_DIR;
    const std::filesystem::path documentPath =
        sourceRoot / "resources" / "preview" / "index.html";
    skui::RuntimeOptions options;
    options.assetRoot = documentPath.parent_path().generic_string();
    skui::Runtime runtime(std::move(options));
    runtime.resize(kWidth, kHeight, 1.F);
    if (!postprocess_test::Require(runtime.loadDocument(documentPath.generic_string()),
                                   "无法加载预览 HTML：" + runtime.lastError()))
    {
        return false;
    }
    const std::array<std::string_view, 24> exportCheckboxIds = {
        "export-provider-gaode", "export-provider-tencent",
        "export-provider-tianditu", "export-provider-baidu",
        "export-provider-google", "export-provider-bing", "export-provider-arcgis",
        "export-gis-group-1", "export-gis-group-2", "export-gis-group-3",
        "export-gis-group-4", "export-gis-group-5", "export-gis-group-6",
        "export-gis-group-7", "export-gis-group-8", "export-gis-group-9",
        "export-gis-combine-overlay", "export-gis-combine-merged",
        "export-gis-combine-separate", "export-satellite-tiles",
        "export-satellite-merged", "export-fbx", "export-geojson",
        "export-building-height-colors"};
    for (const std::string_view checkboxId : exportCheckboxIds)
    {
        const std::string checkedMarkup =
            "id=\"" + std::string(checkboxId) + "\" type=\"checkbox\" checked";
        if (!postprocess_test::Require(
                !postprocess_test::ContainsText(documentPath, checkedMarkup),
                "export checkbox has an unexpected static checked state: " +
                    std::string(checkboxId)))
        {
            return false;
        }
    }

    if (!postprocess_test::Require(
            runtime.setVisibleById("preview-page", true) &&
                runtime.setVisibleById("export-page", false) &&
                runtime.setVisibleById("task-page", false),
            "预览、导出配置或导出任务页面结构缺失") ||
        !postprocess_test::Require(
            runtime.setValueById("export-radius", "2000") &&
                runtime.setValueById("export-config-radius", "2000"),
            "预览页或导出配置页缺少半径输入"))
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

    const std::vector<std::string> exportHeaderChildren =
        runtime.childElementIdsById("export-page-header");
    const std::vector<std::string> taskHeaderChildren =
        runtime.childElementIdsById("task-page-header");
    const auto exportTasksButton =
        std::ranges::find(exportHeaderChildren, "export-open-tasks");
    const auto exportBackButton =
        std::ranges::find(exportHeaderChildren, "export-back-preview");
    const auto taskClearButton =
        std::ranges::find(taskHeaderChildren, "tasks-clear-finished");
    const auto taskNewButton =
        std::ranges::find(taskHeaderChildren, "tasks-new-export");
    const auto taskBackButton =
        std::ranges::find(taskHeaderChildren, "tasks-back-preview");
    if (!postprocess_test::Require(
            exportTasksButton != exportHeaderChildren.end() &&
                exportBackButton == std::next(exportTasksButton),
            "导出配置页的返回按钮没有紧跟在导出任务按钮后") ||
        !postprocess_test::Require(
            taskClearButton != taskHeaderChildren.end() &&
                taskNewButton == std::next(taskClearButton) &&
                taskNewButton != taskHeaderChildren.end() &&
                taskBackButton == std::next(taskNewButton),
            "导出任务页的返回按钮没有紧跟在新建导出按钮后"))
    {
        return false;
    }

    const std::vector<std::string> expectedInitialGisItems = {
        "export-gis-static-road", "export-gis-static-building",
        "export-gis-static-water", "export-gis-static-point",
        "export-gis-static-green", "export-gis-static-boundary",
        "export-gis-static-highway", "export-gis-static-other",
        "export-gis-static-subway"};
    const std::vector<std::string> expectedGisCombinationItems = {
        "export-gis-combine-overlay-item", "export-gis-combine-merged-item",
        "export-gis-combine-separate-item"};
    if (!postprocess_test::Require(
            runtime.childElementIdsById("export-gis-groups") == expectedInitialGisItems,
            "GIS 数据初始结构没有使用单层两列网格") ||
        !postprocess_test::Require(
            runtime.childElementIdsById("export-gis-combine-options") ==
                expectedGisCombinationItems,
            "GIS 组合方式初始结构不是三个独立复选项"))
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
                if (event.id == "gis-overlay-toggle")
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
    sendMouse(runtime, skui::EventType::MouseDown, 970.F, 554.F);
    sendMouse(runtime, skui::EventType::MouseUp, 970.F, 554.F);
    if (!postprocess_test::Require(lastClickId == "gis-color-eyedropper",
                                   "GIS screen eyedropper button did not receive pointer input"))
    {
        return false;
    }
    sendMouse(runtime, skui::EventType::MouseDown, 1008.F, 554.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1008.F, 554.F);
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
    sendMouse(runtime, skui::EventType::MouseMove, 1254.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1254.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1254.F, 26.F);
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

    if (!postprocess_test::Require(
            runtime.setVisibleById("preview-page", false) &&
                runtime.setVisibleById("export-page", true) &&
                runtime.setVisibleById("task-page", false) &&
                runtime.setTextById(
                    "export-config-summary",
                    "中心 116.39739, 39.90872 · z15 · 半径 2000 米") &&
                runtime.setValueById("export-lon", "116.397389") &&
                runtime.setValueById("export-lat", "39.908722") &&
                runtime.setValueById("export-zoom", "15") &&
                runtime.setValueById("export-output", "") &&
                runtime.setAttributeById("export-gis-group-1", "checked", "") &&
                runtime.setAttributeById("export-gis-group-2", "checked", "") &&
                runtime.removeAttributeById("export-gis-group-3", "checked"),
            "无法显示或填充导出配置页面"))
    {
        return false;
    }
    const std::vector<std::string> gisGroupChildren =
        runtime.childElementIdsById("export-gis-static-road");
    if (!postprocess_test::Require(
            runtime.childElementIdsById("export-gis-groups") == expectedInitialGisItems,
            "更新 GIS 勾选状态后破坏了静态两列网格") ||
        !postprocess_test::Require(
            std::ranges::find(gisGroupChildren, "export-gis-group-1") !=
                gisGroupChildren.end(),
            "GIS 数据项没有使用真实的多选框"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            runtime.childElementIdsById("export-gis-combine-options") ==
                expectedGisCombinationItems &&
                runtime.setAttributeById("export-gis-combine-overlay", "checked", "") &&
                runtime.setAttributeById("export-gis-combine-merged", "checked", "") &&
                runtime.setAttributeById("export-gis-combine-separate", "checked", ""),
            "GIS 组合方式没有提供三个可同时勾选的复选框"))
    {
        return false;
    }
    std::fill(pixels.begin(), pixels.end(), 0);
    if (!postprocess_test::Require(
            runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight,
                                       static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.F),
            "导出配置 UI 离屏渲染失败"))
    {
        return false;
    }
    const std::filesystem::path exportOutputPath =
        sourceRoot / "tmp" / "preview-export-config-ui.png";
    cv::Mat exportImage(kHeight, kWidth, CV_8UC4, pixels.data());
    if (!postprocess_test::Require(cv::imwrite(exportOutputPath.string(), exportImage),
                                   "无法写入导出配置 UI 截图"))
    {
        return false;
    }

    if (!postprocess_test::Require(
            runtime.setVisibleById("export-page", false) &&
                runtime.setVisibleById("task-page", true) &&
                runtime.replaceHtmlById(
                    "export-task-list",
                    "<article class=\"task-row\"><div><div class=\"task-title\">"
                    "z15 2000 米 高德卫星 + 高德 GIS</div>"
                    "<div class=\"task-detail\">合并大图 · GIS 合并 · 2026-08-10 10:00:00"
                    "</div><div class=\"task-output\">各下载器默认目录</div></div><div>"
                    "<div class=\"task-stage\"><span>渲染 GIS PNG · 水+绿地</span>"
                    "<span>48% · 24/50</span></div><div class=\"progress-track\">"
                    "<div class=\"progress-value\" style=\"width:48%\"></div></div>"
                    "</div><div><span class=\"task-state running\">导出中</span>"
                    "</div><div class=\"task-row-actions\"><button class=\"danger-command\">"
                    "取消</button></div></article>"),
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
