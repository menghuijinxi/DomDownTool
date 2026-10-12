#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "AmapBuildingFbxExporter.h"
#include "GeographicPolygon.h"

namespace DownTool
{
class CesiumExportOptions
{
public:
    const std::filesystem::path& GetOutputPath() const { return outputPath_; }
    void SetOutputPath(const std::filesystem::path& value) { outputPath_ = value; }
    const GeographicBounds& GetBounds() const { return bounds_; }
    void SetBounds(const GeographicBounds& value) { bounds_ = value; }
    const Wgs84Polygons& GetPolygons() const { return polygons_; }
    void SetPolygons(const Wgs84Polygons& value) { polygons_ = value; }
    double GetSampleMeters() const { return sampleMeters_; }
    void SetSampleMeters(double value) { sampleMeters_ = value; }
    double GetErrorMeters() const { return errorMeters_; }
    void SetErrorMeters(double value) { errorMeters_ = value; }
    double GetHeightOffsetMeters() const { return heightOffsetMeters_; }
    void SetHeightOffsetMeters(double value) { heightOffsetMeters_ = value; }
    const std::filesystem::path& GetGeoidPath() const { return geoidPath_; }
    void SetGeoidPath(const std::filesystem::path& value) { geoidPath_ = value; }
    const std::function<std::optional<double>(const LonLat&)>& GetSampler() const
    {
        return sampler_;
    }
    void SetSampler(const std::function<std::optional<double>(const LonLat&)>& value)
    {
        sampler_ = value;
    }
    const std::function<bool()>& GetCancellation() const { return cancellation_; }
    void SetCancellation(const std::function<bool()>& value) { cancellation_ = value; }
    const std::function<void(uint64_t, uint64_t)>& GetProgress() const { return progress_; }
    void SetProgress(const std::function<void(uint64_t, uint64_t)>& value)
    {
        progress_ = value;
    }

protected:
    std::filesystem::path outputPath_;
    GeographicBounds bounds_;
    Wgs84Polygons polygons_;
    double sampleMeters_ = 30.;
    double errorMeters_ = 1.;
    double heightOffsetMeters_ = 0.;
    std::filesystem::path geoidPath_;
    std::function<std::optional<double>(const LonLat&)> sampler_;
    std::function<bool()> cancellation_;
    std::function<void(uint64_t, uint64_t)> progress_;
};

class CesiumBuildingOptions : public CesiumExportOptions
{
public:
    const AmapBuildingFbxExportOptions& GetBuildingOptions() const { return building_; }
    void SetBuildingOptions(const AmapBuildingFbxExportOptions& value) { building_ = value; }
    uint32_t GetLodLevels() const { return lodLevels_; }
    void SetLodLevels(uint32_t value) { lodLevels_ = value; }
    uint32_t GetAtlasSize() const { return atlasSize_; }
    void SetAtlasSize(uint32_t value) { atlasSize_ = value; }
    bool GetTextureMipmaps() const { return textureMipmaps_; }
    void SetTextureMipmaps(bool value) { textureMipmaps_ = value; }

protected:
    AmapBuildingFbxExportOptions building_;
    uint32_t lodLevels_ = 4;
    uint32_t atlasSize_ = 1024;
    bool textureMipmaps_ = false;
};

[[nodiscard]] bool ExportCesiumTerrain(
    const CesiumExportOptions& options, std::string& error);
[[nodiscard]] bool ExportCesiumBuildings(
    const AmapVectorDecodedData& buildings,
    const CesiumBuildingOptions& options, std::string& error);
} // namespace DownTool
