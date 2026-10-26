#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "DataType.h"

namespace DownTool
{
    enum class VectorCoordinateSystem
    {
        Gcj02,
        Wgs84
    };

    enum class VectorDataSource
    {
        Amap,
        Tencent,
        Shapefile,
        Baidu
    };

    enum class AmapVectorGeometryType
    {
        Point,
        LineString,
        Polygon
    };

    struct AmapVectorCoordinate
    {
        double longitude = 0.;
        double latitude = 0.;
    };

    struct AmapVectorName
    {
        std::string text;
        std::string language;
    };

    // 高德数据默认保持 GCJ-02；本地 SHP 保留原始 WGS84，避免往返转换损失精度。
    struct AmapVectorFeature
    {
        AmapVectorGeometryType geometryType = AmapVectorGeometryType::Point;
        std::vector<std::vector<AmapVectorCoordinate>> geometryParts;
        std::string layerName;
        uint32_t mainKey = 0;
        uint32_t subKey = 0;
        uint32_t resolution = 0;
        uint32_t minZoom = 0;
        uint32_t maxZoom = 0;
        uint32_t drawOrder = 0;
        uint32_t rank = 0;
        uint64_t uid = 0;
        std::vector<AmapVectorName> names;
        std::string shield;
        uint32_t shieldType = 0;
        bool hasHeight = false;
        uint64_t height = 0;
        std::string buildingIds;
        std::string internalType;

        VectorCoordinateSystem GetCoordinateSystem() const { return coordinateSystem_; }
        void SetCoordinateSystem(VectorCoordinateSystem value) { coordinateSystem_ = value; }
        VectorDataSource GetDataSource() const { return dataSource_; }
        void SetDataSource(VectorDataSource value) { dataSource_ = value; }
        const std::optional<double>& GetHeightMeters() const { return heightMeters_; }
        void SetHeightMeters(std::optional<double> value) { heightMeters_ = value; }
        const nlohmann::json& GetSourceProperties() const { return sourceProperties_; }
        void SetSourceProperties(const nlohmann::json& value) { sourceProperties_ = value; }

    protected:
        VectorCoordinateSystem coordinateSystem_ = VectorCoordinateSystem::Gcj02;
        VectorDataSource dataSource_ = VectorDataSource::Amap;
        std::optional<double> heightMeters_;
        nlohmann::json sourceProperties_;
    };

    [[nodiscard]] LonLat VectorCoordinateWgs84(
        const AmapVectorFeature& feature, const AmapVectorCoordinate& coordinate);
    [[nodiscard]] double VectorBuildingHeightCentimeters(
        const AmapVectorFeature& feature, double layerHeightCentimeters);
    [[nodiscard]] bool HasVectorBuildingHeight(const AmapVectorFeature& feature);

    struct AmapVectorDecodedData
    {
        std::string version;
        bool status = false;
        uint32_t packetCount = 0;
        std::vector<AmapVectorFeature> features;
    };

    class AmapVectorDecoder
    {
    public:
        static bool DecodeEnvelope(
            const std::string& envelope,
            AmapVectorDecodedData& decodedData,
            std::string& error);
    };

    class AmapVectorGeoJsonWriter
    {
    public:
        static bool WriteFile(
            const AmapVectorDecodedData& decodedData,
            const std::filesystem::path& outputPath,
            std::string& error,
            const std::function<bool()>& cancellationRequested = {});
    };
}
