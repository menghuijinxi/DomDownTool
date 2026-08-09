#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "preview/GisMeshBuilder.h"

class SkCanvas;

namespace DomDownToolPreview
{
struct Dx12GisDrawBatch
{
    std::shared_ptr<const GisGeometryMesh> mesh;
    float red = 1.F;
    float green = 1.F;
    float blue = 1.F;
    float alpha = 1.F;
    float lineWidth = 2.F;
    float pointRadius = 5.F;
};

struct Dx12GisRenderFrame
{
    ViewportTransform transform;
    int width = 0;
    int height = 0;
    float clipTop = 0.F;
    float clipBottom = 0.F;
    bool meshSetReady = false;
    std::vector<std::shared_ptr<const GisGeometryMesh>> residentMeshes;
    std::vector<Dx12GisDrawBatch> batches;
};

struct Dx12GisRenderStats
{
    size_t fillDrawCalls = 0;
    size_t lineDrawCalls = 0;
    size_t pointDrawCalls = 0;
    size_t uploadedMeshes = 0;
};

class Dx12GisRenderer
{
  public:
    explicit Dx12GisRenderer(std::function<void()> requestRedraw);
    ~Dx12GisRenderer();

    Dx12GisRenderer(const Dx12GisRenderer&) = delete;
    Dx12GisRenderer& operator=(const Dx12GisRenderer&) = delete;

    [[nodiscard]] bool SupportsCanvas(SkCanvas& canvas) const;

    // 在 Skia 已经提交底图后，用同一 D3D12 设备把 GIS mesh 绘制到当前 back buffer。
    // raster canvas 或非 D3D surface 会返回 false，调用方应保留 Skia 回退路径。
    [[nodiscard]] bool Draw(SkCanvas& canvas, const Dx12GisRenderFrame& frame);

    void Clear();

    [[nodiscard]] Dx12GisRenderStats stats() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace DomDownToolPreview
