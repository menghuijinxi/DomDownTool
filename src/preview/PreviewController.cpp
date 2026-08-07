#include "preview/PreviewController.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "fmt/format.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkRect.h"
#include "skui_runtime.h"

#include "preview/SearchTrace.h"

namespace
{
constexpr UINT kPreviewRedrawMessage = WM_APP + 0x642;
constexpr UINT kPreviewUiRefreshMessage = WM_APP + 0x643;
constexpr UINT kPreviewSearchRefreshMessage = WM_APP + 0x644;
constexpr double kSearchControlLeft = 192.;
constexpr double kSearchControlWidth = 300.;
constexpr double kSearchPopupWidth = 360.;
constexpr double kSearchPopupTop = 46.;
constexpr double kSearchPopupHeight = 374.;
constexpr double kGisColorPanelLeft = 720.;
constexpr double kGisColorPanelTop = 48.;
constexpr double kGisColorPanelWidth = 238.;
constexpr double kGisColorPanelHeight = 288.;

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

std::string escapeHtml(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
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
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

std::optional<size_t> searchResultIndex(std::string_view id)
{
    constexpr std::string_view kPrefix = "search-result-";
    if (!id.starts_with(kPrefix))
    {
        return std::nullopt;
    }

    size_t index = 0;
    const std::string_view indexText = id.substr(kPrefix.size());
    const auto parseResult =
        std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
    if (parseResult.ec != std::errc{} || parseResult.ptr != indexText.data() + indexText.size())
    {
        return std::nullopt;
    }
    return index;
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisCategoryFromValue(
    std::string_view value)
{
    static const std::pair<std::string_view, DomDownToolPreview::GisOverlayCategory> categories[] = {
        {"none", DomDownToolPreview::GisOverlayCategory::None},
        {"all", DomDownToolPreview::GisOverlayCategory::All},
        {"road", DomDownToolPreview::GisOverlayCategory::Road},
        {"water", DomDownToolPreview::GisOverlayCategory::Water},
        {"green", DomDownToolPreview::GisOverlayCategory::Green},
        {"highway", DomDownToolPreview::GisOverlayCategory::Highway},
        {"subway", DomDownToolPreview::GisOverlayCategory::Subway},
        {"building", DomDownToolPreview::GisOverlayCategory::Building},
        {"point", DomDownToolPreview::GisOverlayCategory::Point},
        {"boundary", DomDownToolPreview::GisOverlayCategory::Boundary},
        {"line", DomDownToolPreview::GisOverlayCategory::Boundary},
        {"other", DomDownToolPreview::GisOverlayCategory::Other}};
    for (const auto& [name, category] : categories)
    {
        if (name == value)
        {
            return category;
        }
    }
    return std::nullopt;
}

std::string gisCategoryValue(DomDownToolPreview::GisOverlayCategory category)
{
    switch (category)
    {
    case DomDownToolPreview::GisOverlayCategory::None:
        return "none";
    case DomDownToolPreview::GisOverlayCategory::All:
        return "all";
    case DomDownToolPreview::GisOverlayCategory::Road:
        return "road";
    case DomDownToolPreview::GisOverlayCategory::Water:
        return "water";
    case DomDownToolPreview::GisOverlayCategory::Green:
        return "green";
    case DomDownToolPreview::GisOverlayCategory::Highway:
        return "highway";
    case DomDownToolPreview::GisOverlayCategory::Subway:
        return "subway";
    case DomDownToolPreview::GisOverlayCategory::Building:
        return "building";
    case DomDownToolPreview::GisOverlayCategory::Point:
        return "point";
    case DomDownToolPreview::GisOverlayCategory::Boundary:
        return "boundary";
    case DomDownToolPreview::GisOverlayCategory::Other:
        return "other";
    }
    return "none";
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisColorCategoryFromId(
    std::string_view id)
{
    constexpr std::string_view prefix = "gis-color-";
    if (!id.starts_with(prefix))
    {
        return std::nullopt;
    }
    return gisCategoryFromValue(id.substr(prefix.size()));
}

std::optional<DownTool::AmapVectorColor> parseGisColor(std::string_view value)
{
    if (!value.empty() && value.front() == '#')
    {
        value.remove_prefix(1);
    }
    if (value.size() != 6)
    {
        return std::nullopt;
    }
    const auto parseByte = [](std::string_view text) -> std::optional<uint8_t>
    {
        uint32_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 255)
        {
            return std::nullopt;
        }
        return static_cast<uint8_t>(parsed);
    };
    const std::optional<uint8_t> red = parseByte(value.substr(0, 2));
    const std::optional<uint8_t> green = parseByte(value.substr(2, 2));
    const std::optional<uint8_t> blue = parseByte(value.substr(4, 2));
    if (!red || !green || !blue)
    {
        return std::nullopt;
    }
    return DownTool::AmapVectorColor{*red, *green, *blue, 255};
}

std::optional<skui::Event> runtimeMouseEvent(HWND window, UINT message, WPARAM wParam,
                                              LPARAM lParam)
{
    skui::Event event;
    const POINT client = clientPoint(lParam);
    switch (message)
    {
    case WM_MOUSEMOVE:
        event.type = skui::EventType::MouseMove;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_MOUSELEAVE:
        event.type = skui::EventType::MouseLeave;
        break;
    case WM_LBUTTONDOWN:
        event.type = skui::EventType::MouseDown;
        event.button = skui::MouseButton::Left;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_LBUTTONUP:
        event.type = skui::EventType::MouseUp;
        event.button = skui::MouseButton::Left;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_LBUTTONDBLCLK:
        event.type = skui::EventType::MouseDoubleClick;
        event.button = skui::MouseButton::Left;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_MBUTTONDOWN:
        event.type = skui::EventType::MouseDown;
        event.button = skui::MouseButton::Middle;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_MBUTTONUP:
        event.type = skui::EventType::MouseUp;
        event.button = skui::MouseButton::Middle;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_RBUTTONDOWN:
        event.type = skui::EventType::MouseDown;
        event.button = skui::MouseButton::Right;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_RBUTTONUP:
        event.type = skui::EventType::MouseUp;
        event.button = skui::MouseButton::Right;
        event.x = static_cast<float>(client.x);
        event.y = static_cast<float>(client.y);
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    {
        POINT point{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
        ScreenToClient(window, &point);
        event.type = skui::EventType::MouseWheel;
        event.x = static_cast<float>(point.x);
        event.y = static_cast<float>(point.y);
        event.wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
        event.shiftKey = message == WM_MOUSEHWHEEL ||
            (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        break;
    }
    default:
        return std::nullopt;
    }
    return event;
}
} // namespace

DomDownToolPreview::PreviewController::PreviewController()
    : renderer_([this] { requestRedraw(); }),
      placeSearch_([this] { scheduleSearchRefresh(); },
                   [this](const std::string& url) { return webViewSearch_.get(url); },
                   [this] { webViewSearch_.cancelCurrent(); })
{
}

void DomDownToolPreview::PreviewController::attachRuntime(skui::Runtime& runtime)
{
    writeSearchTrace("Controller", "runtime_attached");
    runtime_ = &runtime;
    runtime.setElementEventCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.id == "place-search-input" &&
                event.type == skui::ElementEventType::Input)
            {
                searchQuery_ = event.value;
                pendingLocationRequest_.reset();
                writeSearchTrace("Controller", "input_changed", "query=" + searchQuery_);
                if (searchQuery_.find_first_not_of(" \t\r\n") == std::string::npos)
                {
                    placeSearch_.cancelPending();
                    searchResults_.clear();
                    highlightedSearchResult_.reset();
                    searchState_ = SearchState::Idle;
                    searchPopupOpen_ = false;
                    updateSearchUi(runtime);
                }
                else
                {
                    searchState_ = SearchState::Loading;
                    searchMessage_.clear();
                    searchPopupOpen_ = true;
                    const uint64_t requestId =
                        placeSearch_.submitSuggestions(searchQuery_, renderer_.center());
                    writeSearchTrace("Controller", "suggestions_requested",
                                     fmt::format("id={}", requestId));
                    updateSearchUi(runtime);
                }
                requestRedraw();
                return;
            }

            if (event.id == "place-search-input" &&
                event.type == skui::ElementEventType::MouseDown)
            {
                if (!searchResults_.empty() || searchState_ == SearchState::Error)
                {
                    searchPopupOpen_ = true;
                    updateSearchUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if ((event.id == "provider-select" || event.id == "gis-overlay-select") &&
                event.type == skui::ElementEventType::MouseDown)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_mouse_down id={} value={} x={} y={}",
                                             event.id, event.value, event.x, event.y));
                return;
            }

            if (event.id == "provider-select" && event.type == skui::ElementEventType::Change)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_change id={} value={}", event.id,
                                             event.value));
                if (renderer_.selectSource(event.value))
                {
                    updateUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.id == "gis-overlay-select" && event.type == skui::ElementEventType::Change)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_change id={} value={}", event.id,
                                             event.value));
                const std::optional<GisOverlayCategory> category =
                    gisCategoryFromValue(event.value);
                if (category.has_value())
                {
                    renderer_.SetGisOverlayCategory(*category);
                    updateUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.type == skui::ElementEventType::Input)
            {
                const std::optional<GisOverlayCategory> category = gisColorCategoryFromId(event.id);
                if (category.has_value())
                {
                    const std::optional<DownTool::AmapVectorColor> color =
                        parseGisColor(event.value);
                    if (color.has_value())
                    {
                        renderer_.SetGisOverlayColor(*category, *color);
                    }
                    return;
                }
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
            else if (event.id == "gis-color-toggle")
            {
                gisColorPopupOpen_ = !gisColorPopupOpen_;
            }
            else if (event.id == "view-span-decrease")
            {
                renderer_.adjustViewSpan(-0.5);
            }
            else if (event.id == "view-span-increase")
            {
                renderer_.adjustViewSpan(0.5);
            }
            else if (event.id == "place-search-submit")
            {
                submitPlaceSearch(runtime);
            }
            else if (event.id == "place-search-clear")
            {
                clearPlaceSearch(runtime);
            }
            else if (const std::optional<size_t> resultIndex = searchResultIndex(event.id))
            {
                selectPlaceSearchResult(runtime, *resultIndex);
            }
            else
            {
                return;
            }

            updateUi(runtime);
            requestRedraw();
        });
    runtime.setElementKeyDownCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.id != "place-search-input")
            {
                return false;
            }
            if (event.key == VK_RETURN)
            {
                if (searchPopupOpen_ && highlightedSearchResult_.has_value() &&
                    *highlightedSearchResult_ < searchResults_.size())
                {
                    selectPlaceSearchResult(runtime, *highlightedSearchResult_);
                    return true;
                }
                submitPlaceSearch(runtime);
                return true;
            }
            if (event.key == VK_DOWN)
            {
                moveSearchHighlight(runtime, 1);
                return true;
            }
            if (event.key == VK_UP)
            {
                moveSearchHighlight(runtime, -1);
                return true;
            }
            if (event.key == VK_ESCAPE)
            {
                placeSearch_.cancelPending();
                pendingLocationRequest_.reset();
                searchPopupOpen_ = false;
                updateSearchUi(runtime);
                requestRedraw();
                return true;
            }
            return false;
        });
    updateUi(runtime);
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
    if (message == kPreviewSearchRefreshMessage)
    {
        writeSearchTrace("Controller", "search_refresh_message_received");
        processPlaceSearchResponses(runtime);
        return true;
    }

    const std::optional<skui::Event> mouseEvent =
        runtimeMouseEvent(window, message, wParam, lParam);
    if (mouseEvent.has_value())
    {
        if (runtime.handleEvent(*mouseEvent))
        {
            requestRedraw();
            return true;
        }
    }

    if (searchPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isSearchInteractionPoint(runtime, point.x, point.y))
        {
            return mouseEvent.has_value();
        }
        if (message == WM_LBUTTONDOWN)
        {
            placeSearch_.cancelPending();
            pendingLocationRequest_.reset();
            searchPopupOpen_ = false;
            updateSearchUi(runtime);
        }
    }

    if (gisColorPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isGisColorInteractionPoint(runtime, point.x, point.y))
        {
            return mouseEvent.has_value();
        }
        if (message == WM_LBUTTONDOWN)
        {
            gisColorPopupOpen_ = false;
            updateUi(runtime);
        }
    }

    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
        message == WM_KILLFOCUS)
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
            return mouseEvent.has_value();
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
            return mouseEvent.has_value();
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
            dragging_ = false;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            return mouseEvent.has_value();
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

    return mouseEvent.has_value();
}

bool DomDownToolPreview::PreviewController::isMapPoint(HWND window, int x, int y) const
{
    RECT client{};
    GetClientRect(window, &client);
    return x >= 0 && x < client.right && static_cast<double>(y) >= renderer_.toolbarHeight() &&
           static_cast<double>(y) <
               static_cast<double>(client.bottom) - renderer_.statusBarHeight();
}

bool DomDownToolPreview::PreviewController::isSearchInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.dpiScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    const bool insideControl = logicalX >= kSearchControlLeft &&
        logicalX <= kSearchControlLeft + kSearchControlWidth && logicalY >= 0. && logicalY <= 52.;
    const bool insidePopup = searchPopupOpen_ && logicalX >= kSearchControlLeft &&
        logicalX <= kSearchControlLeft + kSearchPopupWidth && logicalY >= kSearchPopupTop &&
        logicalY <= kSearchPopupTop + kSearchPopupHeight;
    return insideControl || insidePopup;
}

bool DomDownToolPreview::PreviewController::isGisColorInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.dpiScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    return logicalX >= kGisColorPanelLeft &&
        logicalX <= kGisColorPanelLeft + kGisColorPanelWidth &&
        logicalY >= kGisColorPanelTop &&
        logicalY <= kGisColorPanelTop + kGisColorPanelHeight;
}

void DomDownToolPreview::PreviewController::submitPlaceSearch(skui::Runtime& runtime)
{
    if (searchQuery_.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        clearPlaceSearch(runtime);
        return;
    }

    searchResults_.clear();
    highlightedSearchResult_.reset();
    searchMessage_.clear();
    pendingLocationRequest_.reset();
    searchState_ = SearchState::Loading;
    searchPopupOpen_ = true;
    const uint64_t requestId = placeSearch_.submitSearch(searchQuery_, renderer_.center());
    writeSearchTrace("Controller", "search_requested",
                     fmt::format("id={} query={}", requestId, searchQuery_));
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::clearPlaceSearch(skui::Runtime& runtime)
{
    writeSearchTrace("Controller", "search_cleared");
    placeSearch_.cancelPending();
    searchQuery_.clear();
    searchMessage_.clear();
    searchResults_.clear();
    highlightedSearchResult_.reset();
    pendingLocationRequest_.reset();
    searchState_ = SearchState::Idle;
    searchPopupOpen_ = false;
    renderer_.setLocationMarker(std::nullopt);
    runtime.setValueById("place-search-input", "");
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::selectPlaceSearchResult(skui::Runtime& runtime,
                                                                    size_t index)
{
    if (index >= searchResults_.size())
    {
        return;
    }

    const PlaceSearchResult& result = searchResults_[index];
    if (!result.coordinateWgs84.has_value())
    {
        const std::string resultId = result.id;
        const std::string resultName = result.name;
        const std::string resolutionQuery =
            result.name + (result.address.empty() ? "" : " " + result.address);
        searchQuery_ = result.name;
        runtime.setValueById("place-search-input", searchQuery_);
        searchResults_.clear();
        highlightedSearchResult_.reset();
        searchMessage_.clear();
        searchState_ = SearchState::Loading;
        searchPopupOpen_ = true;
        const uint64_t requestId =
            placeSearch_.submitSearch(resolutionQuery, renderer_.center());
        pendingLocationRequest_ = PendingLocationRequest{requestId, resultId, resultName};
        writeSearchTrace("Controller", "location_resolution_requested",
                         fmt::format("id={} query={} expected_id={} expected_name={}",
                                     requestId, resolutionQuery, resultId, resultName));
        updateSearchUi(runtime);
        requestRedraw();
        return;
    }

    pendingLocationRequest_.reset();
    renderer_.setView(*result.coordinateWgs84, result.recommendedZoom);
    renderer_.setLocationMarker(result.coordinateWgs84);
    placeSearch_.cancelPending();
    searchQuery_ = result.name;
    runtime.setValueById("place-search-input", searchQuery_);
    searchPopupOpen_ = false;
    writeSearchTrace(
        "Controller", "location_applied",
        fmt::format("name={} lon={:.6f} lat={:.6f} zoom={:.2f}", result.name,
                    result.coordinateWgs84->lon, result.coordinateWgs84->lat,
                    result.recommendedZoom));
    updateSearchUi(runtime);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::moveSearchHighlight(skui::Runtime& runtime,
                                                                 int direction)
{
    if (searchResults_.empty())
    {
        return;
    }

    const size_t resultCount = searchResults_.size();
    size_t nextIndex = highlightedSearchResult_.value_or(direction > 0 ? resultCount - 1 : 0);
    if (direction > 0)
    {
        nextIndex = (nextIndex + 1) % resultCount;
    }
    else
    {
        nextIndex = nextIndex == 0 ? resultCount - 1 : nextIndex - 1;
    }

    highlightedSearchResult_ = nextIndex;
    searchPopupOpen_ = true;
    updateSearchUi(runtime);
    runtime.scrollIntoViewById("search-result-" + std::to_string(nextIndex));
    requestRedraw();
}

void DomDownToolPreview::PreviewController::processPlaceSearchResponses(skui::Runtime& runtime)
{
    const std::vector<PlaceSearchResponse> responses = placeSearch_.takeCompleted();
    if (responses.empty())
    {
        return;
    }

    const PlaceSearchResponse& response = responses.back();
    writeSearchTrace(
        "Controller", "response_processing",
        fmt::format("id={} kind={} results={} error={}", response.requestId,
                    response.kind == PlaceSearchRequestKind::Suggestions ? "suggestions"
                                                                         : "search",
                    response.results.size(), response.error));
    const bool applyResolvedLocation = pendingLocationRequest_.has_value() &&
        pendingLocationRequest_->requestId == response.requestId;
    const std::optional<PendingLocationRequest> locationRequest =
        applyResolvedLocation ? pendingLocationRequest_ : std::nullopt;
    if (applyResolvedLocation)
    {
        pendingLocationRequest_.reset();
    }
    searchResults_ = response.results;
    searchMessage_ = response.error;
    if (applyResolvedLocation && response.succeeded())
    {
        const std::optional<size_t> resultIndex = findExactPlaceResultIndex(
            searchResults_, locationRequest->resultId, locationRequest->resultName);
        if (resultIndex.has_value())
        {
            writeSearchTrace("Controller", "location_resolution_complete",
                             fmt::format("id={} result_index={}", response.requestId,
                                         *resultIndex));
            selectPlaceSearchResult(runtime, *resultIndex);
            return;
        }

        writeSearchTrace(
            "Controller", "location_resolution_rejected",
            fmt::format("id={} expected_id={} expected_name={} candidates={}",
                        response.requestId, locationRequest->resultId,
                        locationRequest->resultName, searchResults_.size()));
        searchResults_.clear();
        searchMessage_ = "无法确认所选地点的坐标，请选择更具体的候选项。";
        searchState_ = SearchState::Error;
        highlightedSearchResult_.reset();
        searchPopupOpen_ = true;
        updateSearchUi(runtime);
        requestRedraw();
        return;
    }
    if (!response.succeeded())
    {
        searchState_ = SearchState::Error;
        highlightedSearchResult_.reset();
    }
    else if (searchResults_.empty())
    {
        searchState_ = SearchState::Empty;
        highlightedSearchResult_.reset();
    }
    else
    {
        searchState_ = SearchState::Results;
        highlightedSearchResult_ = 0;
    }
    searchPopupOpen_ = true;
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::updateSearchUi(skui::Runtime& runtime)
{
    runtime.setVisibleById("place-search-results", searchPopupOpen_);
    if (!searchPopupOpen_)
    {
        return;
    }

    std::string html;
    if (searchState_ == SearchState::Loading)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state\">正在搜索...</div>";
    }
    else if (searchState_ == SearchState::Empty)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state\">没有找到匹配地点</div>";
    }
    else if (searchState_ == SearchState::Error)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state search-error\">" +
            escapeHtml(searchMessage_) + "</div>";
    }
    else
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-result-list\">";
        for (size_t index = 0; index < searchResults_.size(); ++index)
        {
            const PlaceSearchResult& result = searchResults_[index];
            const std::string subtitle = result.district +
                (result.address.empty() ? "" : " · " + result.address);
            html += fmt::format(
                "<button id=\"search-result-{}\" class=\"search-result{}\" type=\"button\" "
                "data-action=\"select-place-result\">"
                "<span class=\"search-result-name\">{}</span>"
                "<span class=\"search-result-address\">{}</span></button>",
                index,
                highlightedSearchResult_.has_value() && *highlightedSearchResult_ == index
                    ? " selected"
                    : "",
                escapeHtml(result.name), escapeHtml(subtitle));
        }
        html += "</div>";
    }
    const bool replaced = runtime.replaceHtmlById("place-search-results-content", html);
    writeSearchTrace(
        "Controller", "search_ui_replaced",
        fmt::format("success={} state={} results={}", replaced,
                    static_cast<int>(searchState_), searchResults_.size()));
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
         {"view-span-value", fmt::format("视野 {:.1f}x", renderer_.GetViewSpan())},
         {"center-coordinate", fmt::format("{:.6f}, {:.6f}", center.lon, center.lat)},
         {"cache-path", pathToUtf8(source.cacheRoot)},
         {"tile-stats",
         fmt::format("可见 {}  已载入 {}  读取 {}  下载 {}/{}  失败 {}  缓存 {:.1f} MB",
                      stats.visibleTiles, stats.readyTiles, stats.cache.pendingTiles,
                      stats.downloads.activeTiles, stats.downloads.queuedTiles,
                      stats.downloads.failedTiles, memoryMegabytes)}});
    const GisOverlayCategory gisCategory = renderer_.GetGisOverlayCategory();
    if (!lastUiGisCategory_.has_value() || *lastUiGisCategory_ != gisCategory)
    {
        writeSearchTrace("Controller", "gis_ui_sync",
                         fmt::format("category={}", gisCategoryValue(gisCategory)));
        lastUiGisCategory_ = gisCategory;
    }
    runtime.setValueById("gis-overlay-select", gisCategoryValue(gisCategory));
    runtime.setVisibleById("gis-color-panel", gisColorPopupOpen_);
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

void DomDownToolPreview::PreviewController::scheduleSearchRefresh() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window)
    {
        writeSearchTrace("Controller", "search_refresh_post_failed", "reason=no_window");
        return;
    }

    const BOOL posted = PostMessageW(window, kPreviewSearchRefreshMessage, 0, 0);
    writeSearchTrace("Controller", posted ? "search_refresh_posted"
                                           : "search_refresh_post_failed",
                     posted ? "" : fmt::format("win32_error={}", GetLastError()));
}

void DomDownToolPreview::PreviewController::requestRedraw() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (window)
    {
        PostMessageW(window, kPreviewRedrawMessage, 0, 0);
    }
}
