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
    const std::filesystem::path controllerPath =
        sourceRoot / "src" / "preview" / "PreviewController.cpp";
    skui::RuntimeOptions options;
    options.assetRoot = documentPath.parent_path().generic_string();
    skui::Runtime runtime(std::move(options));
    runtime.resize(kWidth, kHeight, 1.F);
    if (!postprocess_test::Require(runtime.loadDocument(documentPath.generic_string()),
                                   "无法加载预览 HTML：" + runtime.lastError()))
    {
        return false;
    }
    const std::array<std::string_view, 26> exportCheckboxIds = {
        "export-provider-gaode", "export-provider-tencent",
        "export-provider-tianditu", "export-provider-baidu",
        "export-provider-google", "export-provider-bing", "export-provider-arcgis",
        "export-gis-group-1", "export-gis-group-2", "export-gis-group-3",
        "export-gis-group-4", "export-gis-group-5", "export-gis-group-6",
        "export-gis-group-7", "export-gis-group-8", "export-gis-group-9",
        "export-gis-combine-overlay", "export-gis-combine-merged",
        "export-gis-combine-separate", "export-satellite-tiles",
        "export-satellite-merged", "export-fbx", "export-terrain-height", "export-geojson",
        "export-building-height-colors", "export-building-fbx"};
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
            postprocess_test::ContainsText(
                documentPath, "fill=\"none\" stroke=\"#c9cfcc\" stroke-width=\"1.6\""),
            "输出目录按钮的 SVG 没有显式设置透明填充和浅色描边"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(
                documentPath, "<option value=\"tianditu.multidate\">天地图多时相</option>") &&
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
                                               "历史时相需要登录后刷新"),
            "预览页缺少天地图多时相入口或登录刷新控件"))
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
                    controllerPath, "RefreshTianDiTuMultidateAfterViewChange"),
            "天地图多时相年份下拉框不应在普通 UI 刷新时反复重建"))
    {
        return false;
    }
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(documentPath, ".param-row") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: repeat(auto-fit, minmax(250px, 1fr))") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: minmax(0, 1fr)") &&
                postprocess_test::ContainsText(documentPath, ".param-field select") &&
                postprocess_test::ContainsText(
                    documentPath, "id=\"export-fbx-texture-format\"") &&
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
                runtime.setValueById("export-config-radius", "2000") &&
                runtime.setValueById("export-terrain-sample-rate", "30"),
            "预览页或导出配置页缺少半径输入"))
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
                postprocess_test::ContainsText(controllerPath,
                                               "open-export-task-config") &&
                postprocess_test::ContainsText(controllerPath, "task-resume-") &&
                postprocess_test::ContainsText(controllerPath, "resume-export-task") &&
                postprocess_test::ContainsText(controllerPath, "taskDeclaresFbxOutput") &&
                postprocess_test::ContainsText(controllerPath,
                                               "record.detail.find(\"FBX\")") &&
                postprocess_test::ContainsText(documentPath, "task-reopen-config") &&
                postprocess_test::ContainsText(
                    documentPath,
                    "grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr) 150px 200px;") &&
                postprocess_test::ContainsText(documentPath, "min-width: 200px;"),
            "导出任务缺少按当前参数重新打开配置的按钮结构"))
    {
        return false;
    }

    const std::vector<std::string> exportHeaderChildren =
        runtime.childElementIdsById("export-page-header");
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

    const std::vector<std::string> expectedInitialGisItems = {
        "export-gis-static-road", "export-gis-static-building",
        "export-gis-static-water", "export-gis-static-point",
        "export-gis-static-green", "export-gis-static-boundary",
        "export-gis-static-highway", "export-gis-static-other",
        "export-gis-static-subway"};
    const std::vector<std::string> expectedGisCombinationItems = {
        "export-gis-combine-overlay-item", "export-gis-combine-merged-item",
        "export-gis-combine-separate-item"};
    const std::vector<std::string> expectedBuildingFbxFields = {
        "export-building-wall-textures", "export-building-roof-textures",
        "export-building-wall-texture-mode", "export-building-roof-texture-mode",
        "export-building-atlas-format", "export-building-layer-height",
        "export-building-max-wall-materials", "export-building-texture-seed"};
    if (!postprocess_test::Require(
            runtime.childElementIdsById("export-gis-groups") == expectedInitialGisItems,
            "GIS 数据初始结构没有使用单层两列网格") ||
        !postprocess_test::Require(
            runtime.childElementIdsById("export-gis-combine-options") ==
                expectedGisCombinationItems,
            "GIS 组合方式初始结构不是三个独立复选项") ||
        !postprocess_test::Require(
            postprocess_test::ContainsText(documentPath, "id=\"export-building-fbx\"") &&
                !postprocess_test::ContainsText(
                    documentPath,
                    "id=\"export-building-fbx\" type=\"checkbox\" disabled") &&
                std::ranges::all_of(
                    expectedBuildingFbxFields,
                    [&documentPath](const std::string& id)
                    {
                        return postprocess_test::ContainsText(
                            documentPath,
                            "id=\"" + id + "\"");
                    }),
            "建筑 FBX 配置区缺少墙体/屋顶贴图模式控件"))
    {
        return false;
    }
    const std::filesystem::path exportManagerPath =
        sourceRoot / "src" / "preview" / "ExportTaskManager.cpp";
    if (!postprocess_test::Require(
            postprocess_test::ContainsText(exportManagerPath, "RunBuildingFbxExport") &&
                postprocess_test::ContainsText(exportManagerPath, "BuildingFbxModeName") &&
                postprocess_test::ContainsText(exportManagerPath,
                                               "ExportAmapBuildingsToFbx"),
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
        !postprocess_test::Require(runtime.setValueById("export-zoom", "15"),
                                   "无法填充导出缩放级别") ||
        !postprocess_test::Require(runtime.setValueById("export-output", ""),
                                   "无法清空导出目录") ||
        !postprocess_test::Require(runtime.setAttributeById("export-gis-group-1", "checked", ""),
                                   "无法勾选导出 GIS 道路项") ||
        !postprocess_test::Require(runtime.setAttributeById("export-gis-group-2", "checked", ""),
                                   "无法勾选导出 GIS 建筑项") ||
        !postprocess_test::Require(runtime.setAttributeById("export-gis-group-3", "checked", ""),
                                   "无法预置导出 GIS 水系项") ||
        !postprocess_test::Require(runtime.removeAttributeById("export-gis-group-3", "checked"),
                                   "无法取消导出 GIS 水系项"))
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
