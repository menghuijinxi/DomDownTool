#include "CesiumExport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <vector>

#include "zlib.h"

#include "CesiumExportSupport.h"
#include "FbxTerrainTriangulator.h"

namespace
{
using namespace DownTool;
using Json = nlohmann::json;

struct TerrainTile
{
    uint32_t level;
    uint32_t x;
    uint32_t y;
    auto operator<=>(const TerrainTile&) const = default;
};

GeographicBounds tileBounds(const TerrainTile& tile)
{
    const double side = 180. / double(uint64_t{1} << tile.level);
    return {-180. + tile.x * side, -180. + (tile.x + 1.) * side,
        -90. + tile.y * side, -90. + (tile.y + 1.) * side};
}

template <typename Value>
void append(std::vector<unsigned char>& bytes, Value value)
{
    const auto start = bytes.size();
    bytes.resize(start + sizeof(value));
    std::memcpy(bytes.data() + start, &value, sizeof(value));
}

std::array<double, 3> horizonPoint(
    const GeographicBounds& bounds, const std::vector<std::array<double, 3>>& positions,
    double minimumHeight)
{
    constexpr std::array<double, 3> radii{6378137., 6378137., 6356752.3142451793};
    auto direction = cesium::ecef(
        (bounds.west + bounds.east) * 0.5, (bounds.south + bounds.north) * 0.5, 0.);
    double length = 0.;
    for (size_t axis = 0; axis < 3; ++axis)
    {
        direction[axis] /= radii[axis] + std::min(0., minimumHeight);
        length += direction[axis] * direction[axis];
    }
    length = std::sqrt(length);
    for (auto& value : direction)
    {
        value /= length;
    }
    double maximumMagnitude = 1.;
    bool unbounded = false;
    for (auto position : positions)
    {
        double squared = 0.;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            position[axis] /= radii[axis] + std::min(0., minimumHeight);
            squared += position[axis] * position[axis];
        }
        const double actualMagnitude = std::sqrt(squared);
        double cosine = 0.;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            cosine += position[axis] / actualMagnitude * direction[axis];
        }
        const double magnitude = std::max(1., actualMagnitude);
        const double denominator = cosine / magnitude -
            std::sqrt(std::max(0., 1. - cosine * cosine)) *
                std::sqrt(std::max(0., squared - 1.)) / magnitude;
        if (denominator <= 1.e-12)
        {
            unbounded = true;
        }
        else
        {
            maximumMagnitude = std::max(maximumMagnitude, 1. / denominator);
        }
    }
    // 半球根块不能得到有限紧界，使用极远点保守避免积极剔除。
    maximumMagnitude = unbounded ? 1.e20 : maximumMagnitude * (1. + 1.e-9);
    for (auto& value : direction)
    {
        value *= maximumMagnitude;
    }
    return direction;
}

bool writeTile(
    const CesiumExportOptions& options, const cesium::HeightConversion& conversion,
    const TerrainTile& tile, Json& statistics, std::string& error)
{
    const auto bounds = tileBounds(tile);
    const double meters = (bounds.east - bounds.west) * 111320.;
    const uint32_t divisions = uint32_t(std::clamp(
        std::ceil(meters / options.GetSampleMeters()), 8., 128.));
    const uint32_t side = divisions + 1;
    std::vector<double> heights(size_t(side) * side);
    uint64_t missing = 0;
    for (uint32_t y = 0; y < side; ++y)
    {
        if (cesium::canceled(options))
        {
            error = "地形导出已取消。";
            return false;
        }
        for (uint32_t x = 0; x < side; ++x)
        {
            const LonLat point(
                bounds.west + (bounds.east - bounds.west) * x / divisions,
                bounds.south + (bounds.north - bounds.south) * y / divisions);
            const auto sample = options.GetSampler()(point);
            if (!sample || !std::isfinite(*sample))
            {
                ++missing;
            }
            heights[size_t(y) * side + x] =
                sample && std::isfinite(*sample) ? conversion.convert(point, *sample) : 0.;
        }
    }
    std::vector<FbxTerrainGridPoint> boundary;
    for (uint32_t index = 0; index < divisions; ++index)
    {
        boundary.push_back({index, 0});
    }
    for (uint32_t index = 0; index < divisions; ++index)
    {
        boundary.push_back({divisions, index});
    }
    for (uint32_t index = divisions; index > 0; --index)
    {
        boundary.push_back({index, divisions});
    }
    for (uint32_t index = divisions; index > 0; --index)
    {
        boundary.push_back({0, index});
    }
    const double tolerance = std::max(
        options.GetErrorMeters(), meters / 128. * 0.02);
    auto curvatureHeights = heights;
    if (tile.level >= 7)
    {
        const double cosine = std::cos(
            (bounds.south + bounds.north) * std::numbers::pi / 360.);
        for (uint32_t y = 0; y < side; ++y)
        {
            for (uint32_t x = 0; x < side; ++x)
            {
                const double east = (double(x) / divisions - 0.5) * meters * cosine;
                const double north = (double(y) / divisions - 0.5) * meters;
                curvatureHeights[size_t(y) * side + x] -=
                    (east * east + north * north) / (2. * 6378137.);
            }
        }
    }
    auto tin = BuildFbxTerrainTin(
        divisions, divisions, curvatureHeights, boundary, tolerance);
    if (tile.level < 7)
    {
        // 粗层即便高度全零也需地球曲率，不能把半球简化为几个平面三角形。
        tin.points.clear();
        tin.triangles.clear();
        const uint32_t coarseDivisions = std::min(32u, divisions);
        for (uint32_t y = 0; y <= coarseDivisions; ++y)
        {
            for (uint32_t x = 0; x <= coarseDivisions; ++x)
            {
                tin.points.push_back({x * divisions / coarseDivisions,
                    y * divisions / coarseDivisions});
            }
        }
        for (uint32_t y = 0; y < coarseDivisions; ++y)
        {
            for (uint32_t x = 0; x < coarseDivisions; ++x)
            {
                const uint32_t a = y * (coarseDivisions + 1) + x;
                const uint32_t b = a + 1;
                const uint32_t c = a + coarseDivisions + 1;
                tin.triangles.push_back({a, b, c + 1});
                tin.triangles.push_back({a, c + 1, c});
            }
        }
    }
    if (tin.triangles.empty())
    {
        error = "地形三角化没有生成有效网格。";
        return false;
    }
    // 按首次索引出现的顺序重排顶点，满足 high-water-mark 编码前置条件。
    std::vector<uint32_t> remap(tin.points.size(), std::numeric_limits<uint32_t>::max());
    std::vector<FbxTerrainGridPoint> points;
    std::vector<uint32_t> indices;
    for (const auto& triangle : tin.triangles)
    {
        std::array<uint32_t, 3> vertices{
            triangle.first, triangle.second, triangle.third};
        const auto& a = tin.points[vertices[0]];
        const auto& b = tin.points[vertices[1]];
        const auto& c = tin.points[vertices[2]];
        if ((double(b.x) - a.x) * (double(c.y) - a.y) -
            (double(b.y) - a.y) * (double(c.x) - a.x) < 0.)
        {
            std::swap(vertices[1], vertices[2]);
        }
        for (const uint32_t index : vertices)
        {
            if (remap[index] == std::numeric_limits<uint32_t>::max())
            {
                remap[index] = uint32_t(points.size());
                points.push_back(tin.points[index]);
            }
            indices.push_back(remap[index]);
        }
    }
    const auto [minimum, maximum] = std::minmax_element(heights.begin(), heights.end());
    const float minHeight = float(*minimum);
    const float maxHeight = float(*maximum);
    std::array<std::vector<uint16_t>, 3> quantized;
    for (const auto& point : points)
    {
        for (uint32_t channel = 0; channel < 3; ++channel)
        {
            const double unit = channel == 0 ? double(point.x) / divisions :
                channel == 1 ? double(point.y) / divisions :
                maxHeight > minHeight ?
                    (heights[size_t(point.y) * side + point.x] - minHeight) /
                        (double(maxHeight) - minHeight) : 0.;
            quantized[channel].push_back(
                uint16_t(std::lround(std::clamp(unit, 0., 1.) * 32767.)));
        }
    }
    std::array<double, 3> low = {1.e100, 1.e100, 1.e100};
    std::array<double, 3> high = {-1.e100, -1.e100, -1.e100};
    std::vector<std::array<double, 3>> positions;
    for (size_t index = 0; index < points.size(); ++index)
    {
        // 包围/剔除信息必须包含客户端解码后的顶点，而不是量化前的理想位置。
        const auto position = cesium::ecef(
            bounds.west + (bounds.east - bounds.west) * quantized[0][index] / 32767.,
            bounds.south + (bounds.north - bounds.south) * quantized[1][index] / 32767.,
            minHeight + (double(maxHeight) - minHeight) * quantized[2][index] / 32767.);
        positions.push_back(position);
        for (size_t axis = 0; axis < 3; ++axis)
        {
            low[axis] = std::min(low[axis], position[axis]);
            high[axis] = std::max(high[axis], position[axis]);
        }
    }
    std::array<double, 3> center{};
    for (size_t axis = 0; axis < 3; ++axis)
    {
        center[axis] = (low[axis] + high[axis]) * 0.5;
    }
    double radius = 0.;
    for (const auto& position : positions)
    {
        double squared = 0.;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            squared += std::pow(position[axis] - center[axis], 2.);
        }
        radius = std::max(radius, std::sqrt(squared));
    }
    std::vector<unsigned char> bytes;
    for (const double value : center)
    {
        append(bytes, value);
    }
    append(bytes, minHeight);
    append(bytes, maxHeight);
    for (const double value : center)
    {
        append(bytes, value);
    }
    append(bytes, radius + 1.);
    const auto horizon = horizonPoint(bounds, positions, minHeight);
    for (const double value : horizon)
    {
        append(bytes, value);
    }
    append(bytes, uint32_t(points.size()));
    for (uint32_t channel = 0; channel < 3; ++channel)
    {
        int32_t previous = 0;
        for (const auto value : quantized[channel])
        {
            const int32_t delta = value - previous;
            append(bytes, uint16_t(delta >= 0 ? delta * 2 : -delta * 2 - 1));
            previous = value;
        }
    }
    const bool wide = points.size() > 65536;
    while (bytes.size() % (wide ? 4 : 2) != 0)
    {
        bytes.push_back(0);
    }
    append(bytes, uint32_t(indices.size() / 3));
    uint32_t highest = 0;
    const auto writeIndex = [&](uint32_t value)
    {
        if (wide)
        {
            append(bytes, value);
        }
        else
        {
            append(bytes, uint16_t(value));
        }
    };
    for (const uint32_t index : indices)
    {
        const auto code = highest - index;
        writeIndex(code);
        if (code == 0)
        {
            ++highest;
        }
    }
    for (uint32_t edge = 0; edge < 4; ++edge)
    {
        std::vector<uint32_t> edgeIndices;
        for (uint32_t index = 0; index < points.size(); ++index)
        {
            const auto& point = points[index];
            if ((edge == 0 && point.x == 0) || (edge == 1 && point.y == 0) ||
                (edge == 2 && point.x == divisions) ||
                (edge == 3 && point.y == divisions))
            {
                edgeIndices.push_back(index);
            }
        }
        append(bytes, uint32_t(edgeIndices.size()));
        for (const auto index : edgeIndices)
        {
            writeIndex(index);
        }
    }
    const auto path = options.GetOutputPath() / std::to_string(tile.level) /
        std::to_string(tile.x) / (std::to_string(tile.y) + ".terrain");
    std::filesystem::create_directories(path.parent_path());
    const auto encodedPath = path.wstring();
#if defined(_WIN32)
    gzFile stream = gzopen_w(encodedPath.c_str(), "wb6");
#else
    gzFile stream = gzopen(cesium::pathUtf8(path).c_str(), "wb6");
#endif
    if (!stream)
    {
        error = "无法创建 terrain 瓦块：" + cesium::pathUtf8(path);
        return false;
    }
    const bool written = gzwrite(stream, bytes.data(), unsigned(bytes.size())) ==
        int(bytes.size());
    const int closed = gzclose(stream);
    if (!written || closed != Z_OK)
    {
        error = "terrain 瓦块压缩或写入失败。";
        return false;
    }
    statistics.push_back({
        {"level", tile.level}, {"x", tile.x}, {"y", tile.y},
        {"triangles", indices.size() / 3}, {"vertices", points.size()},
        {"sampleGridSide", side}, {"sampleSpacingMeters", meters / divisions},
        {"maximumCurvatureAdjustedSampleErrorMeters",
            tile.level < 7 ? Json(nullptr) : Json(tin.maximumErrorMeters)},
        {"missingSamples", missing}});
    return true;
}
} // namespace

bool DownTool::ExportCesiumTerrain(const CesiumExportOptions& options, std::string& error)
{
    try
    {
        if (!cesium::validateOptions(options, error) || !options.GetSampler())
        {
            if (error.empty())
            {
                error = "Cesium 地形缺少高程采样器。";
            }
            return false;
        }
        const cesium::HeightConversion conversion(options);
        const auto& bounds = options.GetBounds();
        const uint32_t maximumLevel = uint32_t(std::clamp(std::ceil(std::log2(
            180. * 111320. / (options.GetSampleMeters() * 128.))), 0., 20.));
        std::set<TerrainTile> tiles{{0, 0, 0}, {0, 1, 0}};
        Json available = Json::array();
        for (uint32_t level = 0; level <= maximumLevel; ++level)
        {
            const double side = 180. / double(uint64_t{1} << level);
            const uint32_t nx = uint32_t{2} << level;
            const uint32_t ny = uint32_t{1} << level;
            const auto coordinate = [](double value, uint32_t count)
            {
                return uint32_t(std::clamp(std::floor(value), 0., double(count - 1)));
            };
            const uint32_t firstX = coordinate((bounds.west + 180.) / side, nx);
            const uint32_t lastX = coordinate((bounds.east + 180.) / side, nx);
            const uint32_t firstY = coordinate((bounds.south + 90.) / side, ny);
            const uint32_t lastY = coordinate((bounds.north + 90.) / side, ny);
            if (uint64_t(lastX - firstX + 1) * (lastY - firstY + 1) > 100000)
            {
                error = "地形瓦块数量过大，请增大采样间距或缩小范围。";
                return false;
            }
            for (uint32_t y = firstY; y <= lastY; ++y)
            {
                for (uint32_t x = firstX; x <= lastX; ++x)
                {
                    const TerrainTile tile{level, x, y};
                    if (options.GetPolygons().empty() ||
                        GeographicPolygonsIntersectBounds(options.GetPolygons(), tileBounds(tile)))
                    {
                        tiles.insert(tile);
                        auto ancestor = tile;
                        while (ancestor.level > 0)
                        {
                            --ancestor.level;
                            ancestor.x /= 2;
                            ancestor.y /= 2;
                            tiles.insert(ancestor);
                        }
                    }
                }
            }
        }
        for (uint32_t level = 0; level <= maximumLevel; ++level)
        {
            Json ranges = Json::array();
            for (const auto& tile : tiles)
            {
                if (tile.level == level)
                {
                    ranges.push_back({{"startX", tile.x}, {"endX", tile.x},
                        {"startY", tile.y}, {"endY", tile.y}});
                }
            }
            available.push_back(std::move(ranges));
        }
        Json statistics = Json::array();
        uint64_t completed = 0;
        for (const auto& tile : tiles)
        {
            if (!writeTile(options, conversion, tile, statistics, error))
            {
                return false;
            }
            if (options.GetProgress())
            {
                options.GetProgress()(++completed, tiles.size());
            }
        }
        const Json layer = {
            {"tilejson", "2.1.0"}, {"format", "quantized-mesh-1.0"}, {"version", "1.0.0"},
            {"scheme", "tms"}, {"projection", "EPSG:4326"}, {"tiles", {"{z}/{x}/{y}.terrain"}},
            {"minzoom", 0}, {"maxzoom", maximumLevel},
            {"bounds", {bounds.west, bounds.south, bounds.east, bounds.north}},
            {"available", available}, {"attribution", "Copernicus DEM / DomDownTool"}};
        return cesium::writeJson(options.GetOutputPath() / "layer.json", layer, error) &&
            cesium::writeJson(options.GetOutputPath() / "export-report.json",
                {{"heightConversion", conversion.description()}, {"missingHeightFallback", 0},
                 {"tiles", statistics}}, error);
    }
    catch (const std::exception& exception)
    {
        error = "Cesium 地形导出失败：" + std::string(exception.what());
        return false;
    }
}
