#include "KmlDownloadRange.h"

#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <system_error>

#include "libxml/parser.h"
#include "libxml/tree.h"

namespace
{
constexpr uintmax_t kMaximumKmlFileBytes = 32u * 1024u * 1024u;

struct XmlDocumentDeleter
{
    void operator()(xmlDoc* document) const
    {
        xmlFreeDoc(document);
    }
};

struct XmlStringDeleter
{
    void operator()(xmlChar* value) const
    {
        xmlFree(value);
    }
};

bool NodeNameEquals(const xmlNode& node, const char* expected)
{
    return node.type == XML_ELEMENT_NODE &&
        xmlStrEqual(node.name, BAD_CAST expected) != 0;
}

const xmlNode* FindDescendant(
    const xmlNode* node, const char* expected, bool stopAtPolygon)
{
    for (const xmlNode* child = node == nullptr ? nullptr : node->children;
         child != nullptr;
         child = child->next)
    {
        if (NodeNameEquals(*child, expected))
        {
            return child;
        }
        if (stopAtPolygon && NodeNameEquals(*child, "Polygon"))
        {
            continue;
        }
        if (const xmlNode* descendant =
                FindDescendant(child, expected, stopAtPolygon))
        {
            return descendant;
        }
    }
    return nullptr;
}

std::optional<double> ParseDouble(std::string_view value)
{
    double parsed = 0.;
    const char* begin = value.data();
    const char* end = value.data() + value.size();
    const std::from_chars_result result =
        std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end ||
        !std::isfinite(parsed))
    {
        return std::nullopt;
    }
    return parsed;
}

bool SameCoordinate(
    const DownTool::LonLat& left, const DownTool::LonLat& right)
{
    return left.lon == right.lon && left.lat == right.lat;
}

std::optional<DownTool::Wgs84Ring> ParseCoordinates(
    std::string_view text, std::string& error)
{
    DownTool::Wgs84Ring ring;
    size_t cursor = 0;
    while (cursor < text.size())
    {
        while (cursor < text.size() &&
               std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
        {
            ++cursor;
        }
        if (cursor >= text.size())
        {
            break;
        }
        const size_t tokenEnd = text.find_first_of(" \t\r\n", cursor);
        const std::string_view token = text.substr(
            cursor,
            tokenEnd == std::string_view::npos ?
                text.size() - cursor : tokenEnd - cursor);
        const size_t firstComma = token.find(',');
        const size_t secondComma =
            firstComma == std::string_view::npos ?
                std::string_view::npos : token.find(',', firstComma + 1);
        if (firstComma == std::string_view::npos)
        {
            error = "KML coordinates 中存在缺少纬度的坐标。";
            return std::nullopt;
        }
        const std::optional<double> longitude =
            ParseDouble(token.substr(0, firstComma));
        const std::optional<double> latitude = ParseDouble(token.substr(
            firstComma + 1,
            secondComma == std::string_view::npos ?
                token.size() - firstComma - 1 :
                secondComma - firstComma - 1));
        if (!longitude.has_value() || !latitude.has_value() ||
            *longitude < -180. || *longitude > 180. ||
            *latitude < -90. || *latitude > 90.)
        {
            error = "KML coordinates 中存在无效的 WGS84 经纬度。";
            return std::nullopt;
        }
        const DownTool::LonLat coordinate{*longitude, *latitude};
        if (ring.empty() || !SameCoordinate(ring.back(), coordinate))
        {
            ring.push_back(coordinate);
        }
        cursor = tokenEnd == std::string_view::npos ?
            text.size() : tokenEnd + 1;
    }
    if (ring.size() >= 2 && SameCoordinate(ring.front(), ring.back()))
    {
        ring.pop_back();
    }
    if (ring.size() < 3)
    {
        error = "KML 多边形至少需要三个不同坐标点。";
        return std::nullopt;
    }
    return ring;
}

bool AppendPolygon(
    const xmlNode& polygonNode,
    DownTool::Wgs84Polygons& polygons,
    std::string& error)
{
    const xmlNode* outerBoundary =
        FindDescendant(&polygonNode, "outerBoundaryIs", true);
    const xmlNode* coordinates =
        FindDescendant(outerBoundary, "coordinates", true);
    if (coordinates == nullptr)
    {
        return true;
    }
    std::unique_ptr<xmlChar, XmlStringDeleter> content(
        xmlNodeGetContent(coordinates));
    if (!content)
    {
        error = "无法读取 KML 多边形坐标。";
        return false;
    }
    const std::optional<DownTool::Wgs84Ring> ring =
        ParseCoordinates(
            reinterpret_cast<const char*>(content.get()), error);
    if (!ring.has_value())
    {
        return false;
    }
    polygons.push_back(*ring);
    return true;
}

bool CollectPolygons(
    const xmlNode* node,
    DownTool::Wgs84Polygons& polygons,
    std::string& error)
{
    for (const xmlNode* current = node;
         current != nullptr;
         current = current->next)
    {
        if (NodeNameEquals(*current, "Polygon") &&
            !AppendPolygon(*current, polygons, error))
        {
            return false;
        }
        if (!CollectPolygons(current->children, polygons, error))
        {
            return false;
        }
    }
    return true;
}

std::string PathFileNameToUtf8(const std::filesystem::path& path)
{
    const std::u8string name = path.filename().u8string();
    return std::string(
        reinterpret_cast<const char*>(name.data()), name.size());
}
} // namespace

std::optional<DownTool::KmlDownloadRange>
DownTool::LoadKmlDownloadRange(
    const std::filesystem::path& path, std::string& error)
{
    std::error_code sizeError;
    const uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
    if (sizeError)
    {
        error = "无法读取 KML 文件：" + sizeError.message();
        return std::nullopt;
    }
    if (fileSize == 0 || fileSize > kMaximumKmlFileBytes)
    {
        error = fileSize == 0 ?
            "KML 文件为空。" : "KML 文件超过 32 MB 限制。";
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        error = "无法打开 KML 文件。";
        return std::nullopt;
    }
    const std::string xml{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof())
    {
        error = "读取 KML 文件失败。";
        return std::nullopt;
    }

    constexpr int parseOptions =
        XML_PARSE_NONET | XML_PARSE_COMPACT | XML_PARSE_NOERROR |
        XML_PARSE_NOWARNING;
    std::unique_ptr<xmlDoc, XmlDocumentDeleter> document(
        xmlReadMemory(
            xml.data(),
            static_cast<int>(xml.size()),
            "download-range.kml",
            nullptr,
            parseOptions));
    if (!document)
    {
        error = "KML XML 格式无效。";
        return std::nullopt;
    }

    KmlDownloadRange range;
    if (!CollectPolygons(xmlDocGetRootElement(document.get()),
                         range.polygonsWgs84,
                         error) ||
        !ValidateWgs84Polygons(range.polygonsWgs84, error))
    {
        return std::nullopt;
    }
    const std::optional<GeographicBounds> bounds =
        CalculateGeographicBounds(range.polygonsWgs84);
    if (!bounds.has_value())
    {
        error = "无法计算 KML 下载范围。";
        return std::nullopt;
    }
    range.bounds = *bounds;
    range.centerWgs84 = GeographicBoundsCenter(*bounds);
    range.boundingRadiusMeters =
        GeographicBoundsRadiusMeters(*bounds, range.centerWgs84);
    range.fileName = PathFileNameToUtf8(path);
    return range;
}
