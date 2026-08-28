#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "AmapVectorDecoder.h"
#include "DataType.h"

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

    struct AmapBuildingFbxExportOptions
    {
        std::filesystem::path outputPath;
        LonLat centerWgs84;
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
}
