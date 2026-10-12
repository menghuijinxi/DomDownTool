#include "CesiumExportSupport.h"

#include <cmath>
#include <fstream>

#include "GeographicLib/Geocentric.hpp"

namespace DownTool::cesium
{
std::string pathUtf8(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool writeJson(
    const std::filesystem::path& path, const nlohmann::json& value, std::string& error)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value.dump(2);
    if (!stream.good())
    {
        error = "无法写入 Cesium 元数据：" + pathUtf8(path);
        return false;
    }
    return true;
}

bool validateOptions(const CesiumExportOptions& options, std::string& error)
{
    const auto& bounds = options.GetBounds();
    if (options.GetOutputPath().empty() ||
        !std::isfinite(bounds.west) || !std::isfinite(bounds.east) ||
        !std::isfinite(bounds.south) || !std::isfinite(bounds.north) ||
        bounds.west < -180. || bounds.east > 180. ||
        bounds.south < -90. || bounds.north > 90. ||
        bounds.west >= bounds.east || bounds.south >= bounds.north ||
        !std::isfinite(options.GetSampleMeters()) || options.GetSampleMeters() <= 0. ||
        !std::isfinite(options.GetErrorMeters()) || options.GetErrorMeters() <= 0. ||
        !std::isfinite(options.GetHeightOffsetMeters()))
    {
        error = "Cesium 输出目录、范围、采样间距或误差参数无效。";
        return false;
    }
    if (!options.GetPolygons().empty() &&
        !ValidateWgs84Polygons(options.GetPolygons(), error))
    {
        return false;
    }
    return true;
}

bool canceled(const CesiumExportOptions& options)
{
    return options.GetCancellation() && options.GetCancellation()();
}

std::array<double, 3> ecef(double longitude, double latitude, double height)
{
    std::array<double, 3> position{};
    GeographicLib::Geocentric::WGS84().Forward(
        latitude, longitude, height, position[0], position[1], position[2]);
    return position;
}

HeightConversion::HeightConversion(const CesiumExportOptions& options)
    : offset_(options.GetHeightOffsetMeters())
{
    if (!options.GetGeoidPath().empty())
    {
        const auto& path = options.GetGeoidPath();
        geoid_ = std::make_unique<GeographicLib::Geoid>(
            pathUtf8(path.stem()), pathUtf8(path.parent_path()), true);
    }
}

double HeightConversion::convert(const LonLat& point, double sourceHeight) const
{
    return sourceHeight + offset_ + (geoid_ ? (*geoid_)(point.lat, point.lon) : 0.);
}

std::string HeightConversion::description() const
{
    return geoid_ ? "orthometric + geoid undulation + offset -> WGS84 ellipsoid" :
        "source height + explicit offset; geoid correction not applied";
}
} // namespace DownTool::cesium
