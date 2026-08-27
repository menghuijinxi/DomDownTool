#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

#include "PostProcessTestSupport.h"
#include "preview/Dx12FbxRenderer.h"
#include "preview/FbxPreviewRenderer.h"

namespace
{
bool hasOpaquePixel(const SkBitmap& bitmap)
{
    for (int y = 0; y < bitmap.height(); ++y)
    {
        for (int x = 0; x < bitmap.width(); ++x)
        {
            if (SkColorGetA(bitmap.getColor(x, y)) != 0)
            {
                return true;
            }
        }
    }
    return false;
}

bool hasMagentaTexturePixel(const SkBitmap& bitmap)
{
    for (int y = 0; y < bitmap.height(); ++y)
    {
        for (int x = 0; x < bitmap.width(); ++x)
        {
            const SkColor color = bitmap.getColor(x, y);
            if (SkColorGetA(color) != 0 && SkColorGetR(color) > 220 &&
                SkColorGetG(color) < 35 && SkColorGetB(color) > 220)
            {
                return true;
            }
        }
    }
    return false;
}

bool drawMode(DomDownToolPreview::FbxPreviewRenderer& renderer,
              DomDownToolPreview::FbxPreviewMode mode, SkBitmap& bitmap)
{
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas(bitmap);
    renderer.setMode(mode);
    renderer.draw(canvas, bitmap.width(), bitmap.height(), 1.F);
    return hasOpaquePixel(bitmap);
}

DomDownToolPreview::FbxPreviewLoadStatus waitForAsyncLoad(
    DomDownToolPreview::Dx12FbxRenderer& renderer)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    DomDownToolPreview::FbxPreviewLoadStatus status = renderer.loadStatus();
    while (status.loading && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        status = renderer.loadStatus();
    }
    return status;
}

bool runTest()
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("fbx-preview-renderer");
    if (!postprocess_test::ResetDirectory(testRoot))
    {
        return false;
    }

    postprocess_test::TestPostProcessDown tool;
    const std::filesystem::path outputRoot = testRoot / "output";
    const std::filesystem::path fbxRoot = outputRoot / "FBX";
    const DownTool::Tile tile(1, 1, 2);
    tool.Configure(testRoot / "cache", outputRoot / "XYZ", 2, 32, tile, tile,
                   DownTool::LonLat(45., 0.));
    tool.SetPostProcessEnabled(true);
    tool.SetPostProcessFbxEnabled(true);
    tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(fbxRoot));
    tool.SetFbxTextureSize(64);
    tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
    tool.SetFbxMeshSubdivision(4);

    if (!postprocess_test::WriteSourceTile(
            tool.TilePath(tile), 32, cv::Scalar(255, 0, 255, 255)))
    {
        return false;
    }
    tool.RunPostProcessForTest();

    const std::filesystem::path fbxPath = fbxRoot / "plane.fbx";
    if (!postprocess_test::RequireNonEmptyFile(fbxPath))
    {
        return false;
    }

    DomDownToolPreview::FbxPreviewRenderer renderer;
    std::string error;
    if (!renderer.load({fbxPath}, error) || renderer.empty() ||
        renderer.vertexCount() == 0 || renderer.triangleCount() == 0)
    {
        std::cerr << "无法加载可预览的 FBX：" << error << '\n';
        return false;
    }

    SkBitmap bitmap;
    if (!bitmap.tryAllocN32Pixels(256, 256))
    {
        return false;
    }
    if (!drawMode(renderer, DomDownToolPreview::FbxPreviewMode::Textured, bitmap) ||
        !hasMagentaTexturePixel(bitmap) ||
        !drawMode(renderer, DomDownToolPreview::FbxPreviewMode::Wireframe, bitmap) ||
        !drawMode(renderer, DomDownToolPreview::FbxPreviewMode::TexturedWireframe, bitmap))
    {
        std::cerr << "FBX 贴图或线框模式没有绘制到画布。\n";
        return false;
    }
    renderer.rotateBy(24.0, -16.0);
    renderer.panBy(18.0, -12.0);
    renderer.zoomBy(1.0);
    if (!drawMode(renderer, DomDownToolPreview::FbxPreviewMode::TexturedWireframe, bitmap))
    {
        std::cerr << "FBX 交互相机变换后没有绘制到画布。\n";
        return false;
    }

    std::atomic_size_t redrawCount = 0;
    DomDownToolPreview::Dx12FbxRenderer dx12Renderer([&redrawCount]
    {
        redrawCount.fetch_add(1, std::memory_order_relaxed);
    });
    const auto loadStarted = std::chrono::steady_clock::now();
    error.clear();
    if (!dx12Renderer.load({fbxPath}, error) || !error.empty() ||
        std::chrono::steady_clock::now() - loadStarted > std::chrono::milliseconds(250))
    {
        std::cerr << "DX12 FBX 后台加载没有立即返回。\n";
        return false;
    }
    const DomDownToolPreview::FbxPreviewLoadStatus asyncStatus =
        waitForAsyncLoad(dx12Renderer);
    if (asyncStatus.loading || !asyncStatus.meshReady || !asyncStatus.error.empty() ||
        asyncStatus.totalTextureCount == 0 ||
        asyncStatus.completedTextureCount != asyncStatus.totalTextureCount ||
        dx12Renderer.vertexCount() == 0 || dx12Renderer.triangleCount() == 0 ||
        redrawCount.load(std::memory_order_relaxed) < 2)
    {
        std::cerr << "DX12 FBX 网格或贴图没有在后台完成加载。\n";
        return false;
    }
    const DomDownToolPreview::FbxPreviewRenderStats texturedStats =
        dx12Renderer.renderStats();
    dx12Renderer.setMode(DomDownToolPreview::FbxPreviewMode::TexturedWireframe);
    const DomDownToolPreview::FbxPreviewRenderStats combinedStats =
        dx12Renderer.renderStats();
    if (texturedStats.totalDrawCallCount == 0 ||
        texturedStats.totalTriangleCount != dx12Renderer.triangleCount() ||
        texturedStats.currentDrawCallCount > texturedStats.totalDrawCallCount ||
        texturedStats.currentTriangleCount > texturedStats.totalTriangleCount ||
        combinedStats.totalDrawCallCount != texturedStats.totalDrawCallCount * 2U ||
        combinedStats.totalTriangleCount != texturedStats.totalTriangleCount ||
        combinedStats.currentDrawCallCount > combinedStats.totalDrawCallCount)
    {
        std::cerr << "DX12 FBX 预览统计没有保留总量或当前模式 DrawCall。\n";
        return false;
    }
    dx12Renderer.rotateBy(24.0, -16.0);
    dx12Renderer.panBy(18.0, -12.0);
    dx12Renderer.verticalPanBy(1.0);
    dx12Renderer.verticalPanBy(-1.0);
    dx12Renderer.zoomBy(1.0);
    dx12Renderer.clear();
    const DomDownToolPreview::FbxPreviewLoadStatus clearedStatus =
        dx12Renderer.loadStatus();
    if (clearedStatus.loading || clearedStatus.meshReady ||
        clearedStatus.totalTextureCount != 0 || !dx12Renderer.empty())
    {
        std::cerr << "DX12 FBX 后台任务取消后没有清理状态。\n";
        return false;
    }

    error.clear();
    if (!dx12Renderer.load({fbxPath}, error))
    {
        return false;
    }
    dx12Renderer.clear();
    const DomDownToolPreview::FbxPreviewLoadStatus cancelledStatus =
        dx12Renderer.loadStatus();
    if (cancelledStatus.loading || cancelledStatus.meshReady ||
        cancelledStatus.totalTextureCount != 0 || !cancelledStatus.error.empty())
    {
        std::cerr << "DX12 FBX 加载中的取消没有恢复空闲状态。\n";
        return false;
    }

    error.clear();
    if (!dx12Renderer.load({testRoot / "missing.fbx"}, error) || !error.empty())
    {
        std::cerr << "DX12 FBX 无效路径没有进入异步错误报告流程。\n";
        return false;
    }
    const DomDownToolPreview::FbxPreviewLoadStatus invalidStatus =
        waitForAsyncLoad(dx12Renderer);
    if (invalidStatus.loading || invalidStatus.meshReady || invalidStatus.error.empty())
    {
        std::cerr << "DX12 FBX 无效路径没有报告后台加载错误。\n";
        return false;
    }

    error.clear();
    if (renderer.load({testRoot / "missing.fbx"}, error) || error.empty() ||
        !renderer.empty())
    {
        std::cerr << "无效 FBX 没有返回明确加载失败。\n";
        return false;
    }
    return true;
}
} // namespace

int main()
{
    try
    {
        return runTest() ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
