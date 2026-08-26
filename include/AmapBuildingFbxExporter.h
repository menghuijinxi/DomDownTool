#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "AmapVectorDecoder.h"
#include "DataType.h"

namespace DownTool
{
    struct AmapBuildingFbxExportOptions
    {
        std::filesystem::path outputPath;
        LonLat centerWgs84;
        std::filesystem::path wallTexturePath = std::filesystem::u8path("贴图/墙体");
        std::filesystem::path roofTexturePath = std::filesystem::u8path("贴图/房顶");
        double layerHeightCentimeters = 45.;
        double textureUnitLayer = 5.;
        uint32_t textureUnitPixelSize = 64;
        uint32_t maxWallMaterialsPerBucket = 1;
        uint32_t randomSeed = 0;
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
