#include "PostProcessTestSupport.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/core/mat.hpp"
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
                "</div>"),
            "无法注入搜索候选结果"))
    {
        return false;
    }

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

    sendMouse(runtime, skui::EventType::MouseDown, 357.F, 887.F);
    sendMouse(runtime, skui::EventType::MouseUp, 357.F, 887.F);
    if (!postprocess_test::Require(lastClickId == "view-span-decrease",
                                   "视野缩小按钮无法点击，实际命中：" + lastClickId))
    {
        return false;
    }

    sendMouse(runtime, skui::EventType::MouseDown, 451.F, 887.F);
    sendMouse(runtime, skui::EventType::MouseUp, 451.F, 887.F);
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

    std::vector<uint32_t> pixels(static_cast<size_t>(kWidth) * kHeight);
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
    sendMouse(runtime, skui::EventType::MouseMove, 1327.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseDown, 1327.F, 26.F);
    sendMouse(runtime, skui::EventType::MouseUp, 1327.F, 26.F);
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
                runtime.setValueById("export-satellite-output", "cache/高德地图/Google_墨卡托") &&
                runtime.setValueById("export-gis-output", "cache/高德地图/gis数据/输出") &&
                runtime.replaceHtmlById(
                    "export-gis-groups",
                    "<div class=\"gis-group-grid\">"
                    "<div id=\"export-gis-group-row-1\" class=\"gis-group-item\">"
                    "<input id=\"export-gis-group-1\" type=\"checkbox\" value=\"1\" checked>"
                    "<label class=\"gis-group-label\" for=\"export-gis-group-1\">"
                    "道路</label><span class=\"gis-group-count\">1</span></div>"
                    "<div id=\"export-gis-group-row-2\" class=\"gis-group-item\">"
                    "<input id=\"export-gis-group-2\" type=\"checkbox\" value=\"2\" checked>"
                    "<label class=\"gis-group-label\" for=\"export-gis-group-2\">水</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">绿地</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">高速</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">地铁</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">建筑</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">点</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">边界</label>"
                    "<span class=\"gis-group-count\">1</span></div>"
                    "<div class=\"gis-group-item\"><input type=\"checkbox\">"
                    "<label class=\"gis-group-label\">其他</label>"
                    "<span class=\"gis-group-count\">1</span></div></div>"),
            "无法显示或填充导出配置页面"))
    {
        return false;
    }
    const std::vector<std::string> gisGroupChildren =
        runtime.childElementIdsById("export-gis-group-row-1");
    if (!postprocess_test::Require(
            std::ranges::find(gisGroupChildren, "export-gis-group-1") !=
                gisGroupChildren.end(),
            "GIS 输出组没有使用真实的多选框"))
    {
        return false;
    }
    lastChangeId.clear();
    sendMouse(runtime, skui::EventType::MouseDown, 724.F, 128.F);
    sendMouse(runtime, skui::EventType::MouseUp, 724.F, 128.F);
    if (!postprocess_test::Require(
            lastChangeId == "export-gis-group-1" && lastChangeValue == "1" &&
                !lastChangeChecked,
            "GIS 输出组多选框无法取消选中"))
    {
        return false;
    }
    sendMouse(runtime, skui::EventType::MouseDown, 724.F, 128.F);
    sendMouse(runtime, skui::EventType::MouseUp, 724.F, 128.F);
    if (!postprocess_test::Require(
            lastChangeId == "export-gis-group-1" && lastChangeChecked,
            "GIS 输出组多选框无法恢复选中"))
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
                    "<div class=\"task-detail\">卫星大图、GIS 大图 · 2026-08-10 10:00:00"
                    "</div><div class=\"task-output\">cache/高德地图；"
                    "cache/高德地图/gis数据/输出</div></div><div>"
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
