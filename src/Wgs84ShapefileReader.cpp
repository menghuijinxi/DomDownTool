#include "Wgs84ShapefileReader.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

#include "shapefil.h"

namespace
{
using Json = nlohmann::json;
using ShapeHandle = std::unique_ptr<std::remove_pointer_t<SHPHandle>, decltype(&SHPClose)>;
using AttributeHandle = std::unique_ptr<std::remove_pointer_t<DBFHandle>, decltype(&DBFClose)>;
using ShapeObject = std::unique_ptr<SHPObject, decltype(&SHPDestroyObject)>;

std::string Utf8Path(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string Trim(std::string text)
{
    const auto space = [](unsigned char value) { return std::isspace(value); };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), space));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), space).base(), text.end());
    return text;
}

std::string Normalized(std::string_view text)
{
    std::string result;
    for (const unsigned char character : text)
    {
        if (character != '_' && character != '-' && !std::isspace(character))
        {
            result += static_cast<char>(std::tolower(character));
        }
    }
    return result;
}

std::filesystem::path Sidecar(std::filesystem::path path, const char* extension)
{
    path.replace_extension(extension);
    if (std::filesystem::exists(path))
    {
        return path;
    }
    std::string upper(extension);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
    path.replace_extension(upper);
    return path;
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// 只检查 PRJ 的地理坐标元数据，不实现投影或坐标转换；未知定义一律拒绝。
class PrjNode
{
  public:
    const std::string& GetName() const { return name_; }
    const std::vector<std::string>& GetTexts() const { return texts_; }
    const std::vector<double>& GetNumbers() const { return numbers_; }
    const std::vector<PrjNode>& GetChildren() const { return children_; }
    const PrjNode* Find(std::string_view name) const
    {
        const auto found = std::find_if(children_.begin(), children_.end(),
                                        [name](const auto& node) { return node.name_ == name; });
        return found == children_.end() ? nullptr : &*found;
    }

    static std::optional<PrjNode> Parse(std::string_view text)
    {
        if (text.starts_with("\xef\xbb\xbf"))
        {
            text.remove_prefix(3);
        }
        size_t position = 0;
        auto node = ParseNode(text, position, 0);
        SkipSpace(text, position);
        return position == text.size() ? node : std::nullopt;
    }

  private:
    static void SkipSpace(std::string_view text, size_t& position)
    {
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])))
        {
            ++position;
        }
    }

    static std::string Identifier(std::string_view text, size_t& position)
    {
        SkipSpace(text, position);
        const auto start = position;
        while (position < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[position])) || text[position] == '_'))
        {
            ++position;
        }
        return Normalized(text.substr(start, position - start));
    }

    static std::optional<PrjNode> ParseNode(std::string_view text, size_t& position, int depth)
    {
        if (depth > 16)
        {
            return std::nullopt;
        }
        PrjNode node;
        node.name_ = Identifier(text, position);
        SkipSpace(text, position);
        if (node.name_.empty() || position == text.size() ||
            (text[position] != '[' && text[position] != '('))
        {
            return std::nullopt;
        }
        const char close = text[position++] == '[' ? ']' : ')';
        while (position < text.size())
        {
            SkipSpace(text, position);
            if (position == text.size())
            {
                return std::nullopt;
            }
            if (text[position] == '"')
            {
                ++position;
                std::string value;
                bool closed = false;
                while (position < text.size())
                {
                    const char character = text[position++];
                    if (character == '"')
                    {
                        if (position < text.size() && text[position] == '"')
                        {
                            ++position;
                            value += '"';
                            continue;
                        }
                        closed = true;
                        break;
                    }
                    value += character;
                }
                if (!closed)
                {
                    return std::nullopt;
                }
                node.texts_.push_back(std::move(value));
            }
            else if (std::isalpha(static_cast<unsigned char>(text[position])))
            {
                const auto start = position;
                auto identifier = Identifier(text, position);
                SkipSpace(text, position);
                if (position < text.size() && (text[position] == '[' || text[position] == '('))
                {
                    position = start;
                    auto child = ParseNode(text, position, depth + 1);
                    if (!child)
                    {
                        return std::nullopt;
                    }
                    node.children_.push_back(std::move(*child));
                }
                else
                {
                    node.texts_.push_back(std::move(identifier));
                }
            }
            else
            {
                double number = 0.;
                const char* begin = text.data() + position;
                const auto parsed = std::from_chars(begin, text.data() + text.size(), number);
                if (parsed.ec != std::errc{} || !std::isfinite(number))
                {
                    return std::nullopt;
                }
                position += static_cast<size_t>(parsed.ptr - begin);
                node.numbers_.push_back(number);
            }
            SkipSpace(text, position);
            if (position < text.size() && text[position] == close)
            {
                ++position;
                return node;
            }
            if (position == text.size() || text[position++] != ',')
            {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::string name_;
    std::vector<std::string> texts_;
    std::vector<double> numbers_;
    std::vector<PrjNode> children_;
};

bool IsDegreeUnit(const PrjNode* node)
{
    return node && node->GetNumbers().size() == 1 &&
           std::abs(node->GetNumbers()[0] - 0.017453292519943295) < 1.e-16;
}

bool IsWgs84(const PrjNode& root)
{
    if (root.GetName() != "geogcs" && root.GetName() != "geogcrs" && root.GetName() != "geodcrs")
    {
        return false;
    }
    const std::vector<std::string> supportedRootChildren = {
        "datum",     "ensemble", "primem", "unit", "angleunit", "cs",    "axis",
        "authority", "id",       "scope",  "area", "bbox",      "usage", "remark"};
    for (const auto& child : root.GetChildren())
    {
        if (std::ranges::find(supportedRootChildren, child.GetName()) ==
            supportedRootChildren.end())
        {
            return false;
        }
        if ((child.GetName() == "authority" || child.GetName() == "id") &&
            !child.GetTexts().empty() && Normalized(child.GetTexts()[0]) == "epsg")
        {
            const bool text4326 = child.GetTexts().size() == 2 && child.GetTexts()[1] == "4326";
            const bool number4326 = child.GetNumbers() == std::vector<double>{4326.};
            if (!text4326 && !number4326)
            {
                return false;
            }
        }
        if (child.GetName() == "axis")
        {
            const auto& texts = child.GetTexts();
            if (texts.size() != 2 || (texts[1] != "north" && texts[1] != "east"))
            {
                return false;
            }
        }
    }
    const auto* datum = root.Find("datum");
    if (!datum)
    {
        datum = root.Find("ensemble");
    }
    if (!datum || datum->GetTexts().empty())
    {
        return false;
    }
    const auto name = Normalized(datum->GetTexts().front());
    if (name != "dwgs1984" && name != "wgs1984" && name != "wgs84" &&
        name != "worldgeodeticsystem1984" && name != "worldgeodeticsystem1984ensemble")
    {
        return false;
    }
    const auto* ellipsoid = datum->Find("spheroid");
    if (!ellipsoid)
    {
        ellipsoid = datum->Find("ellipsoid");
    }
    if (!ellipsoid || ellipsoid->GetNumbers().size() != 2 ||
        ellipsoid->GetNumbers()[0] != 6378137. ||
        std::abs(ellipsoid->GetNumbers()[1] - 298.257223563) > 1.e-10)
    {
        return false;
    }
    if (const auto* unit = ellipsoid->Find("lengthunit");
        unit && (unit->GetNumbers().size() != 1 || unit->GetNumbers()[0] != 1.))
    {
        return false;
    }
    const auto* prime = root.Find("primem");
    if (!prime || prime->GetNumbers().size() != 1 || prime->GetNumbers()[0] != 0.)
    {
        return false;
    }
    if (const auto* shift = datum->Find("towgs84");
        shift &&
        ((shift->GetNumbers().size() != 3 && shift->GetNumbers().size() != 7) ||
         std::ranges::any_of(shift->GetNumbers(), [](double value) { return value != 0.; })))
    {
        return false;
    }
    if (root.Find("extension") || datum->Find("dynamic"))
    {
        return false;
    }
    if (root.GetName() == "geogcs")
    {
        return IsDegreeUnit(root.Find("unit"));
    }
    const auto* cs = root.Find("cs");
    if (!cs || cs->GetTexts() != std::vector<std::string>{"ellipsoidal"} ||
        cs->GetNumbers() != std::vector<double>{2.})
    {
        return false;
    }
    size_t axes = 0;
    for (const auto& child : root.GetChildren())
    {
        if (child.GetName() == "axis")
        {
            if (!IsDegreeUnit(child.Find("angleunit")) && !IsDegreeUnit(root.Find("angleunit")))
            {
                return false;
            }
            ++axes;
        }
    }
    return axes == 2;
}

bool ValidatePrj(const std::filesystem::path& path, std::string& error)
{
    const auto prjPath = Sidecar(path, ".prj");
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(prjPath, sizeError);
    if (sizeError || size == 0 || size > 65536)
    {
        error = "暂不支持缺少有效 .prj 的 SHP，无法确认坐标系；当前仅支持 WGS84 经纬度。";
        return false;
    }
    const auto node = PrjNode::Parse(ReadText(prjPath));
    if (!node || !IsWgs84(*node))
    {
        error = "暂不支持此 SHP 坐标系；当前仅支持 WGS84 经纬度（EPSG:4326），"
                "请在 GIS 软件中转换后再导入。";
        return false;
    }
    return true;
}

std::optional<DownTool::AmapVectorGeometryType> GeometryType(int type)
{
    using Type = DownTool::AmapVectorGeometryType;
    switch (type)
    {
    case SHPT_POINT:
    case SHPT_POINTM:
    case SHPT_POINTZ:
    case SHPT_MULTIPOINT:
    case SHPT_MULTIPOINTM:
    case SHPT_MULTIPOINTZ:
        return Type::Point;
    case SHPT_ARC:
    case SHPT_ARCM:
    case SHPT_ARCZ:
        return Type::LineString;
    case SHPT_POLYGON:
    case SHPT_POLYGONM:
    case SHPT_POLYGONZ:
        return Type::Polygon;
    default:
        return std::nullopt;
    }
}

std::optional<unsigned> TextCodePage(DBFHandle attributes, std::string& error);
std::optional<std::string> DecodeText(const std::string& text, unsigned codePage);

class OpenShapefile
{
  public:
    bool Open(const std::filesystem::path& path, DownTool::ShapefileInfo& info, std::string& error)
    {
        if (Normalized(path.extension().string()) != ".shp" || !ValidatePrj(path, error))
        {
            if (error.empty())
            {
                error = "请选择 .shp 文件。";
            }
            return false;
        }
        SAHooks hooks{};
#ifdef SHPAPI_UTF8_HOOKS
        SASetupUtf8Hooks(&hooks);
#else
        SASetupDefaultHooks(&hooks);
#endif
        shapes_.reset(SHPOpenLL(Utf8Path(path).c_str(), "rb", &hooks));
        attributes_.reset(DBFOpenLL(Utf8Path(Sidecar(path, ".dbf")).c_str(), "rb", &hooks));
        if (!shapes_ || !attributes_)
        {
            error = "无法读取 SHP 配套文件，请确保 .shp、.shx、.dbf 位于同一目录且未损坏。";
            return false;
        }
        double minimum[4]{};
        double maximum[4]{};
        int count = 0;
        int type = 0;
        SHPGetInfo(shapes_.get(), &count, &type, minimum, maximum);
        const auto geometryType = GeometryType(type);
        if (!geometryType)
        {
            error = "暂不支持此 SHP 几何类型，仅支持点、多点、线和面。";
            return false;
        }
        if (count < 0 || count != DBFGetRecordCount(attributes_.get()))
        {
            error = "SHP 几何与 DBF 属性记录数不一致。";
            return false;
        }
        if (count > 0 && (!std::isfinite(minimum[0]) || !std::isfinite(minimum[1]) ||
                          !std::isfinite(maximum[0]) || !std::isfinite(maximum[1]) ||
                          minimum[0] < -180. || maximum[0] > 180. || minimum[1] < -90. ||
                          maximum[1] > 90. || minimum[0] > maximum[0] || minimum[1] > maximum[1]))
        {
            error = "暂不支持此 SHP 坐标：超出 WGS84 经纬度范围，请检查 .prj 定义。";
            return false;
        }
        std::vector<std::string> fields;
        const auto codePage = TextCodePage(attributes_.get(), error);
        if (!codePage)
        {
            return false;
        }
        for (int index = 0; index < DBFGetFieldCount(attributes_.get()); ++index)
        {
            char name[12]{};
            const auto fieldType =
                DBFGetFieldInfo(attributes_.get(), index, name, nullptr, nullptr);
            if (fieldType == FTInteger || fieldType == FTDouble)
            {
                const auto decoded = DecodeText(name, *codePage);
                if (!decoded)
                {
                    error = "DBF 字段名称与字符编码不匹配，请检查 .cpg 文件。";
                    return false;
                }
                fields.push_back(*decoded);
            }
        }
        info.SetGeometryType(*geometryType);
        info.SetBounds({minimum[0], maximum[0], minimum[1], maximum[1]});
        info.SetFeatureCount(count);
        info.SetNumericFields(fields);
        return true;
    }

    SHPHandle GetShapes() const { return shapes_.get(); }
    DBFHandle GetAttributes() const { return attributes_.get(); }

  private:
    ShapeHandle shapes_{nullptr, &SHPClose};
    AttributeHandle attributes_{nullptr, &DBFClose};
};

std::optional<unsigned> TextCodePage(DBFHandle attributes, std::string& error)
{
    const char* raw = DBFGetCodePage(attributes);
    const std::string encoding = raw ? Normalized(raw) : "";
    if (encoding.empty() || encoding == "utf8" || encoding == "65001")
    {
        return 65001;
    }
    if (encoding == "gbk" || encoding == "gb2312" || encoding == "936" || encoding == "ldid/77")
    {
        return 936;
    }
    if (encoding == "gb18030" || encoding == "54936")
    {
        return 54936;
    }
    if (encoding == "1252" || encoding == "ldid/87")
    {
        return 1252;
    }
    error = "暂不支持此 DBF 字符编码：" + std::string(raw) +
            "；请将属性转换为 UTF-8，并提供 .cpg 文件。";
    return std::nullopt;
}

std::optional<std::string> DecodeText(const std::string& text, unsigned codePage)
{
    if (text.empty())
    {
        return text;
    }
#ifdef _WIN32
    const auto count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (count == 0)
    {
        return std::nullopt;
    }
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        wide.data(), count);
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, nullptr, 0, nullptr, nullptr);
    if (bytes == 0)
    {
        return std::nullopt;
    }
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), count, result.data(), bytes, nullptr, nullptr);
    return result;
#else
    if (codePage != 65001)
    {
        return std::nullopt;
    }
    try
    {
        static_cast<void>(Json(text).dump());
    }
    catch (const Json::type_error&)
    {
        return std::nullopt;
    }
    return text;
#endif
}

bool ReadProperties(DBFHandle attributes, int record, unsigned codePage, Json& properties)
{
    properties = Json::object();
    for (int index = 0; index < DBFGetFieldCount(attributes); ++index)
    {
        char rawName[12]{};
        const auto type = DBFGetFieldInfo(attributes, index, rawName, nullptr, nullptr);
        const auto name = DecodeText(rawName, codePage);
        if (!name)
        {
            return false;
        }
        if (DBFIsAttributeNULL(attributes, record, index))
        {
            properties[*name] = nullptr;
        }
        else if (type == FTInteger || type == FTDouble)
        {
            const std::string text = Trim(DBFReadStringAttribute(attributes, record, index));
            if (!text.empty() && text.find_first_of(".eE") == std::string::npos)
            {
                std::string_view integer(text);
                if (integer.starts_with('+'))
                {
                    integer.remove_prefix(1);
                }
                if (integer.starts_with('-'))
                {
                    int64_t value = 0;
                    const auto parsed =
                        std::from_chars(integer.data(), integer.data() + integer.size(), value);
                    properties[*name] =
                        parsed.ec == std::errc{} && parsed.ptr == integer.data() + integer.size()
                            ? Json(value)
                            : Json(text);
                }
                else
                {
                    uint64_t value = 0;
                    const auto parsed =
                        std::from_chars(integer.data(), integer.data() + integer.size(), value);
                    properties[*name] =
                        parsed.ec == std::errc{} && parsed.ptr == integer.data() + integer.size()
                            ? Json(value)
                            : Json(text);
                }
                continue;
            }
            auto number = Json::parse(text, nullptr, false);
            if (number.is_discarded())
            {
                std::string_view numeric(text);
                if (numeric.starts_with('+'))
                {
                    numeric.remove_prefix(1);
                }
                double value = 0.;
                const auto parsed =
                    std::from_chars(numeric.data(), numeric.data() + numeric.size(), value);
                if (parsed.ec == std::errc{} && parsed.ptr == numeric.data() + numeric.size())
                {
                    number = value;
                }
            }
            if (!number.is_number() ||
                (number.is_number_float() && !std::isfinite(number.get<double>())))
            {
                return false;
            }
            properties[*name] = std::move(number);
        }
        else if (type == FTLogical)
        {
            const char* value = DBFReadLogicalAttribute(attributes, record, index);
            properties[*name] = value[0] == 'T' || value[0] == 't' || value[0] == 'Y';
        }
        else
        {
            const auto text =
                DecodeText(DBFReadStringAttribute(attributes, record, index), codePage);
            if (!text)
            {
                return false;
            }
            properties[*name] = *text;
        }
    }
    return true;
}

const Json* FindProperty(const Json& properties, std::string_view name)
{
    for (const auto& [key, value] : properties.items())
    {
        if (Normalized(key) == Normalized(name))
        {
            return &value;
        }
    }
    return nullptr;
}

bool AssignProperties(const DownTool::ShapefileImportOptions& options, const Json& properties,
                      DownTool::AmapVectorFeature& feature, std::string& error)
{
    feature.SetSourceProperties(properties);
    for (const auto name : {"name", "名称", "名字"})
    {
        const auto* value = FindProperty(properties, name);
        if (value && value->is_string() && !value->get_ref<const std::string&>().empty())
        {
            feature.names.push_back({value->get<std::string>(), ""});
            break;
        }
    }
    if (options.GetHeightField() == "-")
    {
        return true;
    }
    const Json* height = nullptr;
    if (!options.GetHeightField().empty())
    {
        height = FindProperty(properties, options.GetHeightField());
        if (!height)
        {
            error = "SHP 中不存在指定的高度字段：" + options.GetHeightField();
            return false;
        }
    }
    else
    {
        for (const auto name : {"height_m", "height", "hgt", "building_h", "高度", "建筑高度"})
        {
            height = FindProperty(properties, name);
            if (height)
            {
                break;
            }
        }
    }
    if (height && !height->is_null())
    {
        if (!height->is_number() || !std::isfinite(height->get<double>()) ||
            height->get<double>() < 0. || height->get<double>() > 100000.)
        {
            error = "SHP 高度字段必须是 0 到 100000 之间的米数。";
            return false;
        }
        const double meters = height->get<double>();
        feature.SetHeightMeters(meters);
        feature.hasHeight = meters > 0.;
        feature.height = static_cast<uint64_t>(std::ceil(meters));
    }
    return true;
}

bool ReadGeometry(const SHPObject& object, DownTool::AmapVectorFeature& feature,
                  const std::function<bool()>& cancellationRequested)
{
    const auto type = GeometryType(object.nSHPType);
    if (!type || *type != feature.geometryType || object.nVertices <= 0 ||
        (*type != DownTool::AmapVectorGeometryType::Point && object.nParts <= 0))
    {
        return false;
    }
    const int parts =
        *type == DownTool::AmapVectorGeometryType::Point ? object.nVertices : object.nParts;
    for (int part = 0; part < parts; ++part)
    {
        const bool point = *type == DownTool::AmapVectorGeometryType::Point;
        const int start = point ? part : object.panPartStart[part];
        const int end = point              ? part + 1
                        : part + 1 < parts ? object.panPartStart[part + 1]
                                           : object.nVertices;
        const int minimum = point ? 1 : *type == DownTool::AmapVectorGeometryType::Polygon ? 3 : 2;
        if (start < 0 || end > object.nVertices || end - start < minimum ||
            (!point && part == 0 && start != 0))
        {
            return false;
        }
        std::vector<DownTool::AmapVectorCoordinate> ring;
        ring.reserve(static_cast<size_t>(end - start));
        for (int index = start; index < end; ++index)
        {
            if ((index % 256 == 0 && cancellationRequested && cancellationRequested()) ||
                !std::isfinite(object.padfX[index]) || !std::isfinite(object.padfY[index]) ||
                object.padfX[index] < -180. || object.padfX[index] > 180. ||
                object.padfY[index] < -90. || object.padfY[index] > 90.)
            {
                return false;
            }
            ring.push_back({object.padfX[index], object.padfY[index]});
        }
        if (*type == DownTool::AmapVectorGeometryType::Polygon &&
            (ring.front().longitude != ring.back().longitude ||
             ring.front().latitude != ring.back().latitude))
        {
            ring.push_back(ring.front());
        }
        feature.geometryParts.push_back(std::move(ring));
    }
    return true;
}
} // namespace

bool DownTool::Wgs84ShapefileReader::Inspect(const std::filesystem::path& path, ShapefileInfo& info,
                                             std::string& error)
{
    error.clear();
    try
    {
        OpenShapefile opened;
        return opened.Open(path, info, error);
    }
    catch (const std::filesystem::filesystem_error& exception)
    {
        error = "无法读取 SHP 配套文件：" + std::string(exception.what());
        return false;
    }
}

bool DownTool::Wgs84ShapefileReader::Read(
    const ShapefileImportOptions& options, AmapVectorDecodedData& decoded, std::string& error,
    const std::function<bool()>& cancellationRequested,
    const std::function<void(size_t, size_t)>& progressCallback)
{
    error.clear();
    if (cancellationRequested && cancellationRequested())
    {
        error = "SHP 导入已取消。";
        return false;
    }
    ShapefileInfo info;
    OpenShapefile opened;
    if (!opened.Open(options.GetPath(), info, error))
    {
        return false;
    }
    const auto codePage = TextCodePage(opened.GetAttributes(), error);
    if (!codePage)
    {
        return false;
    }
    AmapVectorDecodedData result;
    result.status = true;
    result.version = "WGS84 SHP";
    for (int record = 0; record < info.GetFeatureCount(); ++record)
    {
        if (cancellationRequested && cancellationRequested())
        {
            error = "SHP 导入已取消。";
            return false;
        }
        if (record % 256 == 0 && progressCallback)
        {
            progressCallback(static_cast<size_t>(record), info.GetFeatureCount());
        }
        if (DBFIsRecordDeleted(opened.GetAttributes(), record))
        {
            continue;
        }
        ShapeObject object(SHPReadObject(opened.GetShapes(), record), &SHPDestroyObject);
        if (object && object->nSHPType == SHPT_NULL)
        {
            continue;
        }
        AmapVectorFeature feature;
        feature.geometryType = info.GetGeometryType();
        feature.SetCoordinateSystem(VectorCoordinateSystem::Wgs84);
        feature.layerName =
            options.GetLayerName().empty()
                ? feature.geometryType == AmapVectorGeometryType::Polygon      ? "building"
                  : feature.geometryType == AmapVectorGeometryType::LineString ? "road"
                                                                               : "poi"
                : options.GetLayerName();
        feature.uid = static_cast<uint64_t>(record) + 1;
        feature.maxZoom = 30;
        Json properties;
        if (!object || !ReadGeometry(*object, feature, cancellationRequested) ||
            !ReadProperties(opened.GetAttributes(), record, *codePage, properties) ||
            !AssignProperties(options, properties, feature, error))
        {
            error = cancellationRequested && cancellationRequested()
                        ? "SHP 导入已取消。"
                        : "SHP 第 " + std::to_string(record + 1) + " 条记录无效：" +
                              (error.empty() ? "几何、坐标或 DBF 属性损坏或编码不匹配。" : error);
            return false;
        }
        result.features.push_back(std::move(feature));
    }
    if (progressCallback)
    {
        progressCallback(info.GetFeatureCount(), info.GetFeatureCount());
    }
    decoded = std::move(result);
    return true;
}
