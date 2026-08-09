#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include "preview/AmapPlaceSearch.h"
#include "preview/AmapWebViewSearch.h"
#include "preview/MapRenderer.h"

class SkCanvas;

namespace skui
{
class Runtime;
}

namespace DomDownToolPreview
{
class PreviewController
{
  public:
    PreviewController();

    void attachRuntime(skui::Runtime& runtime);

    void resizeRuntime(skui::Runtime& runtime);

    void draw(SkCanvas& canvas, int width, int height, float dpiScale);

    bool handleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                             skui::Runtime& runtime);

  private:
    enum class SearchState
    {
        Idle,
        Loading,
        Results,
        Empty,
        Error
    };

    struct PendingLocationRequest
    {
        uint64_t requestId = 0;
        std::string resultId;
        std::string resultName;
    };

    [[nodiscard]] bool isMapPoint(HWND window, int x, int y) const;

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

    void updateUi(skui::Runtime& runtime);

    void updateCursor(skui::Runtime& runtime, double screenX, double screenY);

    void scheduleUiRefresh();

    void scheduleSearchRefresh() const;

    void requestRedraw() const;

    std::atomic<HWND> window_{nullptr};
    skui::Runtime* runtime_ = nullptr;
    MapRenderer renderer_;
    AmapWebViewSearch webViewSearch_;
    AmapPlaceSearch placeSearch_;
    std::optional<MapRenderStats> scheduledStats_;
    std::string searchQuery_;
    std::string searchMessage_;
    std::vector<PlaceSearchResult> searchResults_;
    std::optional<size_t> highlightedSearchResult_;
    std::optional<PendingLocationRequest> pendingLocationRequest_;
    SearchState searchState_ = SearchState::Idle;
    bool searchPopupOpen_ = false;
    bool gisOverlayPopupOpen_ = false;
    bool gisColorPopupOpen_ = false;
    std::optional<GisOverlayCategories> lastUiGisCategories_;
    bool dragging_ = false;
    POINT lastDragPoint_{};
};
} // namespace DomDownToolPreview
