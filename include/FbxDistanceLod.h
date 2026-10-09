#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"

namespace DownTool
{
class FbxDistanceLodRule
{
public:
    FbxDistanceLodRule() = default;
    FbxDistanceLodRule(double radiusMeters, uint32_t value)
        : radiusMeters_(radiusMeters), value_(value) {}

    double GetRadiusMeters() const { return radiusMeters_; }
    void SetRadiusMeters(double value) { radiusMeters_ = value; }
    uint32_t GetValue() const { return value_; }
    void SetValue(uint32_t value) { value_ = value; }
    bool operator==(const FbxDistanceLodRule&) const = default;

protected:
    double radiusMeters_ = 0.;
    uint32_t value_ = 0;
};

class FbxDistanceLod
{
public:
    const std::vector<FbxDistanceLodRule>& GetTextureRules() const { return textureRules_; }
    void SetTextureRules(const std::vector<FbxDistanceLodRule>& value) { textureRules_ = value; }
    const std::vector<FbxDistanceLodRule>& GetTriangleRules() const { return triangleRules_; }
    void SetTriangleRules(const std::vector<FbxDistanceLodRule>& value) { triangleRules_ = value; }
    bool operator==(const FbxDistanceLod&) const = default;

    uint32_t ResolveTextureSize(double distanceMeters, uint32_t baseSize) const;
    uint32_t ResolveMaximumTriangles(double distanceMeters, uint32_t baseBudget) const;
    std::string Validate() const;

protected:
    std::vector<FbxDistanceLodRule> textureRules_;
    std::vector<FbxDistanceLodRule> triangleRules_;
};

FbxDistanceLod DefaultFbxDistanceLod();
nlohmann::json FbxDistanceLodToJson(const FbxDistanceLod& settings);
std::optional<FbxDistanceLod> FbxDistanceLodFromJson(const nlohmann::json& json);
}
