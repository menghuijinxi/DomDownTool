#include "BaiduVectorDecoder.h"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "polyclipping/clipper.hpp"

#include "BaiduDown.h"
#include "BaiduVectorClassification.h"

namespace
{
using namespace DownTool;
using Json = nlohmann::json;

struct Block
{
    size_t offset;
    size_t length;
};

class TileReader
{
  public:
    TileReader(std::string_view bytes, const BaiduVectorDecodeOptions& options)
        : bytes_(bytes), options_(options)
    {
    }

    void CheckCancellation() const
    {
        if (options_.GetCancellation() && options_.GetCancellation()())
        {
            throw std::runtime_error("百度 GIS 解码已取消。");
        }
    }

    uint32_t Read32(size_t offset) const
    {
        Require(offset, 4);
        uint32_t result = 0;
        for (size_t index = 0; index < 4; ++index)
        {
            result |= uint32_t(static_cast<unsigned char>(bytes_[offset + index])) << (index * 8);
        }
        return result;
    }

    uint32_t Header(const Block& block, size_t word) const
    {
        if (word >= block.length / 4)
        {
            throw std::runtime_error("百度二进制块头越界。");
        }
        return Read32(block.offset + word * 4);
    }

    std::vector<Block> Children(const Block& block, size_t headerWords) const
    {
        CheckCancellation();
        if (headerWords > block.length / 4)
        {
            throw std::runtime_error("百度二进制块头长度无效。");
        }
        if (block.length == headerWords * 4)
        {
            return {};
        }
        const size_t count = Header(block, headerWords);
        if (count > block.length / 4 - headerWords - 1)
        {
            throw std::runtime_error("百度二进制子块目录越界。");
        }
        size_t start = (headerWords + 1 + count) * 4;
        std::vector<Block> children;
        children.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            const size_t length = Header(block, headerWords + 1 + index);
            if (start > block.length || length > block.length - start)
            {
                throw std::runtime_error("百度二进制子块内容越界。");
            }
            children.push_back({block.offset + start, length});
            start += (length + 3) / 4 * 4;
        }
        return children;
    }

    std::string Text(const Block& block) const
    {
        Require(block.offset, block.length);
        const std::string text(bytes_.substr(block.offset, block.length));
        // 复用 JSON 库严格检查 UTF-8，避免损坏文本进入后续导出。
        (void)Json(text).dump();
        return text;
    }

    ClipperLib::Path Vertices(const Block& block) const
    {
        if (block.length % 8 != 0)
        {
            throw std::runtime_error("百度差分顶点数组长度无效。");
        }
        ClipperLib::Path vertices;
        vertices.reserve(block.length / 8);
        int64_t x = 0;
        int64_t y = 0;
        for (size_t offset = 0; offset < block.length; offset += 8)
        {
            if (offset % 8192 == 0)
            {
                CheckCancellation();
            }
            x += std::bit_cast<int32_t>(Read32(block.offset + offset));
            y += std::bit_cast<int32_t>(Read32(block.offset + offset + 4));
            // 原生局部格网仅千级；拒绝损坏差分，避免 Clipper 的整数范围溢出。
            if (std::abs(x) > 100000000LL || std::abs(y) > 100000000LL)
            {
                throw std::runtime_error("百度局部坐标超出有效范围。");
            }
            vertices.emplace_back(x, y);
        }
        return vertices;
    }

    ClipperLib::Paths PolygonRings(const ClipperLib::Path& vertices, const Block& indices) const
    {
        if (indices.length % 6 != 0)
        {
            throw std::runtime_error("百度三角网格索引长度无效。");
        }
        ClipperLib::Clipper clipper;
        bool hasTriangles = false;
        for (size_t offset = 0; offset < indices.length; offset += 6)
        {
            if (offset % 6144 == 0)
            {
                CheckCancellation();
            }
            ClipperLib::Path triangle;
            for (size_t index = 0; index < 3; ++index)
            {
                const size_t position = indices.offset + offset + index * 2;
                Require(position, 2);
                const size_t vertex =
                    static_cast<unsigned char>(bytes_[position]) |
                    (size_t(static_cast<unsigned char>(bytes_[position + 1])) << 8);
                if (vertex >= vertices.size())
                {
                    throw std::runtime_error("百度三角网格索引超出顶点边界。");
                }
                triangle.push_back(vertices[vertex]);
            }
            if (ClipperLib::Area(triangle) == 0.)
            {
                continue;
            }
            if (!ClipperLib::Orientation(triangle))
            {
                ClipperLib::ReversePath(triangle);
            }
            hasTriangles |= clipper.AddPath(triangle, ClipperLib::ptSubject, true);
        }
        ClipperLib::Paths rings;
        // 同向三角形做非零并集，可保留孔洞和独立面，不能把顶点按存储顺序连线。
        if (hasTriangles && !clipper.Execute(ClipperLib::ctUnion, rings, ClipperLib::pftNonZero,
                                             ClipperLib::pftNonZero))
        {
            throw std::runtime_error("百度三角网格合并失败。");
        }
        CheckCancellation();
        return rings;
    }

    AmapVectorFeature BaseFeature(uint32_t group, uint32_t style, uint32_t order) const
    {
        const auto& tile = options_.GetTile();
        AmapVectorFeature feature;
        feature.SetDataSource(VectorDataSource::Baidu);
        feature.SetCoordinateSystem(VectorCoordinateSystem::Wgs84);
        feature.mainKey = style;
        feature.subKey = group;
        feature.drawOrder = order;
        // 数据格网覆盖层级只用于请求选择，不能当成要素可见范围限制 PNG 显示层级。
        feature.SetSourceProperties(
            {{"provider", "baidu"},
             {"coordinateSystem", "WGS84"},
             {"sourceCoordinateSystem", "BD09"},
             {"version", BaiduVectorClassification::kVersion},
             {"updateDate", BaiduVectorClassification::kUpdateDate},
             {"groupId", group},
             {"styleId", style},
             {"nativeZoom", tile.GetZoom()},
             {"order", order},
             {"tile", {{"x", tile.GetX()}, {"y", tile.GetY()}, {"z", tile.GetZoom()}}}});
        auto properties = feature.GetSourceProperties();
        properties["dataStartZoom"] = BaiduVectorTileGrid::StartZoom(tile.GetZoom());
        properties["dataEndZoom"] = BaiduVectorTileGrid::EndZoom(tile.GetZoom());
        properties["geometryCategory"] = group == 3    ? "point"
                                         : group == 4  ? "line"
                                         : group == 7  ? "polygon"
                                         : group == 8  ? "building"
                                         : group == 15 ? "overlay"
                                                       : "elevated";
        feature.SetSourceProperties(properties);
        return feature;
    }

    AmapVectorFeature Point(const Block& record, AmapVectorFeature feature) const
    {
        if (Header(record, 0) != 5)
        {
            throw std::runtime_error("暂不支持的百度地点头。");
        }
        const auto chunks = Children(record, 6);
        if (chunks.size() < 2)
        {
            throw std::runtime_error("百度地点缺少属性块。");
        }
        const auto& tile = options_.GetTile();
        const double span = BaiduVectorTileGrid::MercatorSpan(tile.GetZoom());
        const double x = Header(record, 2) / 100.;
        const double y = Header(record, 3) / 100.;
        const auto position = BaiduDown::BD09ToWGS84(BaiduDown::MercatorToBD09(
            {tile.GetX() * span + std::floor(x + 0.5), tile.GetY() * span + std::floor(y + 0.5)}));
        feature.geometryType = AmapVectorGeometryType::Point;
        feature.geometryParts = {{{position.lon, position.lat}}};
        feature.rank = Header(record, 4);
        const auto name = Text(chunks[1]);
        if (!name.empty())
        {
            feature.names.push_back({name, "zh"});
        }
        auto properties = feature.GetSourceProperties();
        properties["guid"] = Text(chunks[0]);
        properties["visibilityMask"] = Header(record, 1);
        properties["direction"] = Header(record, 5);
        properties["anchorMercator"] = {x, y};
        feature.SetSourceProperties(properties);
        return feature;
    }

    AmapVectorFeature Geometry(const Block& record, AmapVectorFeature feature) const
    {
        if (Header(record, 0) != 2)
        {
            throw std::runtime_error("暂不支持的百度要素头。");
        }
        const auto chunks = Children(record, 3);
        if (chunks.size() < 2)
        {
            throw std::runtime_error("百度要素缺少几何块。");
        }
        const auto vertices = Vertices(chunks[0]);
        const bool polygon = feature.subKey == 7 || feature.subKey == 8 || feature.subKey == 15;
        feature.geometryType =
            polygon ? AmapVectorGeometryType::Polygon : AmapVectorGeometryType::LineString;
        ClipperLib::Paths paths;
        if (polygon)
        {
            const size_t index = feature.subKey == 8 ? 1 : 2;
            if (chunks.size() <= index)
            {
                throw std::runtime_error("百度面缺少原始三角网格。");
            }
            paths = PolygonRings(vertices, chunks[index]);
        }
        else if (vertices.size() >= 2)
        {
            paths.push_back(vertices);
        }
        for (const auto& path : paths)
        {
            std::vector<AmapVectorCoordinate> part;
            part.reserve(path.size() + (polygon ? 1 : 0));
            for (const auto& point : path)
            {
                const auto position = BaiduVectorTileGrid::LocalToWgs84(
                    options_.GetTile(), point.X / 100., point.Y / 100.);
                part.push_back({position.lon, position.lat});
            }
            if (polygon && !part.empty())
            {
                part.push_back(part.front());
            }
            feature.geometryParts.push_back(std::move(part));
        }
        auto properties = feature.GetSourceProperties();
        properties["visibilityMask"] = Header(record, 1);
        properties["rawValue"] = Header(record, 2);
        properties["sourceVertexCount"] = vertices.size();
        if (feature.subKey == 8)
        {
            // TODO: 待确认百度高度字段单位后接入模型；当前只保留原值，不能当作米或楼层。
            properties["heightRaw"] = Header(record, 2);
            properties["heightUnit"] = "unverified";
        }
        if (feature.geometryParts.empty())
        {
            properties["geometryIssue"] = "empty-source-mesh";
        }
        feature.SetSourceProperties(properties);
        return feature;
    }

  private:
    void Require(size_t offset, size_t length) const
    {
        if (offset > bytes_.size() || length > bytes_.size() - offset)
        {
            throw std::runtime_error("百度二进制字段超出响应边界。");
        }
    }

    std::string_view bytes_;
    const BaiduVectorDecodeOptions& options_;
};
} // namespace

bool DownTool::BaiduVectorDecoder::DecodeTile(std::string_view bytes,
                                              const BaiduVectorDecodeOptions& options,
                                              AmapVectorDecodedData& decoded, std::string& error)
{
    try
    {
        BaiduVectorTileGrid::Validate(options.GetTile());
        const TileReader reader(bytes, options);
        if (bytes.size() < 40 || bytes.size() > 32 * 1024 * 1024 || reader.Read32(0) != 2 ||
            reader.Read32(4) != 2)
        {
            throw std::runtime_error("暂不支持的百度响应版本或错误响应。");
        }
        const auto sections = reader.Children({0, bytes.size()}, 4);
        if (sections.size() != 5)
        {
            throw std::runtime_error("暂不支持的百度顶层块结构。");
        }
        AmapVectorDecodedData parsed;
        parsed.status = true;
        parsed.version = std::string("Baidu ") + BaiduVectorClassification::kVersion;
        parsed.packetCount = 1;
        for (const auto& group : reader.Children(sections[0], 0))
        {
            if (reader.Header(group, 0) != 2)
            {
                throw std::runtime_error("暂不支持的百度图层头。");
            }
            const uint32_t id = reader.Header(group, 1);
            const uint32_t order = reader.Header(group, 2);
            if (id != 3 && id != 4 && id != 7 && id != 8 && id != 15 && id != 16)
            {
                continue;
            }
            for (const auto& style : reader.Children(group, 3))
            {
                if (reader.Header(style, 0) != 1)
                {
                    throw std::runtime_error("暂不支持的百度样式头。");
                }
                const auto styleId = reader.Header(style, 1);
                for (const auto& record : reader.Children(style, 2))
                {
                    reader.CheckCancellation();
                    auto feature = reader.BaseFeature(id, styleId, order);
                    parsed.features.push_back(id == 3
                                                  ? reader.Point(record, std::move(feature))
                                                  : reader.Geometry(record, std::move(feature)));
                }
            }
        }
        decoded = std::move(parsed);
        error.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "百度 GIS 解码失败：" + std::string(exception.what());
        return false;
    }
}
