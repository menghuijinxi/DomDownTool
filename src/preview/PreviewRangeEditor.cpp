#include "preview/PreviewController.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>

#include "fmt/format.h"
#include "skui_runtime.h"

namespace
{
std::string EscapeRegionText(const std::string& text)
{
    std::string escaped;
    for (const char character : text)
    {
        switch (character)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

double DistanceSquared(const DomDownToolPreview::ViewportPoint& point, double x, double y)
{
    return (point.x - x) * (point.x - x) + (point.y - y) * (point.y - y);
}

double SegmentDistanceSquared(const DomDownToolPreview::ViewportPoint& start,
                              const DomDownToolPreview::ViewportPoint& end, double x, double y)
{
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double length = dx * dx + dy * dy;
    const double parameter =
        length > 0. ? std::clamp(((x - start.x) * dx + (y - start.y) * dy) / length, 0., 1.) : 0.;
    return DistanceSquared({start.x + parameter * dx, start.y + parameter * dy}, x, y);
}

void SetControlEnabled(skui::Runtime& runtime, const char* id, bool enabled)
{
    if (enabled)
    {
        runtime.removeAttributeById(id, "disabled");
    }
    else
    {
        runtime.setAttributeById(id, "disabled", "");
    }
}
} // namespace

void DomDownToolPreview::PreviewController::FocusDownloadRegion(
    const DownTool::DownloadRegion& region)
{
    const auto bounds = DownTool::CalculateGeographicBounds({region.GetVertices()});
    if (bounds)
    {
        const auto center = DownTool::GeographicBoundsCenter(*bounds);
        const double span =
            std::max(bounds->east - bounds->west, (bounds->north - bounds->south) * 1.5);
        renderer_.setView(center, std::clamp(std::log2(180. / std::max(span, 0.0001)), 1., 20.));
    }
}

void DomDownToolPreview::PreviewController::SyncRangeEditor(skui::Runtime& runtime)
{
    renderer_.SetExportRadiusMeters(previewRangeMode_ == ExportRangeMode::CenterRadius
                                        ? std::strtod(previewRadiusValue_.c_str(), nullptr)
                                        : 0.);
    previewPolygonsWgs84_ = DownTool::GetRegionPolygons(rangeEditor_.GetRegions(),
                                                        DownTool::DownloadRegionRole::Include);
    renderer_.SetExportPolygonsWgs84(previewRangeMode_ == ExportRangeMode::CustomPolygon
                                         ? previewPolygonsWgs84_
                                         : DownTool::Wgs84Polygons{});
    renderer_.SetDownloadRegions(rangeEditor_.GetRegions(),
                                 rangePanelOpen_ ? rangeEditor_.GetSelectedId() : std::string{});
    renderer_.SetDraftRegion(rangeDraft_,
                             rangeDrawRole_ == DownTool::DownloadRegionRole::ExcludeGis,
                             rangeDrawShape_ == RangeDrawShape::Rectangle);
    UpdateRangeEditorUi(runtime);
    requestRedraw();
}

bool DomDownToolPreview::PreviewController::ConvertRadiusToRegion(skui::Runtime& runtime)
{
    if (previewRangeMode_ == ExportRangeMode::CustomPolygon)
    {
        return true;
    }
    if (!DownTool::GetRegionPolygons(rangeEditor_.GetRegions(),
                                     DownTool::DownloadRegionRole::Include)
             .empty())
    {
        previewRangeMode_ = ExportRangeMode::CustomPolygon;
        SyncRangeEditor(runtime);
        updateUi(runtime);
        return true;
    }
    const auto bounds =
        calculateExportGeoBounds(renderer_.center(), renderer_.GetExportRadiusMeters());
    if (bounds)
    {
        const DownTool::Wgs84Ring vertices = {{bounds->minimumLongitude, bounds->maximumLatitude},
                                              {bounds->maximumLongitude, bounds->maximumLatitude},
                                              {bounds->maximumLongitude, bounds->minimumLatitude},
                                              {bounds->minimumLongitude, bounds->minimumLatitude}};
        if (!rangeEditor_.AddRegion(vertices, DownTool::DownloadRegionRole::Include,
                                    rangeEditorMessage_))
        {
            UpdateRangeEditorUi(runtime);
            return false;
        }
    }
    previewRangeMode_ = ExportRangeMode::CustomPolygon;
    previewKmlFileName_.clear();
    SyncRangeEditor(runtime);
    updateUi(runtime);
    return true;
}

void DomDownToolPreview::PreviewController::UpdateRangeEditorUi(skui::Runtime& runtime)
{
    runtime.setVisibleById("range-editor-panel", rangePanelOpen_ && page_ == Page::Preview);
    runtime.setAttributeById("preview-edit-range", "aria-pressed",
                             rangePanelOpen_ ? "true" : "false");
    runtime.setVisibleById("range-return-export", editingExportRange_);
    runtime.setVisibleById("range-cancel-export", editingExportRange_);
    const bool custom = previewRangeMode_ == ExportRangeMode::CustomPolygon;
    runtime.setAttributeById("range-mode-radius", "aria-pressed", custom ? "false" : "true");
    runtime.setAttributeById("range-mode-custom", "aria-pressed", custom ? "true" : "false");
    runtime.setAttributeById("range-role-include", "aria-pressed",
                             rangeDrawRole_ == DownTool::DownloadRegionRole::Include ? "true"
                                                                                     : "false");
    runtime.setAttributeById("range-role-exclude", "aria-pressed",
                             rangeDrawRole_ == DownTool::DownloadRegionRole::ExcludeGis ? "true"
                                                                                        : "false");
    runtime.setAttributeById("range-tool-select", "aria-pressed",
                             rangeDrawShape_ == RangeDrawShape::Select ? "true" : "false");
    runtime.setAttributeById("range-tool-polygon", "aria-pressed",
                             rangeDrawShape_ == RangeDrawShape::Polygon ? "true" : "false");
    runtime.setAttributeById("range-tool-rectangle", "aria-pressed",
                             rangeDrawShape_ == RangeDrawShape::Rectangle ? "true" : "false");
    SetControlEnabled(runtime, "range-undo", rangeEditor_.GetCanUndo() || !rangeDraft_.empty());
    SetControlEnabled(runtime, "range-redo", rangeEditor_.GetCanRedo());
    SetControlEnabled(runtime, "range-complete", rangeDraft_.size() >= 3);
    SetControlEnabled(runtime, "range-delete", rangeEditor_.GetSelectedRegion() != nullptr);
    const auto includes = DownTool::GetRegionPolygons(rangeEditor_.GetRegions(),
                                                      DownTool::DownloadRegionRole::Include);
    const auto excludes = DownTool::GetRegionPolygons(rangeEditor_.GetRegions(),
                                                      DownTool::DownloadRegionRole::ExcludeGis);
    runtime.setTextById(
        "range-summary",
        fmt::format("下载区 {} · GIS 排除区 {}", custom ? includes.size() : 0, excludes.size()));
    runtime.setTextById("preview-kml-name",
                        fmt::format("下载区 {} · 排除区 {}", includes.size(), excludes.size()));
    runtime.setTextById("range-editor-message", rangeEditorMessage_);
    const auto* selected = rangeEditor_.GetSelectedRegion();
    runtime.setVisibleById("range-selected-fields", selected != nullptr);
    if (selected)
    {
        runtime.setValueById("range-region-name", selected->GetName());
    }
    std::string list = "<div id=\"range-region-list\">";
    for (const auto role :
         {DownTool::DownloadRegionRole::Include, DownTool::DownloadRegionRole::ExcludeGis})
    {
        list += role == DownTool::DownloadRegionRole::Include
                    ? "<div class=\"range-section-label\">下载区域</div>"
                    : "<div class=\"range-section-label\">GIS 排除区域</div>";
        for (const auto& region : rangeEditor_.GetRegions())
        {
            if (region.GetRole() != role)
            {
                continue;
            }
            const std::string id = EscapeRegionText(region.GetId());
            list += fmt::format(
                "<div class=\"range-region-row{}\"><input id=\"range-enable-{}\" "
                "type=\"checkbox\" {} aria-label=\"启用 {}\">"
                "<button id=\"range-select-{}\" type=\"button\" class=\"range-region-name\" "
                "data-action=\"edit-download-range\" title=\"{}\">{}</button>"
                "<button id=\"range-focus-{}\" type=\"button\" class=\"range-icon\" "
                "data-action=\"edit-download-range\" title=\"定位区域\" "
                "aria-label=\"定位区域\">&#8982;</button></div>",
                region.GetId() == rangeEditor_.GetSelectedId() ? " is-selected" : "", id,
                region.GetEnabled() ? "checked" : "", EscapeRegionText(region.GetName()), id,
                EscapeRegionText(region.GetName()), EscapeRegionText(region.GetName()), id);
        }
    }
    list += "</div>";
    if (list != lastRangeListHtml_)
    {
        runtime.replaceHtmlById("range-region-list", list);
        lastRangeListHtml_ = std::move(list);
    }
}

void DomDownToolPreview::PreviewController::CompleteRangeDrawing(skui::Runtime& runtime)
{
    if (rangeDraft_.size() < 3)
    {
        rangeEditorMessage_ = "区域至少需要三个顶点。";
    }
    else if (rangeEditor_.AddRegion(rangeDraft_, rangeDrawRole_, rangeEditorMessage_))
    {
        rangeDraft_.clear();
        rangeRectangleAnchor_.reset();
        rangeSelectedVertex_.reset();
        rangeEditorMessage_.clear();
    }
    SyncRangeEditor(runtime);
}

void DomDownToolPreview::PreviewController::BeginExportRangeEditing(skui::Runtime& runtime)
{
    rangePreviewBackup_ = rangeEditor_.GetRegions();
    rangePreviewModeBackup_ = previewRangeMode_;
    rangePreviewRadiusBackup_ = previewRadiusValue_;
    rangePreviewKmlBackup_ = previewKmlFileName_;
    rangePreviewCenterBackup_ = renderer_.center();
    rangePreviewZoomBackup_ = renderer_.zoom();
    rangeEditor_.SetRegions(exportDownloadRegions_);
    previewRangeMode_ = exportRangeMode_;
    previewRadiusValue_ = exportRadiusValue_;
    previewKmlFileName_ = exportKmlFileName_;
    runtime.setValueById("export-radius", previewRadiusValue_);
    const double radius = std::strtod(exportRadiusValue_.c_str(), nullptr);
    renderer_.SetExportRadiusMeters(radius);
    renderer_.setView({std::strtod(exportLongitudeValue_.c_str(), nullptr),
                       std::strtod(exportLatitudeValue_.c_str(), nullptr)},
                      std::strtod(exportZoomValue_.c_str(), nullptr));
    rangeDraft_.clear();
    rangeEditorMessage_.clear();
    rangeDrawShape_ = RangeDrawShape::Select;
    editingExportRange_ = true;
    rangePanelOpen_ = true;
    page_ = Page::Preview;
    SyncRangeEditor(runtime);
    updateUi(runtime);
}

void DomDownToolPreview::PreviewController::FinishExportRangeEditing(skui::Runtime& runtime,
                                                                     bool apply)
{
    if (apply && !rangeDraft_.empty())
    {
        rangeEditorMessage_ = "请先完成或取消正在绘制的区域。";
        UpdateRangeEditorUi(runtime);
        return;
    }
    if (apply)
    {
        exportDownloadRegions_ = rangeEditor_.GetRegions();
        exportRangeMode_ = previewRangeMode_;
        exportPolygonsWgs84_ = DownTool::GetRegionPolygons(exportDownloadRegions_,
                                                           DownTool::DownloadRegionRole::Include);
        exportKmlFileName_ = previewKmlFileName_;
        exportRadiusValue_ = previewRadiusValue_;
        if (exportRangeMode_ == ExportRangeMode::CenterRadius)
        {
            exportLongitudeValue_ = fmt::format("{:.6f}", renderer_.center().lon);
            exportLatitudeValue_ = fmt::format("{:.6f}", renderer_.center().lat);
        }
        runtime.setValueById("export-lon", exportLongitudeValue_);
        runtime.setValueById("export-lat", exportLatitudeValue_);
        runtime.setValueById("export-config-radius", exportRadiusValue_);
    }
    rangeEditor_.SetRegions(rangePreviewBackup_);
    previewRangeMode_ = rangePreviewModeBackup_;
    previewRadiusValue_ = rangePreviewRadiusBackup_;
    previewKmlFileName_ = rangePreviewKmlBackup_;
    runtime.setValueById("export-radius", previewRadiusValue_);
    renderer_.SetExportRadiusMeters(std::strtod(previewRadiusValue_.c_str(), nullptr));
    renderer_.setView(rangePreviewCenterBackup_, rangePreviewZoomBackup_);
    editingExportRange_ = false;
    rangePanelOpen_ = false;
    rangeDraft_.clear();
    rangeDrawShape_ = RangeDrawShape::Select;
    renderer_.SetDraftRegion({}, false, false);
    page_ = Page::Export;
    SyncRangeEditor(runtime);
    updateUi(runtime);
}

bool DomDownToolPreview::PreviewController::HandleRangeEditorEvent(skui::Runtime& runtime,
                                                                   const skui::ElementEvent& event)
{
    if (event.type == skui::ElementEventType::MouseDown && !event.id.empty())
    {
        rangeMapKeyboardActive_ = false;
    }
    const bool click = event.type == skui::ElementEventType::Click;
    const bool change = event.type == skui::ElementEventType::Change;
    if (event.id == "export-edit-range" && click)
    {
        BeginExportRangeEditing(runtime);
        return true;
    }
    if (event.id == "preview-edit-range" && click)
    {
        rangePanelOpen_ = !rangePanelOpen_;
        if (!rangePanelOpen_)
        {
            rangeDraft_.clear();
            rangeDrawShape_ = RangeDrawShape::Select;
        }
        SyncRangeEditor(runtime);
        return true;
    }
    const bool rename =
        event.id == "range-region-name" && event.type == skui::ElementEventType::Input;
    if (!event.id.starts_with("range-") || (!click && !change && !rename))
    {
        return false;
    }
    rangeEditorMessage_.clear();
    if (event.id == "range-return-export")
    {
        FinishExportRangeEditing(runtime, true);
        return true;
    }
    if (event.id == "range-cancel-export")
    {
        FinishExportRangeEditing(runtime, false);
        return true;
    }
    if (event.id == "range-close")
    {
        if (editingExportRange_)
        {
            FinishExportRangeEditing(runtime, true);
            return true;
        }
        rangePanelOpen_ = false;
        rangeDraft_.clear();
        rangeDrawShape_ = RangeDrawShape::Select;
    }
    else if (event.id == "range-mode-radius")
    {
        previewRangeMode_ = ExportRangeMode::CenterRadius;
        rangeDraft_.clear();
        rangeDrawShape_ = RangeDrawShape::Select;
        updateUi(runtime);
    }
    else if (event.id == "range-mode-custom")
    {
        ConvertRadiusToRegion(runtime);
    }
    else if (event.id == "range-role-include" || event.id == "range-role-exclude")
    {
        rangeDrawRole_ = event.id == "range-role-include"
                             ? DownTool::DownloadRegionRole::Include
                             : DownTool::DownloadRegionRole::ExcludeGis;
        rangeDraft_.clear();
        rangeRectangleAnchor_.reset();
    }
    else if (event.id == "range-tool-select")
    {
        rangeDrawShape_ = RangeDrawShape::Select;
        rangeDraft_.clear();
        rangeRectangleAnchor_.reset();
    }
    else if (event.id == "range-tool-polygon" || event.id == "range-tool-rectangle")
    {
        if (ConvertRadiusToRegion(runtime))
        {
            rangeDrawShape_ = event.id == "range-tool-polygon" ? RangeDrawShape::Polygon
                                                               : RangeDrawShape::Rectangle;
            rangeDraft_.clear();
            rangeRectangleAnchor_.reset();
        }
    }
    else if (event.id == "range-complete")
    {
        CompleteRangeDrawing(runtime);
    }
    else if (event.id == "range-import-kml")
    {
        if (const auto path = SelectKmlFile())
        {
            LoadKmlRange(*path, runtime);
        }
    }
    else if (event.id == "range-undo")
    {
        if (!rangeDraft_.empty())
        {
            rangeDraft_.pop_back();
            rangeRectangleAnchor_.reset();
        }
        else
        {
            rangeEditor_.Undo();
        }
    }
    else if (event.id == "range-redo")
    {
        rangeEditor_.Redo();
    }
    else if (event.id == "range-delete")
    {
        rangeEditor_.RemoveSelected(rangeEditorMessage_);
    }
    else if (event.id == "range-region-name" && (change || rename))
    {
        rangeEditor_.SetSelectedName(event.value, rangeEditorMessage_);
    }
    else if (event.id.starts_with("range-select-") || event.id.starts_with("range-focus-"))
    {
        const bool focus = event.id.starts_with("range-focus-");
        rangeEditor_.SetSelectedId(event.id.substr(focus ? 12 : 13));
        rangeDrawShape_ = RangeDrawShape::Select;
        rangeDraft_.clear();
        rangeSelectedVertex_.reset();
        if (focus && rangeEditor_.GetSelectedRegion())
        {
            FocusDownloadRegion(*rangeEditor_.GetSelectedRegion());
        }
    }
    else if (event.id.starts_with("range-enable-") && change)
    {
        rangeEditor_.SetSelectedId(event.id.substr(13));
        rangeEditor_.SetSelectedEnabled(event.checked, rangeEditorMessage_);
    }
    SyncRangeEditor(runtime);
    return true;
}

bool DomDownToolPreview::PreviewController::HandleRangeEditorMessage(HWND window, UINT message,
                                                                     WPARAM wParam, LPARAM lParam,
                                                                     skui::Runtime& runtime)
{
    if (!rangePanelOpen_)
    {
        return false;
    }
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED)
    {
        if (rangeRectangleAnchor_)
        {
            rangeDraft_.clear();
        }
        rangeDraggingVertex_.reset();
        rangeRectangleAnchor_.reset();
        rangePanning_ = false;
        rangeDragVertices_.clear();
        renderer_.SetDraftRegion({}, false, false);
        requestRedraw();
        return false;
    }
    if (message == WM_KEYDOWN &&
        (rangeMapKeyboardActive_ ||
         (wParam == VK_ESCAPE && rangeDrawShape_ != RangeDrawShape::Select)))
    {
        const bool undoOrRedo =
            (GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'Z' || wParam == 'Y');
        if (wParam == VK_ESCAPE || wParam == VK_RETURN || wParam == VK_DELETE ||
            wParam == VK_BACK || undoOrRedo)
        {
            rangeDraggingVertex_.reset();
            rangeDragVertices_.clear();
            rangeRectangleAnchor_.reset();
            rangePanning_ = false;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
        }
        if (wParam == VK_ESCAPE)
        {
            rangeDraft_.clear();
            rangeRectangleAnchor_.reset();
            rangeDrawShape_ = RangeDrawShape::Select;
        }
        else if (wParam == VK_RETURN && !rangeDraft_.empty())
        {
            CompleteRangeDrawing(runtime);
        }
        else if (undoOrRedo)
        {
            if (wParam == 'Z')
            {
                if (!rangeDraft_.empty())
                {
                    rangeDraft_.pop_back();
                }
                else
                {
                    rangeEditor_.Undo();
                }
            }
            else
            {
                rangeDraft_.clear();
                rangeEditor_.Redo();
            }
        }
        else if (wParam == VK_BACK && !rangeDraft_.empty())
        {
            rangeDraft_.pop_back();
        }
        else if (wParam == VK_DELETE && rangeEditor_.GetSelectedRegion())
        {
            auto vertices = rangeEditor_.GetSelectedRegion()->GetVertices();
            if (rangeSelectedVertex_ && *rangeSelectedVertex_ < vertices.size())
            {
                vertices.erase(vertices.begin() + *rangeSelectedVertex_);
                rangeEditor_.SetSelectedVertices(vertices, rangeEditorMessage_);
            }
            else
            {
                rangeEditor_.RemoveSelected(rangeEditorMessage_);
            }
            rangeSelectedVertex_.reset();
        }
        else
        {
            return false;
        }
        SyncRangeEditor(runtime);
        return true;
    }
    const double x = static_cast<short>(LOWORD(lParam));
    const double y = static_cast<short>(HIWORD(lParam));
    const bool mapPoint = isMapPoint(window, static_cast<int>(x), static_cast<int>(y));
    const bool panButton = message == WM_MBUTTONDOWN ||
                           (message == WM_LBUTTONDOWN && (GetKeyState(VK_SPACE) & 0x8000));
    if (panButton && mapPoint)
    {
        rangePanning_ = true;
        rangePanPoint_ = {static_cast<LONG>(x), static_cast<LONG>(y)};
        SetCapture(window);
        return true;
    }
    if (rangePanning_)
    {
        if (message == WM_MOUSEMOVE)
        {
            renderer_.panBy(x - rangePanPoint_.x, y - rangePanPoint_.y);
            rangePanPoint_ = {static_cast<LONG>(x), static_cast<LONG>(y)};
            requestRedraw();
            return true;
        }
        if (message == WM_MBUTTONUP || message == WM_LBUTTONUP)
        {
            rangePanning_ = false;
            ReleaseCapture();
            updateUi(runtime);
            return true;
        }
    }
    if (rangeDraggingVertex_)
    {
        if (message == WM_MOUSEMOVE)
        {
            if (const auto coordinate = renderer_.lonLatAt(x, y))
            {
                rangeDragVertices_[*rangeDraggingVertex_] = *coordinate;
                renderer_.SetDraftRegion(rangeDragVertices_,
                                         rangeEditor_.GetSelectedRegion()->GetRole() ==
                                             DownTool::DownloadRegionRole::ExcludeGis,
                                         true);
                requestRedraw();
            }
            return true;
        }
        if (message == WM_LBUTTONUP)
        {
            rangeEditor_.SetSelectedVertices(rangeDragVertices_, rangeEditorMessage_);
            rangeDraggingVertex_.reset();
            rangeDragVertices_.clear();
            ReleaseCapture();
            SyncRangeEditor(runtime);
            return true;
        }
    }
    if (message == WM_LBUTTONUP && rangeRectangleAnchor_)
    {
        if (const auto coordinate = renderer_.lonLatAt(x, y))
        {
            const auto anchor = *rangeRectangleAnchor_;
            rangeDraft_ = {{anchor.lon, anchor.lat},
                           {coordinate->lon, anchor.lat},
                           *coordinate,
                           {anchor.lon, coordinate->lat}};
        }
        rangeRectangleAnchor_.reset();
        ReleaseCapture();
        CompleteRangeDrawing(runtime);
        return true;
    }
    if (!mapPoint)
    {
        return false;
    }
    const auto coordinate = renderer_.lonLatAt(x, y);
    if (!coordinate)
    {
        return false;
    }
    if (message == WM_LBUTTONDOWN)
    {
        rangeMapKeyboardActive_ = true;
        rangeEditorMessage_.clear();
        const double hitRadiusSquared = std::pow(9. * runtime.effectiveScale(), 2);
        if (rangeDrawShape_ == RangeDrawShape::Polygon)
        {
            const auto first = rangeDraft_.empty()
                                   ? std::nullopt
                                   : renderer_.ScreenPointForLonLat(rangeDraft_.front());
            if (rangeDraft_.size() >= 3 && first &&
                DistanceSquared(*first, x, y) <= hitRadiusSquared)
            {
                CompleteRangeDrawing(runtime);
            }
            else
            {
                rangeDraft_.push_back(*coordinate);
                SyncRangeEditor(runtime);
            }
            return true;
        }
        if (rangeDrawShape_ == RangeDrawShape::Rectangle)
        {
            rangeRectangleAnchor_ = coordinate;
            rangeDraft_.clear();
            SetCapture(window);
            return true;
        }
        if (const auto* selected = rangeEditor_.GetSelectedRegion();
            selected && selected->GetEnabled())
        {
            const auto& vertices = selected->GetVertices();
            for (size_t index = 0; index < vertices.size(); ++index)
            {
                const auto point = renderer_.ScreenPointForLonLat(vertices[index]);
                if (point && DistanceSquared(*point, x, y) <= hitRadiusSquared)
                {
                    rangeDragVertices_ = vertices;
                    rangeDraggingVertex_ = index;
                    rangeSelectedVertex_ = index;
                    SetCapture(window);
                    return true;
                }
            }
            for (size_t index = 0; index < vertices.size(); ++index)
            {
                const auto start = renderer_.ScreenPointForLonLat(vertices[index]);
                const auto end =
                    renderer_.ScreenPointForLonLat(vertices[(index + 1) % vertices.size()]);
                if (start && end && SegmentDistanceSquared(*start, *end, x, y) <= hitRadiusSquared)
                {
                    rangeDragVertices_ = vertices;
                    rangeDragVertices_.insert(rangeDragVertices_.begin() + index + 1, *coordinate);
                    rangeDraggingVertex_ = index + 1;
                    rangeSelectedVertex_ = index + 1;
                    SetCapture(window);
                    return true;
                }
            }
        }
        for (auto region = rangeEditor_.GetRegions().rbegin();
             region != rangeEditor_.GetRegions().rend(); ++region)
        {
            if (region->GetEnabled() &&
                DownTool::GeographicRingContainsPoint(region->GetVertices(), *coordinate))
            {
                rangeEditor_.SetSelectedId(region->GetId());
                rangeSelectedVertex_.reset();
                SyncRangeEditor(runtime);
                return true;
            }
        }
        rangeEditor_.SetSelectedId({});
        SyncRangeEditor(runtime);
        return false;
    }
    if (message == WM_MOUSEMOVE)
    {
        if (rangeRectangleAnchor_)
        {
            const auto anchor = *rangeRectangleAnchor_;
            rangeDraft_ = {{anchor.lon, anchor.lat},
                           {coordinate->lon, anchor.lat},
                           *coordinate,
                           {anchor.lon, coordinate->lat}};
            renderer_.SetDraftRegion(
                rangeDraft_, rangeDrawRole_ == DownTool::DownloadRegionRole::ExcludeGis, true);
            requestRedraw();
            return true;
        }
        if (!rangeDraft_.empty() && rangeDrawShape_ == RangeDrawShape::Polygon)
        {
            auto draft = rangeDraft_;
            draft.push_back(*coordinate);
            renderer_.SetDraftRegion(
                draft, rangeDrawRole_ == DownTool::DownloadRegionRole::ExcludeGis, false);
            requestRedraw();
            return true;
        }
    }
    if (message == WM_LBUTTONDBLCLK && rangeDrawShape_ != RangeDrawShape::Select)
    {
        if (rangeDrawShape_ == RangeDrawShape::Polygon)
        {
            CompleteRangeDrawing(runtime);
        }
        return true;
    }
    return false;
}
