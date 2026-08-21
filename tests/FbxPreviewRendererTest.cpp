#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"

#include "PostProcessTestSupport.h"
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
