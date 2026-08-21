#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "preview/FbxPreviewRenderer.h"

class SkCanvas;

namespace DomDownToolPreview
{
class Dx12FbxRenderer
{
  public:
    explicit Dx12FbxRenderer(std::function<void()> requestRedraw);
    ~Dx12FbxRenderer();

    Dx12FbxRenderer(const Dx12FbxRenderer&) = delete;
    Dx12FbxRenderer& operator=(const Dx12FbxRenderer&) = delete;

    bool load(const std::vector<std::filesystem::path>& paths, std::string& error);

    void clear();

    void setMode(FbxPreviewMode mode);

    [[nodiscard]] FbxPreviewMode mode() const;

    void rotateBy(double deltaX, double deltaY);

    void zoomBy(double delta);

    [[nodiscard]] bool empty() const;

    [[nodiscard]] std::size_t vertexCount() const;

    [[nodiscard]] std::size_t triangleCount() const;

    [[nodiscard]] bool draw(SkCanvas& canvas, int width, int height, float dpiScale);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace DomDownToolPreview
