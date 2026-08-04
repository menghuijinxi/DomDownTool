#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace DownTool
{
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

    // geometryParts 为点、线或面环集合；坐标保持高德 Web 矢量服务使用的 GCJ-02。
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
    };

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
            std::string& error);
    };
}
