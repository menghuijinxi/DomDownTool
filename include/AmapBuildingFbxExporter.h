#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "AmapVectorDecoder.h"
#include "DataType.h"

struct aiScene;

namespace DownTool
{
    enum class AmapBuildingFbxTextureMode
    {
        Scattered,
        MergedAtlas
    };

    enum class AmapBuildingFbxRoofTextureMode
    {
        Scattered,
        MergedAtlas,
        Satellite
    };

    enum class AmapBuildingFbxAtlasImageFormat
    {
        Png,
        Jpg
    };

    struct AmapBuildingFbxSatelliteRoofTexture
    {
        std::filesystem::path texturePath;
        double west = 0.;
        double south = 0.;
        double east = 0.;
        double north = 0.;
    };

    class AmapBuildingTextureSelection
    {
    public:
        AmapBuildingTextureSelection(
            size_t wallTextureIndex, std::optional<size_t> roofTextureIndex)
            : wallTextureIndex_(wallTextureIndex), roofTextureIndex_(roofTextureIndex)
        {
        }

        size_t GetWallTextureIndex() const { return wallTextureIndex_; }
        const std::optional<size_t>& GetRoofTextureIndex() const { return roofTextureIndex_; }

    protected:
        size_t wallTextureIndex_;
        std::optional<size_t> roofTextureIndex_;
    };

    // 与源 features 一一对应；无效建筑没有选图，分块时保留源索引，不依赖 UID。
    using AmapBuildingTextureSelections =
        std::vector<std::optional<AmapBuildingTextureSelection>>;

    struct AmapBuildingFbxExportOptions
    {
        std::filesystem::path outputPath;
        LonLat centerWgs84;
        FbxPlanetShape shape = FbxPlanetShape::Plane;
        std::filesystem::path wallTexturePath = std::filesystem::u8path("贴图/墙体");
        std::filesystem::path roofTexturePath = std::filesystem::u8path("贴图/房顶");
        AmapBuildingFbxTextureMode wallTextureMode = AmapBuildingFbxTextureMode::Scattered;
        AmapBuildingFbxRoofTextureMode roofTextureMode =
            AmapBuildingFbxRoofTextureMode::Scattered;
        AmapBuildingFbxAtlasImageFormat atlasImageFormat =
            AmapBuildingFbxAtlasImageFormat::Png;
        std::vector<AmapBuildingFbxSatelliteRoofTexture> satelliteRoofTextures;
        double layerHeightCentimeters = 45.;
        double textureUnitLayer = 5.;
        uint32_t textureUnitPixelSize = 64;
        uint32_t maxWallMaterialsPerBucket = 1;
        uint32_t randomSeed = 0;
        bool terrainHeightEnabled = false;
        double elevationReferenceMeters = 0.;
        std::function<std::optional<double>(const LonLat&)> elevationSampler;
        std::function<bool()> cancellationRequested;

        double GetChunkSizeMeters() const { return chunkSizeMeters_; }
        void SetChunkSizeMeters(double value) { chunkSizeMeters_ = value; }
        const AmapBuildingTextureSelections& GetTextureSelections() const
        {
            return textureSelections_;
        }
        void SetTextureSelections(const AmapBuildingTextureSelections& value)
        {
            textureSelections_ = value;
        }

    protected:
        double chunkSizeMeters_ = 2200.;
        AmapBuildingTextureSelections textureSelections_;
    };

    struct AmapBuildingFbxExportResult
    {
        bool success = false;
        std::string error;
        std::filesystem::path outputPath;
        uint32_t buildingCount = 0;
        uint32_t meshCount = 0;
        uint32_t materialCount = 0;
    };

    [[nodiscard]] AmapBuildingFbxExportResult ExportAmapBuildingsToFbx(
        const AmapVectorDecodedData& decodedData,
        const AmapBuildingFbxExportOptions& options);

    // 在简化前复用 FBX 规则选图；后续各层使用相同图库和对应 feature 的选图结果。
    [[nodiscard]] bool PrepareAmapBuildingTextureSelections(
        const AmapVectorDecodedData& decodedData,
        const AmapBuildingFbxExportOptions& options,
        AmapBuildingTextureSelections& selections,
        std::string& error);

    // 复用建筑几何和贴图生成，格式写入由调用方负责，不生成中间 FBX。
    using AmapBuildingSceneWriter =
        std::function<bool(aiScene&, std::string&)>;
    [[nodiscard]] AmapBuildingFbxExportResult ExportAmapBuildingScene(
        const AmapVectorDecodedData& decodedData,
        const AmapBuildingFbxExportOptions& options,
        const AmapBuildingSceneWriter& writer);
}
