#include <array>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "cpl_json.h"
#include "zlib.h"

#include "amap_vector_tile.pb.h"

#include "AmapVectorDecoder.h"
#include "AmapVectorTileGrid.h"

namespace
{
    bool Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    bool GzipData(const std::string& source, std::string& gzipData)
    {
        z_stream stream{};
        if (deflateInit2(
            &stream,
            Z_DEFAULT_COMPRESSION,
            Z_DEFLATED,
            16 + MAX_WBITS,
            8,
            Z_DEFAULT_STRATEGY) != Z_OK)
        {
            return false;
        }

        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(source.data()));
        stream.avail_in = static_cast<uInt>(source.size());
        std::array<char, 4096> buffer{};
        int deflateResult = Z_OK;
        while (deflateResult == Z_OK)
        {
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());
            deflateResult = deflate(&stream, Z_FINISH);
            gzipData.append(buffer.data(), buffer.size() - stream.avail_out);
        }
        deflateEnd(&stream);
        return deflateResult == Z_STREAM_END;
    }

    bool BuildEnvelope(const domdown::amap::Result& protobufResult, std::string& envelope)
    {
        std::string protobufData;
        if (!protobufResult.SerializeToString(&protobufData))
        {
            return false;
        }

        std::string gzipData;
        if (!GzipData(protobufData, gzipData) || gzipData.size() > UINT32_MAX)
        {
            return false;
        }

        const uint32_t compressedSize = static_cast<uint32_t>(gzipData.size());
        envelope.clear();
        envelope.reserve(gzipData.size() + 4);
        envelope.push_back(static_cast<char>((compressedSize >> 24) & 0xFF));
        envelope.push_back(static_cast<char>((compressedSize >> 16) & 0xFF));
        envelope.push_back(static_cast<char>((compressedSize >> 8) & 0xFF));
        envelope.push_back(static_cast<char>(compressedSize & 0xFF));
        envelope += gzipData;
        return true;
    }

    domdown::amap::Result BuildSyntheticResult(
        const DownTool::AmapVectorTile& tileCoordinates)
    {
        domdown::amap::Result result;
        result.set_status(true);
        result.set_version("unit-test");
        domdown::amap::Tile* tile = result.add_tiles();
        tile->set_x(tileCoordinates.x);
        tile->set_y(tileCoordinates.y);
        tile->set_z(tileCoordinates.zoom);

        domdown::amap::PoiLayer poiLayer;
        domdown::amap::PointFeatureSameStyle* poiStyle = poiLayer.add_points();
        poiStyle->set_main_key(12024);
        poiStyle->set_sub_key(1);
        poiStyle->set_resolution(12);
        domdown::amap::PointFeatureMulti* poiItem = poiStyle->add_items();
        poiItem->set_min_zoom(14);
        poiItem->set_max_zoom(20);
        poiItem->set_rank(9);
        domdown::amap::PointFeature* poi = poiItem->add_features();
        poi->set_draw_order(10);
        poi->set_internal_type("unit-poi");
        poi->set_uid(101);
        poi->add_position(1024);
        poi->add_position(1024);
        domdown::amap::Language* poiName = poi->add_names();
        poiName->set_name("测试点");
        poiName->set_language("zh-CN");

        std::string poiData;
        poiLayer.SerializeToString(&poiData);
        domdown::amap::Layer* poiTileLayer = tile->add_layers();
        poiTileLayer->set_x(tileCoordinates.x);
        poiTileLayer->set_y(tileCoordinates.y);
        poiTileLayer->set_z(tileCoordinates.zoom);
        poiTileLayer->set_type(0);
        poiTileLayer->set_data(poiData);

        domdown::amap::BuildingLayer buildingLayer;
        domdown::amap::BuildingSameStyle* buildingStyle = buildingLayer.add_buildings();
        buildingStyle->set_main_key(55001);
        buildingStyle->set_sub_key(1);
        buildingStyle->set_resolution(12);
        domdown::amap::BuildingFeatureMulti* buildingItem = buildingStyle->add_items();
        buildingItem->set_min_zoom(15);
        buildingItem->set_max_zoom(20);
        buildingItem->set_draw_order(3);
        domdown::amap::BuildingFeature* building = buildingItem->add_features();
        building->set_rank(5);
        building->set_uid(202);
        building->set_height(18);
        building->set_building_ids("unit-building");
        const std::array<int32_t, 10> ringDelta = {
            800, 800, 400, 0, 0, 400, -400, 0, 0, -400
        };
        for (const int32_t value : ringDelta)
        {
            building->add_polygon(value);
        }

        std::string buildingData;
        buildingLayer.SerializeToString(&buildingData);
        domdown::amap::Layer* buildingTileLayer = tile->add_layers();
        buildingTileLayer->set_x(tileCoordinates.x);
        buildingTileLayer->set_y(tileCoordinates.y);
        buildingTileLayer->set_z(tileCoordinates.zoom);
        buildingTileLayer->set_type(3);
        buildingTileLayer->set_data(buildingData);
        return result;
    }

    bool RunTest()
    {
        // 该点为 WGS84；经 GCJ-02 偏移后仍在单个 z14 高德矢量瓦片内。
        const DownTool::LonLat centerWgs84(116.0500, 39.9760);
        const std::vector<DownTool::AmapVectorTile> selectedTiles =
            DownTool::AmapVectorTileGrid::SelectTilesForWgs84Radius(
                centerWgs84, 25., 14);
        if (!Require(selectedTiles.size() == 1, "25 米半径不应跨越 z14 高德矢量瓦片") ||
            !Require(
                selectedTiles.front().x == 13473 &&
                    selectedTiles.front().y == 4553 &&
                    selectedTiles.front().zoom == 14,
                "WGS84 中心点没有映射到预期的 GCJ-02 高德矢量瓦片"))
        {
            return false;
        }

        std::string envelope;
        if (!Require(
            BuildEnvelope(BuildSyntheticResult(selectedTiles.front()), envelope),
            "无法构造合成高德 Protobuf 信封"))
        {
            return false;
        }

        DownTool::AmapVectorDecodedData decodedData;
        std::string error;
        if (!Require(
            DownTool::AmapVectorDecoder::DecodeEnvelope(envelope, decodedData, error),
            "合成高德 Protobuf 信封解码失败：" + error) ||
            !Require(decodedData.status, "合成高德 Protobuf 的 status 应为 true") ||
            !Require(decodedData.packetCount == 1, "合成高德 Protobuf 应只包含一个包") ||
            !Require(decodedData.features.size() == 2, "合成高德 Protobuf 应导出两个要素"))
        {
            return false;
        }

        const auto buildingFeature = std::find_if(
            decodedData.features.begin(),
            decodedData.features.end(),
            [](const DownTool::AmapVectorFeature& feature)
            {
                return feature.layerName == "building";
            });
        if (!Require(buildingFeature != decodedData.features.end(), "缺少建筑要素") ||
            !Require(buildingFeature->hasHeight && buildingFeature->height == 18,
                "建筑高度没有从 Protobuf 保留") ||
            !Require(
                buildingFeature->geometryParts.size() == 1 &&
                    buildingFeature->geometryParts.front().size() == 5 &&
                    buildingFeature->geometryParts.front().front().longitude ==
                        buildingFeature->geometryParts.front().back().longitude &&
                    buildingFeature->geometryParts.front().front().latitude ==
                        buildingFeature->geometryParts.front().back().latitude,
                "建筑多边形没有闭合"))
        {
            return false;
        }

        const std::filesystem::path outputRoot =
            std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) / "tmp" / "amap-vector-tests";
        std::error_code removeError;
        std::filesystem::remove_all(outputRoot, removeError);
        if (!Require(!removeError, "无法清理高德矢量测试目录"))
        {
            return false;
        }
        const std::filesystem::path outputPath = outputRoot / "small-radius.geojson";
        if (!Require(
            DownTool::AmapVectorGeoJsonWriter::WriteFile(decodedData, outputPath, error),
            "GeoJSON 写出失败：" + error))
        {
            return false;
        }

        std::ifstream inputFile(outputPath, std::ios::binary);
        const std::string geoJson(
            (std::istreambuf_iterator<char>(inputFile)),
            std::istreambuf_iterator<char>());
        CPLJSONDocument document;
        if (!Require(document.LoadMemory(geoJson), "写出的 GeoJSON 不是有效 JSON"))
        {
            return false;
        }
        const CPLJSONObject root = document.GetRoot();
        const CPLJSONArray features = root.GetArray("features");
        if (!Require(root.GetString("type") == "FeatureCollection", "输出不是 FeatureCollection") ||
            !Require(root.GetString("coordinateSystem") == "GCJ-02", "输出坐标系标记错误") ||
            !Require(features.Size() == 2, "输出 GeoJSON 要素数量错误"))
        {
            return false;
        }

        DownTool::AmapVectorDecodedData invalidData;
        if (!Require(
            !DownTool::AmapVectorDecoder::DecodeEnvelope("\x00\x00\x00\x00", invalidData, error),
            "空 gzip 包必须被拒绝"))
        {
            return false;
        }

        std::cout << "AmapVectorGeoJsonTest passed\n";
        return true;
    }
}

int main()
{
    try
    {
        return RunTest() ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "unexpected exception: " << exception.what() << '\n';
        return 1;
    }
}
