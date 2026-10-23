#include "AmapVectorDecoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "zlib.h"

#include "amap_vector_tile.pb.h"
#include "GeographicPolygon.h"
#include "GisSpatialIndex.h"

namespace
{
    using Json = nlohmann::json;

    constexpr size_t kMaximumUncompressedPacketBytes = 64u * 1024u * 1024u;

    struct LayerTileLocation
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t zoom = 0;
    };

    bool InflateGzip(
        const char* compressedData,
        size_t compressedSize,
        std::string& inflatedData,
        std::string& error)
    {
        if (compressedSize == 0)
        {
            error = "高德矢量包缺少 gzip 载荷。";
            return false;
        }

        z_stream stream{};
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
        {
            error = "无法初始化高德矢量 gzip 解码器。";
            return false;
        }

        std::array<char, 32 * 1024> outputBuffer{};
        size_t inputOffset = 0;
        int inflateResult = Z_OK;
        while (inflateResult == Z_OK)
        {
            if (stream.avail_in == 0 && inputOffset < compressedSize)
            {
                const size_t inputSize = std::min(
                    compressedSize - inputOffset,
                    static_cast<size_t>(std::numeric_limits<uInt>::max()));
                stream.next_in = reinterpret_cast<Bytef*>(
                    const_cast<char*>(compressedData + inputOffset));
                stream.avail_in = static_cast<uInt>(inputSize);
                inputOffset += inputSize;
            }

            stream.next_out = reinterpret_cast<Bytef*>(outputBuffer.data());
            stream.avail_out = static_cast<uInt>(outputBuffer.size());
            inflateResult = inflate(&stream, Z_NO_FLUSH);

            const size_t producedSize = outputBuffer.size() - stream.avail_out;
            if (inflatedData.size() > kMaximumUncompressedPacketBytes - producedSize)
            {
                inflateEnd(&stream);
                error = "高德矢量包解压后超过安全大小限制。";
                return false;
            }
            inflatedData.append(outputBuffer.data(), producedSize);

            if (inflateResult == Z_STREAM_END)
            {
                break;
            }
            if (inflateResult != Z_OK)
            {
                inflateEnd(&stream);
                error = "高德矢量 gzip 载荷无效。";
                return false;
            }
            if (stream.avail_in == 0 && inputOffset == compressedSize && producedSize == 0)
            {
                inflateEnd(&stream);
                error = "高德矢量 gzip 载荷不完整。";
                return false;
            }
        }

        const bool hasTrailingData = stream.avail_in != 0 || inputOffset != compressedSize;
        inflateEnd(&stream);
        if (hasTrailingData)
        {
            error = "高德矢量 gzip 载荷含有未消费的数据。";
            return false;
        }
        return true;
    }

    bool ToLonLat(
        const LayerTileLocation& tile,
        uint32_t resolution,
        int64_t localX,
        int64_t localY,
        DownTool::AmapVectorCoordinate& coordinate)
    {
        if (tile.zoom > 30 || resolution == 0 || resolution > 30)
        {
            return false;
        }

        const double tileCount = std::ldexp(1., static_cast<int>(tile.zoom));
        const double localXScale = std::ldexp(1., static_cast<int>(resolution) - 1);
        // 高德内部世界坐标的纬度范围是经度范围的一半，Y 轴局部尺度也相应减半。
        const double localYScale = std::ldexp(1., static_cast<int>(resolution) - 2);
        coordinate.longitude = -180. +
            (static_cast<double>(tile.x) + static_cast<double>(localX) / localXScale) *
                360. / tileCount;
        coordinate.latitude = 90. -
            (static_cast<double>(tile.y) + 1.) * 180. / tileCount +
            static_cast<double>(localY) / localYScale * 180. / tileCount;
        return std::isfinite(coordinate.longitude) && std::isfinite(coordinate.latitude) &&
            coordinate.longitude >= -180. && coordinate.longitude <= 180. &&
            coordinate.latitude >= -90. && coordinate.latitude <= 90.;
    }

    template<typename CoordinateList>
    bool DecodeDeltaGeometry(
        const CoordinateList& encodedGeometry,
        const LayerTileLocation& tile,
        uint32_t resolution,
        std::vector<DownTool::AmapVectorCoordinate>& coordinates)
    {
        if (encodedGeometry.size() < 2 || encodedGeometry.size() % 2 != 0)
        {
            return false;
        }

        int64_t localX = encodedGeometry.Get(0);
        int64_t localY = encodedGeometry.Get(1);
        DownTool::AmapVectorCoordinate coordinate;
        if (!ToLonLat(tile, resolution, localX, localY, coordinate))
        {
            return false;
        }
        coordinates.push_back(coordinate);

        for (int index = 2; index < encodedGeometry.size(); index += 2)
        {
            localX += encodedGeometry.Get(index);
            localY += encodedGeometry.Get(index + 1);
            if (!ToLonLat(tile, resolution, localX, localY, coordinate))
            {
                return false;
            }
            coordinates.push_back(coordinate);
        }
        return true;
    }

    template<typename CoordinateList>
    bool DecodePointGeometry(
        const CoordinateList& encodedPosition,
        const LayerTileLocation& tile,
        uint32_t resolution,
        std::vector<DownTool::AmapVectorCoordinate>& coordinates)
    {
        if (encodedPosition.size() < 2)
        {
            return false;
        }

        DownTool::AmapVectorCoordinate coordinate;
        if (!ToLonLat(
            tile,
            resolution,
            static_cast<int64_t>(encodedPosition.Get(0)),
            static_cast<int64_t>(encodedPosition.Get(1)),
            coordinate))
        {
            return false;
        }
        coordinates.push_back(coordinate);
        return true;
    }

    bool IsSameCoordinate(
        const DownTool::AmapVectorCoordinate& left,
        const DownTool::AmapVectorCoordinate& right)
    {
        return left.longitude == right.longitude && left.latitude == right.latitude;
    }

    void CloseRing(std::vector<DownTool::AmapVectorCoordinate>& coordinates)
    {
        if (!coordinates.empty() && !IsSameCoordinate(coordinates.front(), coordinates.back()))
        {
            coordinates.push_back(coordinates.front());
        }
    }

    template<typename Names>
    void AppendNames(const Names& names, std::vector<DownTool::AmapVectorName>& target)
    {
        for (const domdown::amap::Language& language : names)
        {
            if (!language.name().empty() || !language.language().empty())
            {
                target.push_back({ language.name(), language.language() });
            }
        }
    }

    DownTool::AmapVectorFeature MakeFeature(
        DownTool::AmapVectorGeometryType geometryType,
        const std::string& layerName,
        uint32_t mainKey,
        uint32_t subKey,
        uint32_t resolution,
        uint32_t minZoom,
        uint32_t maxZoom,
        uint32_t drawOrder,
        uint32_t rank,
        uint64_t uid)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = geometryType;
        feature.layerName = layerName;
        feature.mainKey = mainKey;
        feature.subKey = subKey;
        feature.resolution = resolution;
        feature.minZoom = minZoom;
        feature.maxZoom = maxZoom;
        feature.drawOrder = drawOrder;
        feature.rank = rank;
        feature.uid = uid;
        return feature;
    }

    bool IsValidFeatureGeometry(const DownTool::AmapVectorFeature& feature)
    {
        if (feature.geometryParts.empty())
        {
            return false;
        }

        if (feature.geometryType == DownTool::AmapVectorGeometryType::Point)
        {
            return feature.geometryParts.size() == 1 && feature.geometryParts.front().size() == 1;
        }
        if (feature.geometryType == DownTool::AmapVectorGeometryType::LineString)
        {
            return feature.geometryParts.size() == 1 && feature.geometryParts.front().size() >= 2;
        }

        return std::all_of(
            feature.geometryParts.begin(),
            feature.geometryParts.end(),
            [](const std::vector<DownTool::AmapVectorCoordinate>& ring)
            {
                return ring.size() >= 4 && IsSameCoordinate(ring.front(), ring.back());
            });
    }

    void AppendFeatureIfValid(
        DownTool::AmapVectorDecodedData& decodedData,
        DownTool::AmapVectorFeature feature)
    {
        if (IsValidFeatureGeometry(feature))
        {
            decodedData.features.push_back(std::move(feature));
        }
    }

    // 高德在 PBF 中省略协议默认值，proto3 标量 getter 会把这些字段读成 0。
    uint32_t NormalizeStyleValue(uint32_t value, uint32_t defaultValue)
    {
        return value == 0 ? defaultValue : value;
    }

    uint32_t NormalizeMinimumZoom(uint32_t value, uint32_t defaultValue = 2)
    {
        return NormalizeStyleValue(value, defaultValue);
    }

    uint32_t NormalizeMaximumZoom(uint32_t value)
    {
        return NormalizeStyleValue(value, 30);
    }

    void DecodeRoadLayer(
        const domdown::amap::Layer& layer,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        domdown::amap::RoadLayer roadLayer;
        if (!roadLayer.ParseFromString(layer.data()))
        {
            return;
        }

        const LayerTileLocation tile{ layer.x(), layer.y(), layer.z() };
        for (const domdown::amap::RoadLineSameStyle& style : roadLayer.road_lines())
        {
            const uint32_t mainKey = NormalizeStyleValue(style.main_key(), 20004);
            const uint32_t subKey = NormalizeStyleValue(style.sub_key(), 1);
            const uint32_t resolution = NormalizeStyleValue(style.resolution(), 12);
            for (const domdown::amap::RoadLineMulti& item : style.items())
            {
                for (const domdown::amap::RoadLine& road : item.features())
                {
                    DownTool::AmapVectorFeature feature = MakeFeature(
                        DownTool::AmapVectorGeometryType::LineString,
                        "road",
                        mainKey,
                        subKey,
                        resolution,
                        NormalizeMinimumZoom(item.min_zoom()),
                        NormalizeMaximumZoom(item.max_zoom()),
                        item.draw_order(),
                        road.rank(),
                        road.uid());
                    feature.shield = road.shield();
                    feature.shieldType = road.shield_type();
                    AppendNames(road.names(), feature.names);
                    AppendNames(road.alternative_names(), feature.names);
                    AppendNames(road.names_8(), feature.names);
                    AppendNames(road.names_9(), feature.names);
                    std::vector<DownTool::AmapVectorCoordinate> coordinates;
                    if (DecodeDeltaGeometry(road.line(), tile, resolution, coordinates))
                    {
                        feature.geometryParts.push_back(std::move(coordinates));
                    }
                    AppendFeatureIfValid(decodedData, std::move(feature));
                }
            }
        }
    }

    void DecodeLineStyles(
        const google::protobuf::RepeatedPtrField<domdown::amap::LineFeatureSameStyle>& styles,
        const LayerTileLocation& tile,
        const std::string& layerName,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        for (const domdown::amap::LineFeatureSameStyle& style : styles)
        {
            const uint32_t mainKey = NormalizeStyleValue(style.main_key(), 20016);
            const uint32_t subKey = NormalizeStyleValue(style.sub_key(), 1);
            const uint32_t resolution = NormalizeStyleValue(style.resolution(), 12);
            for (const domdown::amap::LineFeatureMulti& item : style.items())
            {
                for (const domdown::amap::LineFeature& line : item.features())
                {
                    DownTool::AmapVectorFeature feature = MakeFeature(
                        DownTool::AmapVectorGeometryType::LineString,
                        layerName,
                        mainKey,
                        subKey,
                        resolution,
                        NormalizeMinimumZoom(item.min_zoom()),
                        NormalizeMaximumZoom(item.max_zoom()),
                        item.draw_order(),
                        line.rank(),
                        line.uid());
                    AppendNames(line.names(), feature.names);
                    AppendNames(line.alternative_names(), feature.names);
                    AppendNames(line.names_6(), feature.names);
                    AppendNames(line.names_7(), feature.names);
                    std::vector<DownTool::AmapVectorCoordinate> coordinates;
                    if (DecodeDeltaGeometry(line.line(), tile, resolution, coordinates))
                    {
                        feature.geometryParts.push_back(std::move(coordinates));
                    }
                    AppendFeatureIfValid(decodedData, std::move(feature));
                }
            }
        }
    }

    void DecodePolygonStyles(
        const google::protobuf::RepeatedPtrField<domdown::amap::PolygonFeatureSameStyle>& styles,
        const LayerTileLocation& tile,
        const std::string& layerName,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        for (const domdown::amap::PolygonFeatureSameStyle& style : styles)
        {
            const uint32_t mainKey = NormalizeStyleValue(style.main_key(), 30001);
            const uint32_t subKey = NormalizeStyleValue(style.sub_key(), 1);
            const uint32_t resolution = NormalizeStyleValue(style.resolution(), 12);
            for (const domdown::amap::PolygonFeature& polygon : style.items())
            {
                DownTool::AmapVectorFeature feature = MakeFeature(
                    DownTool::AmapVectorGeometryType::Polygon,
                    layerName,
                    mainKey,
                    subKey,
                    resolution,
                    NormalizeMinimumZoom(polygon.min_zoom()),
                    NormalizeMaximumZoom(polygon.max_zoom()),
                    polygon.draw_order(),
                    polygon.rank(),
                    polygon.uid());
                AppendNames(polygon.names(), feature.names);
                AppendNames(polygon.names_7(), feature.names);
                for (const domdown::amap::GeometryPolygon& polygonGeometry : polygon.polygons())
                {
                    std::vector<DownTool::AmapVectorCoordinate> ring;
                    if (DecodeDeltaGeometry(
                        polygonGeometry.polygon(),
                        tile,
                        resolution,
                        ring))
                    {
                        CloseRing(ring);
                        feature.geometryParts.push_back(std::move(ring));
                    }
                }
                AppendFeatureIfValid(decodedData, std::move(feature));
            }
        }
    }

    void DecodePointStyles(
        const google::protobuf::RepeatedPtrField<domdown::amap::PointFeatureSameStyle>& styles,
        const LayerTileLocation& tile,
        const std::string& layerName,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        for (const domdown::amap::PointFeatureSameStyle& style : styles)
        {
            const uint32_t mainKey = NormalizeStyleValue(style.main_key(), 12024);
            const uint32_t subKey = NormalizeStyleValue(style.sub_key(), 1);
            const uint32_t resolution = NormalizeStyleValue(style.resolution(), 12);
            for (const domdown::amap::PointFeatureMulti& item : style.items())
            {
                for (const domdown::amap::PointFeature& point : item.features())
                {
                    DownTool::AmapVectorFeature feature = MakeFeature(
                        DownTool::AmapVectorGeometryType::Point,
                        layerName,
                        mainKey,
                        subKey,
                        resolution,
                        NormalizeMinimumZoom(item.min_zoom(), 18),
                        NormalizeMaximumZoom(item.max_zoom()),
                        point.draw_order(),
                        item.rank(),
                        point.uid());
                    feature.internalType = point.internal_type();
                    AppendNames(point.names(), feature.names);
                    AppendNames(point.alternative_names(), feature.names);
                    AppendNames(point.names_7(), feature.names);
                    AppendNames(point.names_8(), feature.names);
                    std::vector<DownTool::AmapVectorCoordinate> coordinates;
                    if (DecodePointGeometry(point.position(), tile, resolution, coordinates))
                    {
                        feature.geometryParts.push_back(std::move(coordinates));
                    }
                    AppendFeatureIfValid(decodedData, std::move(feature));
                }
            }
        }
    }

    void DecodeRegionLayer(
        const domdown::amap::Layer& layer,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        domdown::amap::RegionLayer regionLayer;
        if (!regionLayer.ParseFromString(layer.data()))
        {
            return;
        }

        const LayerTileLocation tile{ layer.x(), layer.y(), layer.z() };
        DecodePolygonStyles(regionLayer.polygons(), tile, "region", decodedData);
        DecodeLineStyles(regionLayer.lines(), tile, "region-line", decodedData);
    }

    void DecodeBuildingLayer(
        const domdown::amap::Layer& layer,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        domdown::amap::BuildingLayer buildingLayer;
        if (!buildingLayer.ParseFromString(layer.data()))
        {
            return;
        }

        const LayerTileLocation tile{ layer.x(), layer.y(), layer.z() };
        for (const domdown::amap::BuildingSameStyle& style : buildingLayer.buildings())
        {
            const uint32_t mainKey = NormalizeStyleValue(style.main_key(), 55001);
            const uint32_t subKey = NormalizeStyleValue(style.sub_key(), 1);
            const uint32_t resolution = NormalizeStyleValue(style.resolution(), 12);
            for (const domdown::amap::BuildingFeatureMulti& item : style.items())
            {
                for (const domdown::amap::BuildingFeature& building : item.features())
                {
                    DownTool::AmapVectorFeature feature = MakeFeature(
                        DownTool::AmapVectorGeometryType::Polygon,
                        "building",
                        mainKey,
                        subKey,
                        resolution,
                        NormalizeMinimumZoom(item.min_zoom(), 15),
                        NormalizeMaximumZoom(item.max_zoom()),
                        item.draw_order(),
                        building.rank(),
                        building.uid());
                    // proto3 标量字段没有 presence；高德建筑层的该字段语义为高度。
                    feature.hasHeight = true;
                    feature.height = building.height();
                    feature.buildingIds = building.building_ids();
                    AppendNames(building.names(), feature.names);
                    AppendNames(building.names_7(), feature.names);
                    AppendNames(building.names_8(), feature.names);
                    AppendNames(building.names_9(), feature.names);
                    std::vector<DownTool::AmapVectorCoordinate> ring;
                    if (DecodeDeltaGeometry(
                        building.polygon(),
                        tile,
                        resolution,
                        ring))
                    {
                        CloseRing(ring);
                        feature.geometryParts.push_back(std::move(ring));
                    }
                    AppendFeatureIfValid(decodedData, std::move(feature));
                }
            }
        }
    }

    void DecodePoiLayer(
        const domdown::amap::Layer& layer,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        domdown::amap::PoiLayer poiLayer;
        if (!poiLayer.ParseFromString(layer.data()))
        {
            return;
        }

        DecodePointStyles(
            poiLayer.points(),
            { layer.x(), layer.y(), layer.z() },
            "poi",
            decodedData);
    }

    void DecodeTransitLayer(
        const domdown::amap::Layer& layer,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        domdown::amap::TransitLayer transitLayer;
        if (!transitLayer.ParseFromString(layer.data()))
        {
            return;
        }

        const LayerTileLocation tile{ layer.x(), layer.y(), layer.z() };
        DecodeLineStyles(transitLayer.lines(), tile, "transit-line", decodedData);
        DecodePointStyles(transitLayer.points(), tile, "transit-point", decodedData);
        DecodePolygonStyles(transitLayer.polygons(), tile, "transit-polygon", decodedData);
    }

    void DecodeResult(
        const domdown::amap::Result& result,
        DownTool::AmapVectorDecodedData& decodedData)
    {
        if (decodedData.version.empty())
        {
            decodedData.version = result.version();
        }
        decodedData.status = decodedData.status || result.status();

        for (const domdown::amap::Tile& tile : result.tiles())
        {
            (void)tile;
            for (const domdown::amap::Layer& layer : tile.layers())
            {
                switch (layer.type())
                {
                case 0:
                    DecodePoiLayer(layer, decodedData);
                    break;
                case 1:
                    DecodeRoadLayer(layer, decodedData);
                    break;
                case 2:
                    DecodeRegionLayer(layer, decodedData);
                    break;
                case 3:
                    DecodeBuildingLayer(layer, decodedData);
                    break;
                case 4:
                    DecodeTransitLayer(layer, decodedData);
                    break;
                default:
                    break;
                }
            }
        }
    }

    Json BuildCoordinateArray(
        const std::vector<DownTool::AmapVectorCoordinate>& coordinates,
        const std::function<bool()>& cancellationRequested)
    {
        Json coordinateArray = Json::array();
        for (const DownTool::AmapVectorCoordinate& coordinate : coordinates)
        {
            if (cancellationRequested && cancellationRequested())
            {
                break;
            }
            coordinateArray.push_back(
                Json::array({coordinate.longitude, coordinate.latitude}));
        }
        return coordinateArray;
    }

    Json BuildWgs84PolygonGeometry(
        const DownTool::AmapVectorFeature& feature,
        const std::function<bool()>& cancellationRequested)
    {
        const size_t count = feature.geometryParts.size();
        std::vector<DownTool::Wgs84Ring> rings(count);
        std::vector<DownTool::GisSpatialBounds> bounds(count);
        std::vector<double> areas(count);
        for (size_t index = 0; index < count; ++index)
        {
            for (const auto& point : feature.geometryParts[index])
            {
                if (rings[index].size() % 256 == 0 &&
                    cancellationRequested && cancellationRequested())
                {
                    return {};
                }
                rings[index].emplace_back(point.longitude, point.latitude);
                bounds[index].Add(point.longitude, point.latitude);
            }
            const auto& ring = rings[index];
            for (size_t vertex = 1; vertex < ring.size(); ++vertex)
            {
                areas[index] += (ring[vertex - 1].lon - ring.front().lon) *
                                    (ring[vertex].lat - ring.front().lat) -
                                (ring[vertex].lon - ring.front().lon) *
                                    (ring[vertex - 1].lat - ring.front().lat);
            }
            areas[index] = std::abs(areas[index]);
        }
        DownTool::GisSpatialIndex spatialIndex;
        if (!spatialIndex.Build(std::move(bounds), cancellationRequested))
        {
            return {};
        }
        std::vector<size_t> parents(count, count);
        for (size_t index = 0; index < count; ++index)
        {
            const auto& point = rings[index].front();
            std::vector<size_t> candidates;
            if (!spatialIndex.Query({point.lon, point.lat, point.lon, point.lat},
                                    candidates, cancellationRequested))
            {
                return {};
            }
            for (size_t candidate : candidates)
            {
                if (areas[candidate] > areas[index] &&
                    (parents[index] == count || areas[candidate] < areas[parents[index]]) &&
                    spatialIndex.GetBounds(candidate).Contains(spatialIndex.GetBounds(index)) &&
                    DownTool::GeographicRingContainsPoint(rings[candidate], point))
                {
                    parents[index] = candidate;
                }
            }
        }
        std::vector<std::vector<size_t>> children(count);
        for (size_t index = 0; index < count; ++index)
        {
            if (parents[index] < count)
            {
                children[parents[index]].push_back(index);
            }
        }
        Json multipolygon = Json::array();
        for (size_t index = 0; index < count; ++index)
        {
            if (cancellationRequested && cancellationRequested())
            {
                return {};
            }
            size_t depth = 0;
            for (size_t parent = parents[index]; parent < count; parent = parents[parent])
            {
                ++depth;
            }
            if (depth % 2 != 0)
            {
                continue;
            }
            Json polygon = Json::array(
                {BuildCoordinateArray(feature.geometryParts[index], cancellationRequested)});
            for (size_t hole : children[index])
            {
                polygon.push_back(BuildCoordinateArray(
                    feature.geometryParts[hole], cancellationRequested));
            }
            multipolygon.push_back(std::move(polygon));
        }
        return multipolygon.size() == 1
                   ? Json{{"type", "Polygon"}, {"coordinates", multipolygon.front()}}
                   : Json{{"type", "MultiPolygon"}, {"coordinates", std::move(multipolygon)}};
    }

    Json BuildFeatureJson(
        const DownTool::AmapVectorFeature& feature,
        const std::function<bool()>& cancellationRequested)
    {
        Json properties = {
            {"layer", feature.layerName},
            {"mainKey", static_cast<int>(feature.mainKey)},
            {"subKey", static_cast<int>(feature.subKey)},
            {"resolution", static_cast<int>(feature.resolution)},
            {"minZoom", static_cast<int>(feature.minZoom)},
            {"maxZoom", static_cast<int>(feature.maxZoom)},
            {"drawOrder", static_cast<int>(feature.drawOrder)},
            {"rank", static_cast<int>(feature.rank)},
            {"uid", static_cast<uint64_t>(feature.uid)}};

        Json names = Json::array();
        for (const DownTool::AmapVectorName& name : feature.names)
        {
            if (cancellationRequested && cancellationRequested())
            {
                break;
            }
            names.push_back({{"text", name.text}, {"language", name.language}});
        }
        properties["names"] = std::move(names);
        if (!feature.shield.empty())
        {
            properties["shield"] = feature.shield;
        }
        if (feature.shieldType != 0)
        {
            properties["shieldType"] = static_cast<int>(feature.shieldType);
        }
        if (feature.hasHeight)
        {
            properties["height"] = static_cast<uint64_t>(feature.height);
        }
        if (!feature.buildingIds.empty())
        {
            properties["buildingIds"] = feature.buildingIds;
        }
        if (!feature.internalType.empty())
        {
            properties["internalType"] = feature.internalType;
        }
        if (feature.GetSourceProperties().is_object())
        {
            properties.update(feature.GetSourceProperties());
        }
        if (feature.GetHeightMeters())
        {
            properties["height_m"] = *feature.GetHeightMeters();
        }

        Json geometry;
        if (feature.geometryType == DownTool::AmapVectorGeometryType::Point)
        {
            Json points = Json::array();
            for (const auto& part : feature.geometryParts)
            {
                for (const auto& coordinate : part)
                {
                    points.push_back({coordinate.longitude, coordinate.latitude});
                }
            }
            geometry = points.size() == 1
                           ? Json{{"type", "Point"}, {"coordinates", points.front()}}
                           : Json{{"type", "MultiPoint"}, {"coordinates", std::move(points)}};
        }
        else if (feature.geometryType == DownTool::AmapVectorGeometryType::LineString)
        {
            Json lines = Json::array();
            for (const auto& part : feature.geometryParts)
            {
                lines.push_back(BuildCoordinateArray(part, cancellationRequested));
            }
            geometry = lines.size() == 1
                           ? Json{{"type", "LineString"}, {"coordinates", lines.front()}}
                           : Json{{"type", "MultiLineString"}, {"coordinates", std::move(lines)}};
        }
        else
        {
            Json rings = Json::array();
            for (const std::vector<DownTool::AmapVectorCoordinate>& ring : feature.geometryParts)
            {
                if (cancellationRequested && cancellationRequested())
                {
                    break;
                }
                rings.push_back(BuildCoordinateArray(ring, cancellationRequested));
            }
            geometry = {{"type", "Polygon"}, {"coordinates", std::move(rings)}};
            if (feature.GetCoordinateSystem() == DownTool::VectorCoordinateSystem::Wgs84 &&
                feature.geometryParts.size() > 1)
            {
                // SHP 允许一个记录包含多个外环；GeoJSON 需要按包含关系组织 MultiPolygon。
                geometry = BuildWgs84PolygonGeometry(feature, cancellationRequested);
            }
        }

        if (feature.geometryParts.empty())
        {
            geometry = nullptr;
        }
        return {{"type", "Feature"},
                {"properties", std::move(properties)},
                {"geometry", std::move(geometry)}};
    }
}

bool DownTool::AmapVectorDecoder::DecodeEnvelope(
    const std::string& envelope,
    AmapVectorDecodedData& decodedData,
    std::string& error)
{
    if (envelope.empty())
    {
        error = "高德矢量下载结果为空。";
        return false;
    }

    AmapVectorDecodedData result;
    size_t offset = 0;
    while (offset < envelope.size())
    {
        if (envelope.size() - offset < 4)
        {
            error = "高德矢量包缺少四字节长度头。";
            return false;
        }

        const unsigned char* header = reinterpret_cast<const unsigned char*>(
            envelope.data() + offset);
        const uint32_t compressedSize =
            (static_cast<uint32_t>(header[0]) << 24) |
            (static_cast<uint32_t>(header[1]) << 16) |
            (static_cast<uint32_t>(header[2]) << 8) |
            static_cast<uint32_t>(header[3]);
        offset += 4;
        if (compressedSize == 0 || compressedSize > envelope.size() - offset)
        {
            error = "高德矢量包的 gzip 长度无效。";
            return false;
        }

        std::string protobufData;
        if (!InflateGzip(
            envelope.data() + offset,
            compressedSize,
            protobufData,
            error))
        {
            return false;
        }
        offset += compressedSize;
        if (protobufData.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            error = "高德矢量 Protobuf 包超过解析器大小限制。";
            return false;
        }

        domdown::amap::Result protobufResult;
        if (!protobufResult.ParseFromArray(
            protobufData.data(), static_cast<int>(protobufData.size())))
        {
            error = "高德矢量 Protobuf 数据无法解析。";
            return false;
        }
        DecodeResult(protobufResult, result);
        result.packetCount++;
    }

    decodedData = std::move(result);
    error.clear();
    return true;
}

bool DownTool::AmapVectorGeoJsonWriter::WriteFile(
    const AmapVectorDecodedData& decodedData,
    const std::filesystem::path& outputPath,
    std::string& error,
    const std::function<bool()>& cancellationRequested)
{
    if (outputPath.empty())
    {
        error = "GeoJSON 输出路径不能为空。";
        return false;
    }

    const bool outputWgs84 = decodedData.version == "WGS84 SHP" ||
        decodedData.version.find("Tencent") != std::string::npos ||
        std::ranges::any_of(decodedData.features, [](const auto& feature)
    {
        return feature.GetCoordinateSystem() == VectorCoordinateSystem::Wgs84;
    });
    Json features = Json::array();
    for (const AmapVectorFeature& feature : decodedData.features)
    {
        if (cancellationRequested && cancellationRequested())
        {
            error = "GeoJSON 导出已取消。";
            return false;
        }
        if (outputWgs84 && feature.GetCoordinateSystem() != VectorCoordinateSystem::Wgs84)
        {
            auto converted = feature;
            for (auto& part : converted.geometryParts)
            {
                for (auto& coordinate : part)
                {
                    if (cancellationRequested && cancellationRequested())
                    {
                        error = "GeoJSON 导出已取消。";
                        return false;
                    }
                    const auto wgs84 = VectorCoordinateWgs84(feature, coordinate);
                    coordinate = {wgs84.lon, wgs84.lat};
                }
            }
            converted.SetCoordinateSystem(VectorCoordinateSystem::Wgs84);
            features.push_back(BuildFeatureJson(converted, cancellationRequested));
        }
        else
        {
            features.push_back(BuildFeatureJson(feature, cancellationRequested));
        }
    }

    if (cancellationRequested && cancellationRequested())
    {
        error = "GeoJSON 导出已取消。";
        return false;
    }

    const Json root = {
        {"type", "FeatureCollection"},
        {"coordinateSystem", outputWgs84 ? "WGS84" : "GCJ-02"},
        {"sourceFormat", outputWgs84 ? "GIS vector data" : "AMap web vector tile v2"},
        {"packetCount", static_cast<int>(decodedData.packetCount)},
        {"version", decodedData.version},
        {"features", std::move(features)}};

    std::error_code directoryError;
    const std::filesystem::path parentPath = outputPath.parent_path();
    if (!parentPath.empty())
    {
        std::filesystem::create_directories(parentPath, directoryError);
        if (directoryError)
        {
            error = "无法创建 GeoJSON 输出目录。";
            return false;
        }
    }

    const std::string outputJson = root.dump();
    if (cancellationRequested && cancellationRequested())
    {
        error = "GeoJSON 导出已取消。";
        return false;
    }

    std::ofstream outputFile(outputPath, std::ios::binary | std::ios::trunc);
    if (!outputFile.is_open())
    {
        error = "无法打开 GeoJSON 输出文件。";
        return false;
    }
    outputFile << outputJson << '\n';
    if (!outputFile.good())
    {
        error = "写入 GeoJSON 输出文件失败。";
        return false;
    }

    error.clear();
    return true;
}
