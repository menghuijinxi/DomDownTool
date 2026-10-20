#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "AmapBuildingFbxExporter.h"
#include "AmapVectorDownloader.h"
#include "GoogleDownBase.h"

namespace DownTool
{
    struct AmapVectorRenderProgress
    {
        size_t completedTileCount = 0;
        size_t totalTileCount = 0;
        size_t failedTileCount = 0;
    };

    using AmapVectorRenderProgressCallback =
        std::function<void(const AmapVectorRenderProgress&)>;

    enum class AmapVectorRenderCategory
    {
        All,
        Point,
        Road,
        Green,
        Highway,
        Subway,
        Building,
        Water,
        Boundary,
        Other
    };

    struct AmapVectorColor
    {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        uint8_t alpha = 255;
    };

    [[nodiscard]] AmapVectorRenderCategory ClassifyAmapVectorFeature(
        const AmapVectorFeature& feature);

    [[nodiscard]] bool IsAmapVectorBuildingWithHeight(const AmapVectorFeature& feature);

    [[nodiscard]] AmapVectorColor GetDefaultAmapVectorRenderColor(
        AmapVectorRenderCategory category);

    struct AmapVectorRenderOptions
    {
        // All 表示所有类别都会输出；否则只输出集合中列出的类别。
        std::set<AmapVectorRenderCategory> categories = { AmapVectorRenderCategory::All };
        // 默认按显示层级过滤 minZoom/maxZoom；仅显式强制时忽略。
        bool ignoreFeatureZoomRange = false;
        // 启用后，带有效高度的建筑按高度稳定着色；无高度建筑仍使用建筑类别颜色。
        bool colorBuildingsByHeight = false;
        std::map<AmapVectorRenderCategory, AmapVectorColor> colors;

        // 高德未公开 mainKey 的稳定语义表，调用方可按实际下载数据覆盖分类。
        std::map<std::pair<uint32_t, uint32_t>, AmapVectorRenderCategory>
            featureCodeCategories;
        std::map<uint32_t, AmapVectorRenderCategory> mainKeyCategories;
        std::map<std::string, AmapVectorRenderCategory> layerCategories;
        std::map<std::pair<uint32_t, uint32_t>, AmapVectorColor> featureCodeColors;
        std::map<uint32_t, AmapVectorColor> mainKeyColors;

        std::filesystem::path pngTileCachePath =
            std::filesystem::u8path("cache/高德地图/gis数据/临时");
        // 0 表示按硬件线程数自动选择；Render 返回前会等待所有工作线程结束。
        uint32_t maxConcurrency = std::thread::hardware_concurrency();
        // 回调由工作线程串行调用；Render 返回后不会再调用。
        AmapVectorRenderProgressCallback progressCallback;
        // 返回 true 时尽快停止领取新任务。
        std::function<bool()> cancellationRequested;
        uint32_t tileSize = 256;
        int roadLineWidth = 4;
        int lineWidth = 2;
        int transitLineWidth = 3;
        int pointRadius = 5;
        bool exportBuildingsToFbx = false;
        std::filesystem::path buildingWallTexturePath =
            std::filesystem::u8path("贴图/墙体");
        std::filesystem::path buildingRoofTexturePath =
            std::filesystem::u8path("贴图/房顶");
        AmapBuildingFbxTextureMode buildingFbxWallTextureMode =
            AmapBuildingFbxTextureMode::Scattered;
        AmapBuildingFbxRoofTextureMode buildingFbxRoofTextureMode =
            AmapBuildingFbxRoofTextureMode::Scattered;
        AmapBuildingFbxAtlasImageFormat buildingFbxAtlasImageFormat =
            AmapBuildingFbxAtlasImageFormat::Png;
        std::vector<AmapBuildingFbxSatelliteRoofTexture> buildingFbxSatelliteRoofTextures;
        double buildingLayerHeightCentimeters = 45.;
        double buildingTextureUnitLayer = 5.;
        uint32_t buildingTextureUnitPixelSize = 64;
        uint32_t buildingMaxWallMaterialsPerBucket = 1;
        uint32_t buildingTextureRandomSeed = 0;

        double GetBuildingChunkSizeMeters() const { return buildingChunkSizeMeters_; }
        void SetBuildingChunkSizeMeters(double value) { buildingChunkSizeMeters_ = value; }

    protected:
        double buildingChunkSizeMeters_ = 2200.;
    };

    struct AmapVectorPngDownloadOptions
    {
        AmapVectorDownloadOptions vectorDownload;
        AmapVectorRenderOptions render;
    };

    struct AmapVectorPngRenderResult
    {
        bool success = false;
        std::string error;
        uint32_t renderedTileCount = 0;
        std::filesystem::path pngTileCachePath;
    };

    struct AmapVectorPngDownloadResult
    {
        bool success = false;
        std::string error;
        AmapVectorDownloadResult vectorDownload;
        AmapVectorPngRenderResult render;
    };

    // 该类将 GCJ-02 矢量要素转换到标准 Web Mercator PNG XYZ 瓦片。
    class AmapVectorPngDownloader : public GoogleDownBase
    {
    public:
        AmapVectorPngRenderResult Render(
            const AmapVectorDecodedData& decodedData,
            const AmapVectorDownloadOptions& vectorOptions,
            const AmapVectorRenderOptions& renderOptions);

        AmapVectorPngDownloadResult DownloadPngTiles(
            const AmapVectorPngDownloadOptions& options);
    };
}
