#include "BaiduVectorClassification.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace
{
using Json = nlohmann::json;
using namespace DownTool;

const std::map<std::string, std::string>& Categories()
{
    static const auto categories = [] {
        std::map<std::string, std::string> result;
        const std::map<std::string, std::vector<std::string>> groups = {
            {"building", {"BUILDING"}},
            {"water", {"WATER", "OCEAN", "WATEROTHER"}},
            {"green", {"GREEN"}},
            {"road",
             {"ROAD", "NATIONALWAY", "PROVINCIALWAY", "CITYHIGHWAY", "ARTERIAL", "TERTIARYWAY",
              "FOURLEVELWAY", "LOCAL", "SCENICSPOTSWAY", "UNIVERSITYWAY", "VACATIONWAY"}},
            {"highway", {"HIGHWAY"}},
            {"railway", {"RAILWAY", "RAILROAD", "HIGHRAILWAY"}},
            {"subway", {"SUBWAY", "SUBWAYSTATION"}},
            {"boundary", {"BOUNDARY", "ADMINISTRATIVE"}},
            {"scenic", {"SCENICSPOTS"}},
            {"education", {"EDUCATION"}},
            {"medical", {"MEDICAL"}},
            {"estate", {"ESTATE"}},
            {"entertainment", {"ENTERTAINMENT"}},
            {"transportation", {"TRANSPORTATION", "PARKINGLOT"}},
            {"manmade", {"MANMADE", "AOIOTHER", "SHOPPING", "PLAYGROUND"}},
            {"roadFacility",
             {"FOOTBRIDGE", "CROSSWALK", "UNDERPASS", "PARKINGSPACE", "LANELINE", "DIVIDER", "CURB",
              "GUARDRAIL"}}};
        for (const auto& [category, types] : groups)
        {
            for (const auto& type : types)
            {
                result.emplace(type, category);
            }
        }
        return result;
    }();
    return categories;
}

bool IsSupportedCategory(const std::string& category)
{
    constexpr std::array<std::string_view, 8> supported = {"building", "water",  "green",    "road",
                                                           "highway",  "subway", "boundary", "poi"};
    return std::find(supported.begin(), supported.end(), category) != supported.end();
}
} // namespace

bool DownTool::BaiduVectorClassification::DecodeResponse(const std::string& response,
                                                         const std::vector<uint32_t>& ids,
                                                         BaiduFeatureTypes& types,
                                                         std::string& error)
{
    try
    {
        const auto begin = response.find_first_not_of(" \t\r\n");
        const auto end = response.find_last_not_of(" \t\r\n;");
        constexpr std::string_view prefix = "baiduFeatureTypes(";
        if (ids.empty() || ids.size() > 64 || begin == std::string::npos ||
            end == std::string::npos || response.compare(begin, prefix.size(), prefix) != 0 ||
            response[end] != ')' || end < begin + prefix.size())
        {
            throw std::runtime_error("百度分类接口响应无效。");
        }
        const auto values =
            Json::parse(response.begin() + begin + prefix.size(), response.begin() + end);
        if (!values.is_array() || values.size() != ids.size())
        {
            throw std::runtime_error("百度分类编号与返回类型未一一对应。");
        }
        BaiduFeatureTypes parsed;
        for (size_t index = 0; index < ids.size(); ++index)
        {
            const auto type = values[index].get<std::string>();
            if (!std::all_of(type.begin(), type.end(), [](char c) {
                    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                }))
            {
                throw std::runtime_error("百度分类接口包含非法类型。");
            }
            if (!parsed.emplace(ids[index], type).second)
            {
                throw std::runtime_error("百度分类请求包含重复编号。");
            }
        }
        types = std::move(parsed);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool DownTool::BaiduVectorClassification::Apply(AmapVectorDecodedData& decoded,
                                                const BaiduFeatureTypes& types, std::string& error)
{
    try
    {
        // 先检查完整性，防止分类查询缺失时留下部分成功的结果。
        for (const auto& feature : decoded.features)
        {
            if (!types.contains(feature.mainKey))
            {
                throw std::runtime_error("缺少百度样式 " + std::to_string(feature.mainKey) +
                                         " 的分类结果。");
            }
        }
        for (auto& feature : decoded.features)
        {
            auto properties = feature.GetSourceProperties();
            const auto& rawType = types.at(feature.mainKey);
            const bool officialKnown = !rawType.empty() && rawType != "UNCLASSIFIED";
            const bool roadSurface = !officialKnown && properties.at("version") == kVersion &&
                                     properties.at("updateDate") == kUpdateDate &&
                                     properties.at("nativeZoom") == 19 &&
                                     properties.at("groupId") == 15 &&
                                     (feature.mainKey == 504812 || feature.mainKey == 504813);
            const std::string resolved = roadSurface ? "ROAD" : rawType;
            const auto category = Categories().find(resolved);
            std::string semantic =
                officialKnown || roadSurface
                    ? (category == Categories().end() ? "other" : category->second)
                    : "unknown";
            if (feature.geometryType == AmapVectorGeometryType::Point)
            {
                semantic = "point";
            }
            else if (properties.at("groupId") == 8)
            {
                semantic = "building";
            }
            feature.layerName = semantic == "point"             ? "poi"
                                : IsSupportedCategory(semantic) ? semantic
                                                                : "other";
            properties["featureType"] = rawType;
            properties["resolvedFeatureType"] = resolved;
            properties["semanticCategory"] = semantic;
            properties["classificationKnown"] = officialKnown || roadSurface;
            properties["classificationSource"] = officialKnown ? "official-feature-type"
                                                 : roadSurface ? "verified-road-surface"
                                                 : properties.at("groupId") == 8 ? "geometry-group"
                                                                                 : "unmapped";
            feature.SetSourceProperties(properties);
        }
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "百度分类失败：" + std::string(exception.what());
        return false;
    }
}
