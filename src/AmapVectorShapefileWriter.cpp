#include "AmapVectorShapefileWriter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "shapefil.h"

#include "GCJ02GoogleDownBase.h"

namespace
{
using Json = nlohmann::json;
using ShapeHandle = std::unique_ptr<std::remove_pointer_t<SHPHandle>, decltype(&SHPClose)>;
using AttributeHandle = std::unique_ptr<std::remove_pointer_t<DBFHandle>, decltype(&DBFClose)>;
using ShapeObject = std::unique_ptr<SHPObject, decltype(&SHPDestroyObject)>;
using Field = std::tuple<const char*, const char*, DBFFieldType, int>;

constexpr std::array<Field, 18> kFields = {{
    {"layer", "layer", FTString, 32},
    {"mainKey", "mainKey", FTDouble, 10},
    {"subKey", "subKey", FTDouble, 10},
    {"resolution", "resolution", FTDouble, 10},
    {"minZoom", "minZoom", FTDouble, 10},
    {"maxZoom", "maxZoom", FTDouble, 10},
    {"drawOrder", "drawOrder", FTDouble, 10},
    {"rank", "rank", FTDouble, 10},
    {"uid", "uid", FTString, 20},
    {"name", "name", FTString, 254},
    {"language", "language", FTString, 64},
    {"names", "names", FTString, 254},
    {"shield", "shield", FTString, 254},
    {"shieldType", "shieldType", FTDouble, 10},
    {"hasHeight", "hasHeight", FTLogical, 1},
    {"height", "height", FTDouble, 20},
    {"buildingId", "buildingIds", FTString, 254},
    {"internType", "internalType", FTString, 254},
}};

constexpr std::string_view kWgs84Prj =
    R"(GEOGCS["GCS_WGS_1984",DATUM["D_WGS_1984",SPHEROID["WGS_1984",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["Degree",0.0174532925199433]])";

std::string PathToUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path WithSuffix(std::filesystem::path path, const char* suffix)
{
    path += suffix;
    return path;
}

bool CheckCancellation(const std::function<bool()>& requested, std::string& error)
{
    if (requested && requested())
    {
        error = "SHP 导出已取消。";
        return true;
    }
    return false;
}

class StagingDirectory
{
  public:
    explicit StagingDirectory(const std::filesystem::path& outputDirectory)
        : path_(outputDirectory /
                (".shp-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
    }

    ~StagingDirectory()
    {
        if (created_)
        {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    const std::filesystem::path& GetPath() const { return path_; }

    bool Create(std::string& error)
    {
        std::error_code createError;
        std::filesystem::create_directories(path_.parent_path(), createError);
        if (!createError)
        {
            created_ = std::filesystem::create_directory(path_, createError);
        }
        if (!created_)
        {
            error = "无法创建 SHP 临时目录：" + createError.message();
        }
        return created_;
    }

  private:
    std::filesystem::path path_;
    bool created_ = false;
};

Json FeatureProperties(const DownTool::AmapVectorFeature& feature)
{
    Json names = Json::array();
    for (const auto& name : feature.names)
    {
        names.push_back({{"text", name.text}, {"language", name.language}});
    }
    Json properties = {{"layer", feature.layerName},
                       {"mainKey", feature.mainKey},
                       {"subKey", feature.subKey},
                       {"resolution", feature.resolution},
                       {"minZoom", feature.minZoom},
                       {"maxZoom", feature.maxZoom},
                       {"drawOrder", feature.drawOrder},
                       {"rank", feature.rank},
                       {"uid", feature.uid},
                       {"names", std::move(names)},
                       {"shield", feature.shield},
                       {"shieldType", feature.shieldType},
                       {"hasHeight", feature.hasHeight},
                       {"height", feature.hasHeight ? Json(feature.height) : Json(nullptr)},
                       {"buildingIds", feature.buildingIds},
                       {"internalType", feature.internalType}};
    const auto name = std::find_if(feature.names.begin(), feature.names.end(),
                                   [](const auto& item) { return !item.text.empty(); });
    properties["name"] = name == feature.names.end() ? "" : name->text;
    properties["language"] = name == feature.names.end() ? "" : name->language;
    return properties;
}

std::string Utf8Prefix(std::string text, size_t maximumBytes)
{
    if (text.size() > maximumBytes)
    {
        size_t end = maximumBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
        {
            --end;
        }
        text.resize(end);
    }
    return text;
}

bool WriteAttributes(DBFHandle handle, int record, const Json& properties)
{
    for (size_t field = 0; field < kFields.size(); ++field)
    {
        const auto& [name, key, type, width] = kFields[field];
        const auto& value = properties.at(key);
        const int index = static_cast<int>(field);
        int written = 0;
        if (value.is_null())
        {
            written = DBFWriteNULLAttribute(handle, record, index);
        }
        else if (type == FTLogical)
        {
            written =
                DBFWriteLogicalAttribute(handle, record, index, value.get<bool>() ? 'T' : 'F');
        }
        else
        {
            std::string text = value.is_string() ? value.get<std::string>() : value.dump();
            if (type == FTString)
            {
                // DBF 按字节限制宽度；完整值另存属性 JSON，避免截断 UTF-8 字符。
                text = Utf8Prefix(std::move(text), static_cast<size_t>(width));
                written = DBFWriteStringAttribute(handle, record, index, text.c_str());
            }
            else
            {
                // 直接写十进制文本，避免 uint64 高度先转 double 后损失精度。
                text.insert(0, static_cast<size_t>(width) - text.size(), ' ');
                written = DBFWriteAttributeDirectly(handle, record, index, text.c_str());
            }
        }
        if (!written)
        {
            return false;
        }
    }
    return true;
}

ShapeObject MakeShape(const DownTool::AmapVectorFeature& feature, int shapeType,
                      const std::function<bool()>& cancellationRequested, std::string& error)
{
    ShapeObject object(nullptr, &SHPDestroyObject);
    std::vector<int> starts;
    std::vector<double> longitude;
    std::vector<double> latitude;
    if (feature.geometryParts.empty())
    {
        error = "SHP 要素缺少几何。";
        return object;
    }
    for (const auto& part : feature.geometryParts)
    {
        const size_t minimum = shapeType == SHPT_POLYGON ? 3 : shapeType == SHPT_ARC ? 2 : 1;
        if (part.size() < minimum ||
            part.size() >=
                static_cast<size_t>(std::numeric_limits<int>::max()) - longitude.size() ||
            starts.size() >= static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            error = "SHP 要素的顶点数量无效或超过格式上限。";
            return object;
        }
        starts.push_back(static_cast<int>(longitude.size()));
        for (const auto& coordinate : part)
        {
            if (CheckCancellation(cancellationRequested, error))
            {
                return object;
            }
            if (!std::isfinite(coordinate.longitude) || !std::isfinite(coordinate.latitude) ||
                coordinate.longitude < -180. || coordinate.longitude > 180. ||
                coordinate.latitude < -90. || coordinate.latitude > 90.)
            {
                error = "SHP 要素包含无效经纬度。";
                return object;
            }
            const auto wgs84 = DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(
                {coordinate.longitude, coordinate.latitude});
            longitude.push_back(wgs84.lon);
            latitude.push_back(wgs84.lat);
        }
        const auto start = static_cast<size_t>(starts.back());
        if (shapeType == SHPT_POLYGON &&
            (longitude[start] != longitude.back() || latitude[start] != latitude.back()))
        {
            longitude.push_back(longitude[start]);
            latitude.push_back(latitude[start]);
        }
    }
    const bool hasParts = shapeType == SHPT_ARC || shapeType == SHPT_POLYGON;
    object.reset(SHPCreateObject(shapeType, -1, hasParts ? static_cast<int>(starts.size()) : 0,
                                 hasParts ? starts.data() : nullptr, nullptr,
                                 static_cast<int>(longitude.size()), longitude.data(),
                                 latitude.data(), nullptr, nullptr));
    if (!object)
    {
        error = "无法创建 SHP 几何。";
    }
    return object;
}

bool WriteText(const std::filesystem::path& path, std::string_view text, std::string& error)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    output.close();
    if (!output)
    {
        error = "无法写入 SHP 配套文件：" + PathToUtf8(path);
        return false;
    }
    return true;
}

class LayerWriter
{
  public:
    explicit LayerWriter(const std::filesystem::path& basePath)
        : basePath_(basePath), shapes_(nullptr, &SHPClose), attributes_(nullptr, &DBFClose)
    {
    }

    bool Open(int shapeType, std::string& error)
    {
        SAHooks hooks{};
#ifdef SHPAPI_UTF8_HOOKS
        SASetupUtf8Hooks(&hooks);
#else
        SASetupDefaultHooks(&hooks);
#endif
        const auto path = PathToUtf8(basePath_);
        shapes_.reset(SHPCreateLL(path.c_str(), shapeType, &hooks));
        // 先写 CPG，避免 Shapelib 在 CPG 无法创建时操作空文件句柄。
        if (!shapes_ || !WriteText(WithSuffix(basePath_, ".cpg"), "UTF-8", error))
        {
            if (error.empty())
            {
                error = "无法创建 SHP 文件。";
            }
            return false;
        }
        attributes_.reset(DBFCreateLL(path.c_str(), "UTF-8", &hooks));
        if (!attributes_)
        {
            error = "无法创建 SHP 属性表。";
            return false;
        }
        for (size_t index = 0; index < kFields.size(); ++index)
        {
            const auto& [name, key, type, width] = kFields[index];
            if (DBFAddField(attributes_.get(), name, type, width, 0) != static_cast<int>(index))
            {
                error = "无法创建 SHP 属性字段：" + std::string(name);
                return false;
            }
        }
        fullAttributes_.open(WithSuffix(basePath_, ".attributes.json"), std::ios::binary);
        fullAttributes_ << "{\"features\":[";
        if (!fullAttributes_)
        {
            error = "无法创建 SHP 完整属性文件。";
            return false;
        }
        return true;
    }

    bool Append(const DownTool::AmapVectorFeature& feature, int shapeType,
                const std::function<bool()>& cancellationRequested, std::string& error)
    {
        auto object = MakeShape(feature, shapeType, cancellationRequested, error);
        if (!object)
        {
            return false;
        }
        if (shapeType == SHPT_POLYGON)
        {
            SHPRewindObject(shapes_.get(), object.get());
        }
        if (CheckCancellation(cancellationRequested, error))
        {
            return false;
        }
        const int record = SHPWriteObject(shapes_.get(), -1, object.get());
        const auto properties = FeatureProperties(feature);
        if (record != count_ || !WriteAttributes(attributes_.get(), record, properties))
        {
            error = "无法写入 SHP 几何或属性记录。";
            return false;
        }
        if (count_ != 0)
        {
            fullAttributes_ << ',';
        }
        fullAttributes_ << Json({{"record", record}, {"properties", properties}}).dump();
        if (!fullAttributes_)
        {
            error = "无法写入 SHP 完整属性。";
            return false;
        }
        ++count_;
        return true;
    }

    bool Close(std::string& error)
    {
        fullAttributes_ << "]}\n";
        fullAttributes_.close();
        attributes_.reset();
        shapes_.reset();
        if (!fullAttributes_)
        {
            error = "无法完成 SHP 完整属性写入。";
            return false;
        }
        return WriteText(WithSuffix(basePath_, ".prj"), kWgs84Prj, error);
    }

    int GetCount() const { return count_; }

  private:
    std::filesystem::path basePath_;
    ShapeHandle shapes_;
    AttributeHandle attributes_;
    std::ofstream fullAttributes_;
    int count_ = 0;
};
} // namespace

bool DownTool::AmapVectorShapefileWriter::WriteDirectory(
    const AmapVectorDecodedData& decodedData, const std::filesystem::path& outputDirectory,
    std::string& error, const std::function<bool()>& cancellationRequested)
{
    error.clear();
    if (outputDirectory.empty())
    {
        error = "SHP 输出目录不能为空。";
        return false;
    }
    if (CheckCancellation(cancellationRequested, error))
    {
        return false;
    }
    StagingDirectory staging(outputDirectory);
    if (!staging.Create(error))
    {
        return false;
    }
    Json layers = Json::array();
    const std::array<std::tuple<AmapVectorGeometryType, int, const char*>, 3> types = {{
        {AmapVectorGeometryType::Point, SHPT_MULTIPOINT, "points"},
        {AmapVectorGeometryType::LineString, SHPT_ARC, "lines"},
        {AmapVectorGeometryType::Polygon, SHPT_POLYGON, "polygons"},
    }};
    for (const auto& [geometryType, shapeType, name] : types)
    {
        LayerWriter writer(staging.GetPath() / name);
        bool opened = false;
        for (const auto& feature : decodedData.features)
        {
            if (CheckCancellation(cancellationRequested, error))
            {
                return false;
            }
            if (feature.geometryType != geometryType)
            {
                continue;
            }
            if (!opened)
            {
                if (!writer.Open(shapeType, error))
                {
                    return false;
                }
                opened = true;
            }
            if (!writer.Append(feature, shapeType, cancellationRequested, error))
            {
                return false;
            }
        }
        if (opened)
        {
            if (!writer.Close(error))
            {
                return false;
            }
            layers.push_back({{"file", std::string(name) + ".shp"},
                              {"attributes", std::string(name) + ".attributes.json"},
                              {"featureCount", writer.GetCount()}});
        }
    }
    Json fields = Json::object();
    for (const auto& [name, key, type, width] : kFields)
    {
        fields[name] = key;
    }
    const Json manifest = {{"coordinateSystem", "EPSG:4326"},
                           {"sourceCoordinateSystem", "GCJ-02"},
                           {"encoding", "UTF-8"},
                           {"version", decodedData.version},
                           {"packetCount", decodedData.packetCount},
                           {"layers", layers},
                           {"dbfFields", fields}};
    if (!WriteText(staging.GetPath() / "manifest.json", manifest.dump(2), error) ||
        CheckCancellation(cancellationRequested, error))
    {
        return false;
    }
    for (const auto& file : std::filesystem::directory_iterator(staging.GetPath()))
    {
        const auto target = outputDirectory / file.path().filename();
        std::error_code publishError;
        // 临时文件位于同一目录，重命名避免大规模导出后再复制一遍全部数据。
        std::filesystem::remove(target, publishError);
        if (!publishError)
        {
            std::filesystem::rename(file.path(), target, publishError);
        }
        if (publishError)
        {
            error = "无法发布 SHP 文件：" + publishError.message();
            return false;
        }
    }
    return true;
}
