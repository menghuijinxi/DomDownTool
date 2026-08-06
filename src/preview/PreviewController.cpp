#include "preview/PreviewController.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "fmt/format.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkRect.h"
#include "skui_runtime.h"

namespace
{
constexpr UINT kPreviewRedrawMessage = WM_APP + 0x642;
constexpr UINT kPreviewUiRefreshMessage = WM_APP + 0x643;

POINT clientPoint(LPARAM lParam)
{
    return POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                 static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool equalStats(const DomDownToolPreview::MapRenderStats& left,
                const DomDownToolPreview::MapRenderStats& right)
{
    return left.visibleTiles == right.visibleTiles && left.readyTiles == right.readyTiles &&
        left.cache.residentTiles == right.cache.residentTiles &&
        left.cache.pendingTiles == right.cache.pendingTiles &&
        left.cache.cpuBytes == right.cache.cpuBytes &&
        left.cache.gpuBytes == right.cache.gpuBytes &&
        left.downloads.queuedTiles == right.downloads.queuedTiles &&
        left.downloads.activeTiles == right.downloads.activeTiles &&
        left.downloads.failedTiles == right.downloads.failedTiles;
}
} // namespace

DomDownToolPreview::PreviewController::PreviewController()
    : renderer_([this] { requestRedraw(); })
{
}

void DomDownToolPreview::PreviewController::attachRuntime(skui::Runtime& runtime)
{
    runtime_ = &runtime;
    runtime.setElementEventCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.id == "provider-select" &&
                event.type == skui::ElementEventType::MouseDown)
            {
                selectPopupOpen_ = true;
                return;
            }

            if (event.id == "provider-select" && event.type == skui::ElementEventType::Change)
            {
                selectPopupOpen_ = false;
                if (renderer_.selectSource(event.value))
                {
                    updateUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.type != skui::ElementEventType::Click)
            {
                return;
            }

            if (event.id == "zoom-in")
            {
                renderer_.zoomAt(static_cast<double>(runtime.width()) * 0.5,
                                 static_cast<double>(runtime.height()) * 0.5, 1.);
            }
            else if (event.id == "zoom-out")
            {
                renderer_.zoomAt(static_cast<double>(runtime.width()) * 0.5,
                                 static_cast<double>(runtime.height()) * 0.5, -1.);
            }
            else if (event.id == "reload-tiles")
            {
                renderer_.clearCache();
            }
            else if (event.id == "auto-download-toggle")
            {
                renderer_.setAutoDownloadEnabled(!renderer_.autoDownloadEnabled());
            }
            else
            {
                return;
            }

            updateUi(runtime);
            requestRedraw();
        });
}

void DomDownToolPreview::PreviewController::resizeRuntime(skui::Runtime& runtime)
{
    updateUi(runtime);
}

void DomDownToolPreview::PreviewController::draw(SkCanvas& canvas, int width, int height,
                                                 float dpiScale)
{
    renderer_.draw(canvas, width, height, dpiScale);

    if (runtime_)
    {
        SkAutoCanvasRestore restore(&canvas, true);
        canvas.clipRect(SkRect::MakeLTRB(0.F, 0.F, static_cast<float>(width),
                                         static_cast<float>(height)),
                        SkClipOp::kIntersect, false);
        skui::RuntimeRenderRegion overlayRegion;
        overlayRegion.drawBackground = false;
        runtime_->renderInto(canvas, overlayRegion);
    }

    scheduleUiRefresh();
}

bool DomDownToolPreview::PreviewController::handleWindowMessage(HWND window, UINT message,
                                                                WPARAM wParam, LPARAM lParam,
                                                                skui::Runtime& runtime)
{
    window_.store(message == WM_DESTROY ? nullptr : window, std::memory_order_relaxed);
    if (message == kPreviewRedrawMessage)
    {
        return true;
    }
    if (message == kPreviewUiRefreshMessage)
    {
        updateUi(runtime);
        return true;
    }

    if (selectPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        if (message == WM_LBUTTONDOWN)
        {
            selectPopupOpen_ = false;
        }
        return false;
    }

    if (message == WM_CAPTURECHANGED)
    {
        dragging_ = false;
        return false;
    }

    if (message == WM_LBUTTONUP && dragging_)
    {
        dragging_ = false;
        if (GetCapture() == window)
        {
            ReleaseCapture();
        }
        return true;
    }

    if (message == WM_MOUSEWHEEL)
    {
        POINT point{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
        ScreenToClient(window, &point);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }

        const double wheelSteps = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        renderer_.zoomAt(point.x, point.y, wheelSteps * 0.25);
        updateCursor(runtime, point.x, point.y);
        updateUi(runtime);
        return true;
    }

    if (message == WM_LBUTTONDBLCLK)
    {
        const POINT point = clientPoint(lParam);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }
        renderer_.zoomAt(point.x, point.y, 1.);
        updateUi(runtime);
        return true;
    }

    if (message == WM_LBUTTONDOWN)
    {
        const POINT point = clientPoint(lParam);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }
        dragging_ = true;
        lastDragPoint_ = point;
        SetCapture(window);
        return true;
    }

    if (message == WM_MOUSEMOVE)
    {
        const POINT point = clientPoint(lParam);
        if (dragging_)
        {
            const double deltaX = static_cast<double>(point.x - lastDragPoint_.x);
            const double deltaY = static_cast<double>(point.y - lastDragPoint_.y);
            lastDragPoint_ = point;
            renderer_.panBy(deltaX, deltaY);
            updateUi(runtime);
            updateCursor(runtime, point.x, point.y);
            return true;
        }

        if (isMapPoint(window, point.x, point.y))
        {
            updateCursor(runtime, point.x, point.y);
            return true;
        }
    }

    return false;
}

bool DomDownToolPreview::PreviewController::isMapPoint(HWND window, int x, int y) const
{
    RECT client{};
    GetClientRect(window, &client);
    return x >= 0 && x < client.right && static_cast<double>(y) >= renderer_.toolbarHeight() &&
           static_cast<double>(y) <
               static_cast<double>(client.bottom) - renderer_.statusBarHeight();
}

void DomDownToolPreview::PreviewController::updateUi(skui::Runtime& runtime)
{
    const MapSourceSummary source = renderer_.activeSource();
    const DownTool::LonLat center = renderer_.center();
    const MapRenderStats stats = renderer_.stats();
    const double memoryMegabytes =
        static_cast<double>(stats.cache.cpuBytes + stats.cache.gpuBytes) / (1024. * 1024.);

    runtime.setValueById("provider-select", source.id);
    runtime.setTextById("auto-download-toggle",
                        renderer_.autoDownloadEnabled() ? "自动下载" : "仅缓存");
    runtime.setTextsById(
        {{"active-provider", source.name},
         {"coordinate-system", source.coordinateSystem},
         {"zoom-value", fmt::format("Z {:.2f}", renderer_.zoom())},
         {"center-coordinate", fmt::format("{:.6f}, {:.6f}", center.lon, center.lat)},
         {"cache-path", pathToUtf8(source.cacheRoot)},
         {"tile-stats",
          fmt::format("可见 {}  已载入 {}  读取 {}  下载 {}/{}  失败 {}  缓存 {:.1f} MB",
                      stats.visibleTiles, stats.readyTiles, stats.cache.pendingTiles,
                      stats.downloads.activeTiles, stats.downloads.queuedTiles,
                      stats.downloads.failedTiles, memoryMegabytes)}});
}

void DomDownToolPreview::PreviewController::updateCursor(skui::Runtime& runtime, double screenX,
                                                         double screenY)
{
    const std::optional<DownTool::LonLat> coordinate = renderer_.lonLatAt(screenX, screenY);
    if (!coordinate.has_value())
    {
        return;
    }

    runtime.setTextById("cursor-coordinate",
                        fmt::format("{:.6f}, {:.6f}", coordinate->lon, coordinate->lat));
}

void DomDownToolPreview::PreviewController::scheduleUiRefresh()
{
    const MapRenderStats stats = renderer_.stats();
    if (scheduledStats_.has_value() && equalStats(*scheduledStats_, stats))
    {
        return;
    }

    const HWND window = window_.load(std::memory_order_relaxed);
    if (window && PostMessageW(window, kPreviewUiRefreshMessage, 0, 0))
    {
        scheduledStats_ = stats;
    }
}

void DomDownToolPreview::PreviewController::requestRedraw() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (window)
    {
        PostMessageW(window, kPreviewRedrawMessage, 0, 0);
    }
}
