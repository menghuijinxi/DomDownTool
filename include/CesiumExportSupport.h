#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>

#include "GeographicLib/Geoid.hpp"
#include "nlohmann/json.hpp"

#include "CesiumExport.h"

namespace DownTool::cesium
{
[[nodiscard]] std::string pathUtf8(const std::filesystem::path& path);
[[nodiscard]] bool writeJson(
    const std::filesystem::path& path, const nlohmann::json& value, std::string& error);
[[nodiscard]] bool validateOptions(const CesiumExportOptions& options, std::string& error);
[[nodiscard]] bool canceled(const CesiumExportOptions& options);
[[nodiscard]] std::array<double, 3> ecef(double longitude, double latitude, double height);

class HeightConversion
{
public:
    explicit HeightConversion(const CesiumExportOptions& options);
    [[nodiscard]] double convert(const LonLat& point, double sourceHeight) const;
    [[nodiscard]] std::string description() const;

protected:
    double offset_ = 0.;
    std::unique_ptr<GeographicLib::Geoid> geoid_;
};
} // namespace DownTool::cesium
