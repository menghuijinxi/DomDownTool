#pragma once

#include <atomic>
#include <optional>

#include <windows.h>

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
    [[nodiscard]] bool isMapPoint(HWND window, int x, int y) const;

    void updateUi(skui::Runtime& runtime);

    void updateCursor(skui::Runtime& runtime, double screenX, double screenY);

    void scheduleUiRefresh();

    void requestRedraw() const;

    std::atomic<HWND> window_{nullptr};
    skui::Runtime* runtime_ = nullptr;
    MapRenderer renderer_;
    std::optional<MapRenderStats> scheduledStats_;
    bool selectPopupOpen_ = false;
    bool dragging_ = false;
    POINT lastDragPoint_{};
};
} // namespace DomDownToolPreview
