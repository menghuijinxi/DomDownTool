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

    std::string lastInputId;
    std::string lastInputValue;
    std::string lastMouseDownId;
    std::string lastClickId;
    std::string lastChangeId;
    std::string lastChangeValue;
    bool lastChangeChecked = false;
    bool gisPanelOpen = false;
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
            else if (event.type == skui::ElementEventType::Click)
            {
                lastClickId = event.id;
                if (event.id == "gis-overlay-toggle")
                {
                    gisPanelOpen = !gisPanelOpen;
                    runtime.setVisibleById("gis-overlay-panel", gisPanelOpen);
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

    std::cout << "PreviewSearchUiTest passed: " << outputPath << '\n';
    return true;
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(runTest);
}
