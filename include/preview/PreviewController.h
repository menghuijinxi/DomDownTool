#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>
#include <imm.h>

#include "TianDiTuMultidateDown.h"
#include "KmlDownloadRange.h"
#include "preview/AmapPlaceSearch.h"
#include "preview/AmapWebViewSearch.h"
#include "preview/Dx12FbxRenderer.h"
#include "preview/ExportTaskManager.h"
#include "preview/MapHoverRefreshThrottle.h"
#include "preview/MapRenderer.h"
#include "preview/DownloadRangeEditor.h"
#include "preview/TianDiTuMultidateWebViewSession.h"
#include "preview/WaybackCatalog.h"

class SkCanvas;

namespace skui
{
class Runtime;
struct ElementEvent;
}

namespace DomDownToolPreview
{
class PreviewController
{
  public:
    explicit PreviewController(WaybackCatalog::Fetch waybackFetch = {});

    void attachRuntime(skui::Runtime& runtime);

    void resizeRuntime(skui::Runtime& runtime);

    void draw(SkCanvas& canvas, int width, int height, float dpiScale);

    bool handleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                             skui::Runtime& runtime);

  private:
    enum class Page
    {
        Preview,
        Export,
        Tasks,
        FbxPreview
    };

    enum class SearchState
    {
        Idle,
        Loading,
        Results,
        Empty,
        Error
    };

    enum class FbxDragMode
    {
        None,
        Orbit,
        Pan
    };

    struct PendingLocationRequest
    {
        uint64_t requestId = 0;
        std::string resultId;
        std::string resultName;
    };

    struct ExportGisUiGroup
    {
        uint64_t id = 0;
        std::set<DownTool::AmapVectorRenderCategory> categories;
        bool selected = false;
    };

    [[nodiscard]] bool isMapPoint(HWND window, int x, int y) const;

    [[nodiscard]] bool isFbxPreviewPoint(HWND window, int x, int y) const;

    [[nodiscard]] bool isSearchInteractionPoint(const skui::Runtime& runtime, int x, int y) const;

    [[nodiscard]] bool isGisOverlayInteractionPoint(const skui::Runtime& runtime, int x,
                                                     int y) const;

    [[nodiscard]] bool isGisColorInteractionPoint(const skui::Runtime& runtime, int x,
                                                   int y) const;

    void submitPlaceSearch(skui::Runtime& runtime);

    void clearPlaceSearch(skui::Runtime& runtime);

    void selectPlaceSearchResult(skui::Runtime& runtime, size_t index);

    void moveSearchHighlight(skui::Runtime& runtime, int direction);

    void processPlaceSearchResponses(skui::Runtime& runtime);

    void updateSearchUi(skui::Runtime& runtime);

    void LoginTianDiTuMultidate(skui::Runtime& runtime,
                                std::string_view preferredYear = {});

    void RefreshTianDiTuMultidateTimelines(skui::Runtime& runtime,
                                           std::string_view preferredYear = {});

    void SelectTianDiTuMultidateTimeline(skui::Runtime& runtime,
                                         std::string_view value);

    [[nodiscard]] std::string GetSelectedTianDiTuMultidateYear() const;

    [[nodiscard]] std::string GetSelectedExportTianDiTuMultidateYear() const;

    void EnsureExportTianDiTuMultidateYears(skui::Runtime& runtime);

    void RefreshTianDiTuMultidateAfterViewChange(skui::Runtime& runtime,
                                                 std::string_view preferredYear);

    void UpdateTianDiTuMultidateUi(skui::Runtime& runtime,
                                   const MapSourceSummary& source);

    void RefreshWaybackCatalog(skui::Runtime& runtime);
    void ProcessWaybackCatalog();
    void SelectWaybackRelease(skui::Runtime& runtime, std::string_view number);
    void UpdateWaybackUi(skui::Runtime& runtime, const MapSourceSummary& source);

    [[nodiscard]] bool DownloadTianDiTuMultidateTile(
        const DownTool::TileHttpRequest& request,
        const std::filesystem::path& partialPath);

    void updateUi(skui::Runtime& runtime);

    [[nodiscard]] std::string buildFbxPreviewStatsText() const;

    void updateFbxPreviewFrameStatsUi(skui::Runtime& runtime);

    void schedulePageChange(Page page, skui::Runtime& runtime);

    void OpenExportPage(skui::Runtime& runtime);

    void OpenTaskPage(skui::Runtime& runtime);

    void OpenPreviewPage(skui::Runtime& runtime);

    void OpenFbxPreviewPage(skui::Runtime& runtime,
                            const std::filesystem::path& taskRoot,
                            std::string title);

    void UpdateExportUi(skui::Runtime& runtime);

    void ResetExportComposer(const ExportConfiguration* configuration);

    void RenderExportComposer(skui::Runtime& runtime);

    bool HandleExportComposerEvent(skui::Runtime& runtime, std::string_view id,
                                   std::string_view value, bool checked, bool fromClick,
                                   bool fromChange);

    [[nodiscard]] std::optional<std::filesystem::path> SelectExportDirectory() const;

    [[nodiscard]] std::optional<std::filesystem::path> SelectKmlFile() const;

    bool LoadKmlRange(const std::filesystem::path& path, skui::Runtime& runtime);

    void ApplyKmlRange(const DownTool::KmlDownloadRange& range,
                       skui::Runtime& runtime);

    void ClearKmlRange(skui::Runtime& runtime);

    [[nodiscard]] std::vector<ExportContentGroup> ResolveExportGroups(
        std::string& error) const;

    void UpdateTaskUi(skui::Runtime& runtime);

    void SaveTaskName(skui::Runtime& runtime);

    void setExportGisGroupSelected(uint64_t groupId, bool selected);

    [[nodiscard]] bool hasSelectedBuildingExportGroup() const;

    void SubmitExport(skui::Runtime& runtime);

    void selectGisColorCategory(GisOverlayCategory category);

    void applyGisColorPicker(skui::Runtime& runtime);

    void setGisColorEyedropperActive(bool active);

    bool sampleGisColorAtScreenCursor(skui::Runtime& runtime);

    void submitCenterCoordinate(skui::Runtime& runtime, std::string_view value);

    void CommitViewSpanInput(skui::Runtime& runtime, std::string_view value);

    void scheduleImmediateMapRedraw(HWND window);

    void scheduleImmediateFbxRedraw(HWND window, bool throttle = true);

    [[nodiscard]] bool hasFbxKeyboardPanDirection() const;

    void setFbxKeyboardPanDirection(HWND window, int rightDirection,
                                    int forwardDirection, int verticalDirection);

    void applyFbxKeyboardPanTick();

    void scheduleFbxKeyboardPanFrame(HWND window);

    void stopFbxKeyboardPanFrame(HWND window);

    void startFbxPreviewFrameLoop(HWND window);

    void stopFbxPreviewFrameLoop(HWND window);

    void recordFbxPreviewFrame(skui::Runtime& runtime);

    void resetFbxPreviewFrameRate();

    void disableFbxInputMethod(HWND window);

    void restoreFbxInputMethod();

    void scheduleMapUiRefresh(HWND window);

    void updateMapInteractionUi(skui::Runtime& runtime);

    void updateTileDebugCursor(HWND window, const POINT& point);

    void ToggleTileDebug(skui::Runtime& runtime);

    [[nodiscard]] std::string BuildTileDebugHtml() const;

    [[nodiscard]] bool UpdateTileDebugUi(skui::Runtime& runtime);

    void applyFbxInteractionSettings(FbxPreviewInteractionSettings settings);

    bool updateFbxInteractionSetting(skui::Runtime& runtime, std::string_view id,
                                     std::string_view value, bool persist);

    void syncFbxInteractionSettingsUi(skui::Runtime& runtime);

    void scheduleUiRefresh();

    void scheduleSearchRefresh() const;

    void scheduleExportTaskRefresh();

    void requestRedraw() const;

protected:
    bool ImportShapefile(const std::filesystem::path& path, skui::Runtime& runtime,
                         std::string& error);
    void InitializeExportDraft(
        skui::Runtime& runtime, const ExportConfiguration* configuration = nullptr);
    [[nodiscard]] std::optional<ExportConfiguration> BuildExportConfiguration(
        std::string& error) const;
    bool HandleRangeEditorEvent(skui::Runtime& runtime, const skui::ElementEvent& event);
    bool HandleRangeEditorMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam, skui::Runtime& runtime);
    void UpdateRangeEditorUi(skui::Runtime& runtime);
    void SyncRangeEditor(skui::Runtime& runtime);
    void BeginExportRangeEditing(skui::Runtime& runtime);
    void FinishExportRangeEditing(skui::Runtime& runtime, bool apply);
    void CompleteRangeDrawing(skui::Runtime& runtime);
    void FocusDownloadRegion(const DownTool::DownloadRegion& region);
    bool ConvertRadiusToRegion(skui::Runtime& runtime);
    void CommitRangeRadiusInput(skui::Runtime& runtime);

private:
    void SyncShapefilePreview();
    void UpdateShapefilePreviewUi(skui::Runtime& runtime);
    void FocusShapefile(const ExportSourceEntry& source);
    bool HandleShapefilePreviewEvent(skui::Runtime& runtime, const skui::ElementEvent& event);

    std::atomic<HWND> window_{nullptr};
    skui::Runtime* runtime_ = nullptr;
    TianDiTuMultidateWebViewSession tianDiTuMultidateSession_;
    MapRenderer renderer_;
    AmapWebViewSearch webViewSearch_;
    AmapPlaceSearch placeSearch_;
    Page page_ = Page::Preview;
    std::vector<ExportGisUiGroup> exportGisGroups_;
    std::string previewRadiusValue_ = "0";
    ExportRangeMode previewRangeMode_ = ExportRangeMode::CenterRadius;
    DownTool::Wgs84Polygons previewPolygonsWgs84_;
    std::string previewKmlFileName_;
    ExportRangeMode exportRangeMode_ = ExportRangeMode::CenterRadius;
    DownTool::Wgs84Polygons exportPolygonsWgs84_;
    std::string exportKmlFileName_;
    std::string exportLongitudeValue_;
    std::string exportLatitudeValue_;
    std::string exportRadiusValue_ = "0";
    std::string exportZoomValue_;
    std::set<std::string> exportMapSourceIds_;
    std::map<DownTool::AmapVectorRenderCategory, DownTool::AmapVectorColor>
        exportGisColors_;
    std::string exportOutputValue_;
    std::string exportMergedFormat_ = "png";
    std::string exportMergedResolutionValue_ = "16384";
    std::string exportFbxSizeValue_ = "4096";
    std::string exportFbxTextureFormatValue_ = "png";
    std::string exportTerrainSampleRateValue_ = "30";
    std::string exportBuildingWallTexturePathValue_ = "贴图/墙体";
    std::string exportBuildingRoofTexturePathValue_ = "贴图/房顶";
    std::string exportBuildingWallTextureModeValue_ = "merged-atlas";
    std::string exportBuildingRoofTextureModeValue_ = "merged-atlas";
    std::string exportBuildingAtlasFormatValue_ = "jpg";
    std::string exportBuildingLayerHeightValue_ = "45";
    std::string exportBuildingMaxWallMaterialsValue_ = "1";
    std::string exportBuildingTextureSeedValue_ = "0";
    bool exportTiles_ = false;
    bool exportMergedImage_ = false;
    bool exportGisOverlayOnSatellite_ = false;
    bool exportGisLayersMerged_ = false;
    bool exportGisLayersSeparate_ = false;
    bool exportFbx_ = false;
    bool exportTerrainHeight_ = false;
    bool exportGeoJson_ = false;
    bool exportBuildingHeightColors_ = false;
    bool exportBuildingFbx_ = false;
    std::set<std::string> exportTianDiTuMultidateSelectedValues_ = {"latest"};
    std::string exportValidationMessage_;
    std::vector<ExportContentGroup> exportContentGroups_;
    std::vector<ExportSourceEntry> importedShapefileSources_;
    std::map<std::filesystem::path, DownTool::ShapefileInfo> importedShapefileInfo_;
    std::set<std::string> hiddenShapefileSourceIds_;
    std::string shapefilePreviewError_;
    std::string lastShapefilePreviewHtml_;
    std::string exportSelectedComponentId_;
    std::string exportComponentMenuGroupId_;
    std::string exportSourceMenuComponentId_;
    uint64_t exportComposerSerial_ = 1;
    std::string lastExportGroupListHtml_;
    std::string lastExportParamPanelHtml_;
    std::string lastExportParamPanelComponentId_;
    std::string lastTaskListHtml_;
    std::optional<MapRenderStats> scheduledStats_;
    std::string searchQuery_;
    std::string searchMessage_;
    std::vector<PlaceSearchResult> searchResults_;
    std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo> tianDiTuMultidateTimelines_;
    std::string tianDiTuMultidateCookie_;
    std::string tianDiTuMultidateRegionGbCode_;
    std::string tianDiTuMultidateRegionName_;
    std::string tianDiTuMultidateSelectedValue_ = "latest";
    std::string tianDiTuMultidateStatus_ = "默认显示最新影像；历史时相需要登录后刷新。";
    bool tianDiTuMultidateYearPickerActive_ = false;
    std::chrono::steady_clock::time_point tianDiTuMultidateYearPickerReleaseAt_{};
    std::mutex tianDiTuMultidateTileFetchMutex_;
    std::optional<bool> lastTianDiTuMultidatePanelVisible_;
    std::string lastTianDiTuMultidateSelectHtml_;
    std::string lastTianDiTuMultidateSelectedValue_;
    std::string lastTianDiTuMultidateStatus_;
    std::optional<bool> lastTianDiTuMultidateRefreshDisabled_;
    std::optional<bool> lastTianDiTuMultidateYearDisabled_;
    std::string lastExportTianDiTuMultidateYearsHtml_;
    std::optional<size_t> highlightedSearchResult_;
    std::optional<PendingLocationRequest> pendingLocationRequest_;
    SearchState searchState_ = SearchState::Idle;
    bool searchPopupOpen_ = false;
    bool gisOverlayPopupOpen_ = false;
    bool gisColorPopupOpen_ = false;
    GisOverlayCategory gisColorPaletteCategory_ = GisOverlayCategory::Road;
    double gisColorHue_ = 0.;
    double gisColorSaturation_ = 0.;
    double gisColorBrightness_ = 0.;
    bool gisColorFieldDragging_ = false;
    bool gisColorHueDragging_ = false;
    bool gisColorEyedropperActive_ = false;
    bool tileDebugEnabled_ = false;
    bool taskClearConfirmOpen_ = false;
    std::optional<ExportConfiguration> pendingExportConfiguration_;
    std::optional<std::string> editingTaskId_;
    std::string exportTaskTitleValue_;
    std::optional<std::string> taskRenameTargetId_;
    std::string taskRenameValue_;
    std::string taskRenameError_;
    std::optional<std::string> taskDeleteTargetId_;
    std::string taskDeleteTargetTitle_;
    std::string taskDeleteTargetPath_;
    std::optional<GisOverlayCategories> lastUiGisCategories_;
    std::array<std::string, 9> lastUiGisColorValues_{};
    std::string lastUiGisPickerState_;
    std::string lastUiTileStats_;
    std::string lastUiTileDebugHtml_;
    std::string lastCenterCoordinate_;
    std::string lastZoomValue_;
    std::optional<std::string> viewSpanInput_;
    FbxPreviewInteractionSettings fbxInteractionSettings_;
    bool dragging_ = false;
    FbxDragMode fbxDragMode_ = FbxDragMode::None;
    POINT lastDragPoint_{};
    bool immediateMapRedrawPosted_ = false;
    bool immediateFbxRedrawPosted_ = false;
    int fbxKeyboardRightDirection_ = 0;
    int fbxKeyboardForwardDirection_ = 0;
    int fbxKeyboardVerticalDirection_ = 0;
    std::chrono::steady_clock::time_point lastFbxKeyboardPanTick_{};
    std::chrono::steady_clock::time_point lastFbxKeyboardPanFrame_{};
    std::chrono::steady_clock::time_point lastFbxFrameTime_{};
    std::chrono::steady_clock::time_point fbxFrameRateWindowStart_{};
    uint32_t fbxFrameRateFrameCount_ = 0;
    std::string fbxFrameRateText_ = "FPS --";
    bool fbxKeyboardPanTimerActive_ = false;
    bool fbxFrameTimerActive_ = false;
    bool fbxInputMethodDisabled_ = false;
    bool fileDropEnabled_ = false;
    HWND fbxInputMethodWindow_ = nullptr;
    HIMC fbxInputMethodContext_ = nullptr;
    bool mapUiRefreshPosted_ = false;
    std::optional<POINT> tileDebugCursor_;
    std::optional<bool> lastUiTileDebugVisible_;
    std::optional<bool> lastUiTileDebugPressed_;
    mutable std::atomic_bool fbxRedrawPosted_ = false;
    MapHoverRefreshThrottle mapDragRedrawThrottle_{std::chrono::milliseconds(4)};
    MapHoverRefreshThrottle fbxDragRedrawThrottle_{std::chrono::milliseconds(4)};
    MapHoverRefreshThrottle mapCenterUiRefreshThrottle_{std::chrono::milliseconds(50)};
    MapHoverRefreshThrottle tileDebugUiRefreshThrottle_{std::chrono::milliseconds(80)};
    std::atomic_bool exportTaskRefreshPosted_ = false;
    // 必须先于任务状态回调所访问的成员析构。
    ExportTaskManager exportTaskManager_;
    Dx12FbxRenderer dx12FbxRenderer_;
    std::string fbxPreviewTitle_;
    std::filesystem::path fbxPreviewTaskRoot_;

protected:
    enum class RangeDrawShape
    {
        Select,
        Polygon,
        Rectangle
    };
    DownloadRangeEditor rangeEditor_;
    DownTool::DownloadRegions exportDownloadRegions_;
    bool rangePanelOpen_ = false;
    bool editingExportRange_ = false;
    bool rangeMapKeyboardActive_ = false;
    RangeDrawShape rangeDrawShape_ = RangeDrawShape::Select;
    DownTool::DownloadRegionRole rangeDrawRole_ = DownTool::DownloadRegionRole::Include;
    DownTool::Wgs84Ring rangeDraft_;
    DownTool::Wgs84Ring rangeDragVertices_;
    std::optional<DownTool::LonLat> rangeRectangleAnchor_;
    std::optional<size_t> rangeDraggingVertex_;
    std::optional<size_t> rangeSelectedVertex_;
    bool rangePanning_ = false;
    POINT rangePanPoint_{};
    std::string rangeEditorMessage_;
    std::optional<std::string> rangeRadiusInput_;
    std::string lastRangeListHtml_;
    DownTool::DownloadRegions rangePreviewBackup_;
    ExportRangeMode rangePreviewModeBackup_ = ExportRangeMode::CenterRadius;
    std::string rangePreviewRadiusBackup_;
    std::string rangePreviewKmlBackup_;
    DownTool::LonLat rangePreviewCenterBackup_;
    double rangePreviewZoomBackup_ = 15.;

private:
    std::vector<DownTool::WaybackRelease> waybackReleases_;
    std::string waybackSelectedNumber_;
    std::string waybackStatus_;
    std::string lastWaybackSelectHtml_;
    std::string lastWaybackSelectedNumber_;
    bool waybackCatalogRequested_ = false;
    // 最先析构，保证异步完成回调执行时控制器的其余成员仍然存在。
    WaybackCatalog waybackCatalog_;
};
} // namespace DomDownToolPreview
