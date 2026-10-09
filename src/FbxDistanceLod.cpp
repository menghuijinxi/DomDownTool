#include "FbxDistanceLod.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>

#include "nlohmann/json.hpp"

namespace
{
uint32_t ResolveRule(
    const std::vector<DownTool::FbxDistanceLodRule>& rules,
    double distanceMeters, uint32_t fallback)
{
    double selectedRadius = -1.;
    for (const auto& rule : rules)
    {
        if (rule.GetRadiusMeters() <= distanceMeters &&
            rule.GetRadiusMeters() >= selectedRadius)
        {
            selectedRadius = rule.GetRadiusMeters();
            fallback = rule.GetValue();
        }
    }
    return fallback;
}

std::string ValidateRadii(const std::vector<DownTool::FbxDistanceLodRule>& rules)
{
    std::set<double> radii;
    for (const auto& rule : rules)
    {
        if (!std::isfinite(rule.GetRadiusMeters()) || rule.GetRadiusMeters() < 0.)
        {
            return "距离档位的半径必须是大于等于 0 的米数。";
        }
        if (!radii.insert(rule.GetRadiusMeters()).second)
        {
            return "同一组距离档位不能使用重复半径。";
        }
    }
    return {};
}

nlohmann::json RulesToJson(
    const std::vector<DownTool::FbxDistanceLodRule>& rules, std::string_view valueName)
{
    auto array = nlohmann::json::array();
    for (const auto& rule : rules)
    {
        array.push_back({
            {"radiusMeters", rule.GetRadiusMeters()},
            {std::string(valueName), rule.GetValue()}});
    }
    return array;
}

std::optional<std::vector<DownTool::FbxDistanceLodRule>> RulesFromJson(
    const nlohmann::json& json, std::string_view key, std::string_view valueName)
{
    std::vector<DownTool::FbxDistanceLodRule> rules;
    const auto found = json.find(key);
    if (found == json.end())
    {
        return rules;
    }
    if (!found->is_array())
    {
        return std::nullopt;
    }
    for (const auto& entry : *found)
    {
        if (!entry.is_object())
        {
            return std::nullopt;
        }
        const auto radius = entry.find("radiusMeters");
        const auto value = entry.find(valueName);
        if (radius == entry.end() || !radius->is_number() ||
            value == entry.end() || !value->is_number())
        {
            return std::nullopt;
        }
        const double number = value->get<double>();
        if (!std::isfinite(number) || number < 0. ||
            number > std::numeric_limits<uint32_t>::max() || std::floor(number) != number)
        {
            return std::nullopt;
        }
        rules.emplace_back(radius->get<double>(), static_cast<uint32_t>(number));
    }
    return rules;
}
}

uint32_t DownTool::FbxDistanceLod::ResolveTextureSize(
    double distanceMeters, uint32_t baseSize) const
{
    // 远处只降低清晰度，不把低分辨率源图放大。
    return std::min(baseSize, ResolveRule(textureRules_, distanceMeters, baseSize));
}

uint32_t DownTool::FbxDistanceLod::ResolveMaximumTriangles(
    double distanceMeters, uint32_t baseBudget) const
{
    return ResolveRule(triangleRules_, distanceMeters, baseBudget);
}

std::string DownTool::FbxDistanceLod::Validate() const
{
    for (const auto* rules : {&textureRules_, &triangleRules_})
    {
        const std::string error = ValidateRadii(*rules);
        if (!error.empty())
        {
            return error;
        }
    }
    for (const auto& rule : textureRules_)
    {
        const uint32_t value = rule.GetValue();
        if (value < 128 || value > 16384 || (value & (value - 1)) != 0)
        {
            return "距离档位的贴图尺寸必须是 128 到 16384 之间的 2 的次幂。";
        }
    }
    for (const auto& rule : triangleRules_)
    {
        if (rule.GetValue() == 1)
        {
            return "距离档位的三角面上限必须为 0（不限）或至少 2。";
        }
    }
    return {};
}

DownTool::FbxDistanceLod DownTool::DefaultFbxDistanceLod()
{
    FbxDistanceLod settings;
    settings.SetTextureRules({{10000., 2048}, {30000., 1024}});
    settings.SetTriangleRules({{10000., 10000}, {30000., 5000}});
    return settings;
}

nlohmann::json DownTool::FbxDistanceLodToJson(const FbxDistanceLod& settings)
{
    return {
        {"textureRules", RulesToJson(settings.GetTextureRules(), "textureSize")},
        {"triangleRules", RulesToJson(settings.GetTriangleRules(), "maximumTriangles")}};
}

std::optional<DownTool::FbxDistanceLod> DownTool::FbxDistanceLodFromJson(
    const nlohmann::json& json)
{
    if (!json.is_object())
    {
        return std::nullopt;
    }
    const auto textures = RulesFromJson(json, "textureRules", "textureSize");
    const auto triangles = RulesFromJson(json, "triangleRules", "maximumTriangles");
    if (!textures || !triangles)
    {
        return std::nullopt;
    }
    FbxDistanceLod settings;
    settings.SetTextureRules(*textures);
    settings.SetTriangleRules(*triangles);
    return settings;
}
