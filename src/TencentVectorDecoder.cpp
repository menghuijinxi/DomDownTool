#include "TencentVectorDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

#include "nlohmann/json.hpp"
#include "openssl/evp.h"

#include "GCJ02GoogleDownBase.h"

namespace
{
    using Json = nlohmann::json;
    using namespace DownTool;

    const Json& LayerRules()
    {
        static const Json rules = Json::parse(
#include "TencentVectorLayerRules.inc"
        );
        return rules;
    }

    uint8_t Rotate(uint8_t value, unsigned bits)
    {
        return static_cast<uint8_t>((value << bits) | (value >> (8 - bits)));
    }

    Json Unwrap(const std::string& envelope)
    {
        Json payload = Json::parse(envelope);
        if (!payload.is_object())
        {
            throw std::runtime_error("腾讯响应必须为 JSON 对象。");
        }
        if (!payload.contains("ret"))
        {
            const std::string encoded = payload.at("data").get<std::string>();
            if (encoded.empty() || encoded.size() % 4 != 0 ||
                encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            {
                throw std::runtime_error("腾讯 data 的 Base64 长度无效。");
            }
            std::string bytes(encoded.size() / 4 * 3, '\0');
            int length = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
                                         reinterpret_cast<const unsigned char*>(encoded.data()),
                                         static_cast<int>(encoded.size()));
            if (length < 0)
            {
                throw std::runtime_error("腾讯 data 不是有效 Base64。");
            }
            length -= encoded.back() == '=' ? 1 : 0;
            length -= encoded.size() > 1 && encoded[encoded.size() - 2] == '=' ? 1 : 0;
            bytes.resize(static_cast<size_t>(length));
            if (bytes.size() % 2 != 0)
            {
                bytes.push_back('\0');
            }
            // SDK 的三轮字节对变换；只还原腾讯信封，JSON 与 Base64 均由现有库处理。
            for (size_t offset = 0; offset < bytes.size(); offset += 2)
            {
                uint8_t left = static_cast<uint8_t>(bytes[offset]);
                uint8_t right = static_cast<uint8_t>(bytes[offset + 1]);
                for (unsigned round = 0; round < 3; ++round)
                {
                    const uint8_t next =
                        left ^ (Rotate(right, 2) ^ (Rotate(right, 1) & Rotate(right, 3)));
                    left = right;
                    right = next;
                }
                bytes[offset] = static_cast<char>(left);
                bytes[offset + 1] = static_cast<char>(right);
            }
            const size_t end = bytes.find_last_of('}');
            if (end == std::string::npos)
            {
                throw std::runtime_error("腾讯 data 还原后不是 JSON。");
            }
            payload = Json::parse(bytes.begin(), bytes.begin() + end + 1);
        }
        if (!payload.at("ret").is_number_integer() || payload.at("ret") != 0 ||
            !payload.at("features").is_array())
        {
            throw std::runtime_error("腾讯服务返回错误 ret 或缺少 features。");
        }
        return payload;
    }

    std::string Category(uint32_t group, std::string_view tag)
    {
        if (group == 65536 || group == 262144)
        {
            return "poi";
        }
        if (group == 196608)
        {
            if (tag == "building" || tag == "water")
            {
                return std::string(tag);
            }
            if (tag == "grass" || tag == "park")
            {
                return "green";
            }
            if (tag == "subwayarea")
            {
                return "subway";
            }
        }
        if (group == 131072)
        {
            if (tag == "highway" || tag == "motorway")
            {
                return "highway";
            }
            if (tag == "mainroad" || tag == "secondaryroad" || tag == "street" || tag == "path")
            {
                return "road";
            }
            if (tag == "water")
            {
                return "water";
            }
            if (tag == "subwayline")
            {
                return "subway";
            }
            if (tag == "guojie" || tag == "shengjie")
            {
                return "boundary";
            }
        }
        return "other";
    }

    void AppendFeature(const Json& raw, const AmapVectorTile& tile, bool compressed,
                       const char* origin, AmapVectorDecodedData& result)
    {
        if (!raw.is_array() || raw.size() < 2 || !raw[0].is_number_unsigned())
        {
            throw std::runtime_error("腾讯要素缺少有效图层编号。");
        }
        const uint32_t id = raw[0].get<uint32_t>();
        const uint32_t group = id & 0xf0000;
        if (group != 65536 && group != 131072 && group != 196608 && group != 262144 &&
            group != 327680)
        {
            throw std::runtime_error("暂不支持的腾讯几何图层：" + std::to_string(id));
        }
        const Json paths = raw[1].is_array() && !raw[1].empty() && raw[1][0].is_array()
                               ? raw[1]
                               : Json::array({raw[1]});
        if (group == 196608 && paths.size() != 1)
        {
            // TODO: 取得真实嵌套面样本后确认外环与洞编码，当前拒绝猜测拓扑。
            throw std::runtime_error("暂不支持腾讯多路径面，需确认外环与洞编码。");
        }
        AmapVectorFeature feature;
        feature.SetDataSource(VectorDataSource::Tencent);
        feature.mainKey = id;
        feature.geometryType = group == 65536 || group == 262144 ? AmapVectorGeometryType::Point
                               : group == 196608                 ? AmapVectorGeometryType::Polygon
                                                 : AmapVectorGeometryType::LineString;
        const auto rule = LayerRules().find(std::to_string(id));
        const std::string tag =
            rule == LayerRules().end() ? "unmapped" : (*rule)[0].get<std::string>();
        feature.layerName = Category(group, tag);
        feature.minZoom = rule == LayerRules().end() ? 0 : (*rule)[1].get<uint32_t>();
        // z18 以上继续显示最高数据级，不能因样式表止于 z20 而隐藏已加载数据。
        feature.maxZoom = 30;
        Json localPaths = Json::array();
        std::string issue;
        const double scale = std::ldexp(1., static_cast<int>(tile.zoom));
        for (const auto& path : paths)
        {
            if (!path.is_array() || path.size() < 2 || path.size() % 2 != 0)
            {
                throw std::runtime_error("腾讯几何坐标必须为成对的有限数值。");
            }
            std::vector<AmapVectorCoordinate> coordinates;
            Json local = Json::array();
            double x = 0.;
            double y = 0.;
            for (size_t index = 0; index < path.size(); index += 2)
            {
                if (!path[index].is_number() || !path[index + 1].is_number())
                {
                    throw std::runtime_error("腾讯坐标包含非数值。");
                }
                x = (compressed ? x : 0.) + path[index].get<double>();
                y = (compressed ? y : 0.) + path[index + 1].get<double>();
                if (!std::isfinite(x) || !std::isfinite(y))
                {
                    throw std::runtime_error("腾讯坐标包含非有限值。");
                }
                local.push_back({x, y});
                const LonLat gcj{(tile.x + x / 256.) / scale * 360. - 180.,
                                 std::atan(std::sinh(std::numbers::pi *
                                                     (1. - 2. * (tile.y + y / 256.) / scale))) *
                                     180. / std::numbers::pi};
                const auto wgs = GCJ02GoogleDownBase::GCJ02ToWGS84(gcj);
                coordinates.push_back({wgs.lon, wgs.lat});
            }
            if (feature.geometryType == AmapVectorGeometryType::Point)
            {
                coordinates.resize(1);
            }
            else if (feature.geometryType == AmapVectorGeometryType::Polygon)
            {
                double area = 0.;
                for (size_t index = 0; index < local.size(); ++index)
                {
                    const auto& next = local[(index + 1) % local.size()];
                    area += local[index][0].get<double>() * next[1].get<double>() -
                            next[0].get<double>() * local[index][1].get<double>();
                }
                if (coordinates.size() < 3 || area == 0.)
                {
                    issue = "腾讯原始面路径退化，无法绘制有效面";
                }
                else if (coordinates.front().longitude != coordinates.back().longitude ||
                         coordinates.front().latitude != coordinates.back().latitude)
                {
                    coordinates.push_back(coordinates.front());
                }
            }
            else if (coordinates.size() < 2)
            {
                issue = "腾讯原始线路径少于两个顶点";
            }
            feature.geometryParts.push_back(std::move(coordinates));
            localPaths.push_back(std::move(local));
        }
        feature.SetCoordinateSystem(VectorCoordinateSystem::Wgs84);
        Json fields = Json::array();
        for (size_t index = 2; index < raw.size(); ++index)
        {
            fields.push_back(raw[index]);
        }
        if (id == 196619 && raw.size() > 2 && raw[2].is_number())
        {
            const double height = raw[2].get<double>();
            if (std::isfinite(height) && height > 0.)
            {
                feature.SetHeightMeters(height);
            }
        }
        if (raw.size() > 2 && raw[2].is_string())
        {
            feature.names.push_back({raw[2].get<std::string>(), "zh-CN"});
        }
        if (!issue.empty())
        {
            feature.geometryParts.clear();
        }
        feature.SetSourceProperties(
            {{"provider", "tencent"},
             {"coordinateSystem", "WGS84"},
             {"sourceCoordinateSystem", "GCJ-02"},
             {"layerId", id},
             {"tag", tag},
             {"styleName", rule == LayerRules().end() ? "" : (*rule)[3].get<std::string>()},
             {"tile", std::to_string(tile.zoom) + "/" + std::to_string(tile.x) + "/" +
                          std::to_string(tile.y)},
             {"origin", origin},
             {"localCoordinates", localPaths},
             {"rawFields", fields},
             {"geometryIssue", issue.empty() ? Json() : Json(issue)}});
        result.features.push_back(std::move(feature));
    }
} // namespace

bool DownTool::TencentVectorDecoder::DecodeEnvelope(const std::string& envelope,
                                                    const AmapVectorTile& tile,
                                                    AmapVectorDecodedData& decodedData,
                                                    std::string& error, bool compressed)
{
    try
    {
        if (tile.zoom < 3 || tile.zoom > 18 || tile.x >= (1u << tile.zoom) ||
            tile.y >= (1u << tile.zoom))
        {
            throw std::runtime_error("腾讯矢量瓦片坐标无效，暂支持 z3 至 z18。");
        }
        const Json payload = Unwrap(envelope);
        AmapVectorDecodedData result;
        result.version = "Tencent 405";
        result.packetCount = 1;
        result.status = true;
        for (const auto& raw : payload.at("features"))
        {
            if (raw.is_array() && !raw.empty() && raw[0].is_array())
            {
                for (const auto& entry : raw)
                {
                    AppendFeature(entry, tile, compressed, "features", result);
                }
            }
            else
            {
                AppendFeature(raw, tile, compressed, "features", result);
            }
        }
        if (payload.contains("pois"))
        {
            if (!payload["pois"].is_array())
            {
                throw std::runtime_error("暂不支持的腾讯 pois 格式。");
            }
            for (const auto& raw : payload["pois"])
            {
                AppendFeature(raw, tile, compressed, "pois", result);
            }
        }
        decodedData = std::move(result);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "腾讯矢量解码失败：" + std::string(exception.what());
        return false;
    }
}
