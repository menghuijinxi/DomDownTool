#include "PostProcessTestSupport.h"

#include <array>
#include <memory>
#include <type_traits>

#include "nlohmann/json.hpp"
#include "shapefil.h"

#include "AmapVectorShapefileWriter.h"

namespace
{
using Json = nlohmann::json;
using postprocess_test::Require;
using ShapeHandle = std::unique_ptr<std::remove_pointer_t<SHPHandle>, decltype(&SHPClose)>;
using AttributeHandle = std::unique_ptr<std::remove_pointer_t<DBFHandle>, decltype(&DBFClose)>;
using ShapeObject = std::unique_ptr<SHPObject, decltype(&SHPDestroyObject)>;

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

double SignedArea(const SHPObject& object, int part)
{
    const int start = object.panPartStart[part];
    const int end = part + 1 < object.nParts ? object.panPartStart[part + 1] : object.nVertices;
    double area = 0.;
    for (int index = start; index + 1 < end; ++index)
    {
        const double x = object.padfX[index] - object.padfX[start];
        const double y = object.padfY[index] - object.padfY[start];
        const double nextX = object.padfX[index + 1] - object.padfX[start];
        const double nextY = object.padfY[index + 1] - object.padfY[start];
        area += x * nextY - nextX * y;
    }
    return area * 0.5;
}

bool RunTest()
{
    const auto root = postprocess_test::MakeTestRoot("shapefile");
    if (!postprocess_test::ResetDirectory(root))
    {
        return false;
    }
    const auto output = root / std::filesystem::u8path("中文目录/导出SHP");
    DownTool::AmapVectorFeature point;
    point.geometryType = DownTool::AmapVectorGeometryType::Point;
    point.layerName = "poi";
    point.mainKey = std::numeric_limits<uint32_t>::max();
    point.uid = std::numeric_limits<uint64_t>::max();
    std::string longName;
    for (int count = 0; count < 100; ++count)
    {
        longName += "中文名称";
    }
    point.names = {{longName, "zh-CN"}, {"Second name", "en"}};
    point.geometryParts = {{{116.42, 39.89}}, {{116.421, 39.891}}};
    DownTool::AmapVectorFeature line = point;
    line.geometryType = DownTool::AmapVectorGeometryType::LineString;
    line.layerName = "road";
    line.shield = "京A1";
    line.shieldType = 12;
    line.geometryParts = {{{116.42, 39.89}, {116.421, 39.891}},
                          {{116.422, 39.892}, {116.423, 39.893}}};
    DownTool::AmapVectorFeature polygon = point;
    polygon.geometryType = DownTool::AmapVectorGeometryType::Polygon;
    polygon.layerName = "building";
    polygon.hasHeight = true;
    polygon.height = std::numeric_limits<uint64_t>::max();
    polygon.buildingIds = "building-中文-001";
    polygon.internalType = "test-building";
    polygon.geometryParts = {
        {{116.42, 39.89}, {116.424, 39.89}, {116.424, 39.894}, {116.42, 39.894}, {116.42, 39.89}},
        {{116.421, 39.891},
         {116.422, 39.891},
         {116.422, 39.892},
         {116.421, 39.892},
         {116.421, 39.891}}};
    DownTool::AmapVectorDecodedData decoded;
    decoded.version = "shapefile-test";
    decoded.packetCount = 2;
    decoded.features = {point, line, polygon};
    std::string error;
    if (!Require(DownTool::AmapVectorShapefileWriter::WriteDirectory(decoded, output, error),
                 "SHP 写出失败：" + error))
    {
        return false;
    }
    SAHooks hooks{};
#ifdef SHPAPI_UTF8_HOOKS
    SASetupUtf8Hooks(&hooks);
#else
    SASetupDefaultHooks(&hooks);
#endif
    const std::array<std::pair<const char*, int>, 3> layers = {
        {{"points", SHPT_MULTIPOINT}, {"lines", SHPT_ARC}, {"polygons", SHPT_POLYGON}}};
    for (const auto& [name, expectedType] : layers)
    {
        const auto base = output / name;
        const auto path = postprocess_test::PathToUtf8String(base);
        ShapeHandle shapes(SHPOpenLL(path.c_str(), "rb", &hooks), &SHPClose);
        AttributeHandle attributes(DBFOpenLL(path.c_str(), "rb", &hooks), &DBFClose);
        if (!Require(shapes && attributes, "中文路径下的 SHP/DBF 无法重新读取"))
        {
            return false;
        }
        int count = 0;
        int type = 0;
        SHPGetInfo(shapes.get(), &count, &type, nullptr, nullptr);
        if (!Require(count == 1 && type == expectedType &&
                         DBFGetRecordCount(attributes.get()) == count &&
                         DBFGetFieldCount(attributes.get()) == 18,
                     "SHP 几何类型、记录数或属性记录不匹配"))
        {
            return false;
        }
        ShapeObject object(SHPReadObject(shapes.get(), 0), &SHPDestroyObject);
        const auto expected = DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84({116.42, 39.89});
        if (!Require(object && std::abs(object->padfX[0] - expected.lon) < 1.e-12 &&
                         std::abs(object->padfY[0] - expected.lat) < 1.e-12,
                     "SHP 坐标没有正确转换为 WGS84"))
        {
            return false;
        }
        if (type == SHPT_MULTIPOINT && !Require(object->nVertices == 2, "SHP 多点要素丢失坐标"))
        {
            return false;
        }
        if (type == SHPT_ARC &&
            !Require(object->nParts == 2 && object->nVertices == 4, "SHP 多段线丢失分段"))
        {
            return false;
        }
        if (type == SHPT_POLYGON &&
            !Require(object->nParts == 2 && SignedArea(*object, 0) < 0. &&
                         SignedArea(*object, 1) > 0. &&
                         std::string(DBFReadStringAttribute(attributes.get(), 0, 15)) ==
                             std::to_string(polygon.height),
                     "SHP 面孔洞方向或原始高度数据错误"))
        {
            return false;
        }
        const std::string uid = DBFReadStringAttribute(attributes.get(), 0, 8);
        const std::string exportedName = DBFReadStringAttribute(attributes.get(), 0, 9);
        if (!Require(uid == std::to_string(point.uid) &&
                         DBFReadDoubleAttribute(attributes.get(), 0, 1) == point.mainKey &&
                         exportedName.size() == 252 && longName.starts_with(exportedName) &&
                         !Json::parse("\"" + exportedName + "\"", nullptr, false).is_discarded() &&
                         std::string(DBFReadStringAttribute(attributes.get(), 0, 10)) == "zh-CN",
                     "SHP 的 DBF 中文属性截短或大整数 ID 错误") ||
            !Require(!std::filesystem::exists(output / (std::string(name) + ".attributes.json")),
                     "SHP 仍生成了非标准属性 JSON 文件") ||
            !Require(ReadText(output / (std::string(name) + ".cpg")).starts_with("UTF-8") &&
                         ReadText(output / (std::string(name) + ".prj")).find("WGS_1984") !=
                             std::string::npos,
                     "SHP 缺少编码或坐标系配套文件"))
        {
            return false;
        }
    }
    const auto manifest = Json::parse(ReadText(output / "manifest.json"));
    if (!Require(manifest["coordinateSystem"] == "EPSG:4326" && manifest["layers"].size() == 3 &&
                     manifest["dbfFields"]["buildingId"] == "buildingIds",
                 "SHP 输出清单或 DBF 字段映射错误"))
    {
        return false;
    }
    for (const auto& layer : manifest["layers"])
    {
        if (!Require(!layer.contains("attributes"), "SHP 清单仍引用属性 JSON 文件"))
        {
            return false;
        }
    }
    const auto emptyOutput = root / "empty";
    if (!Require(DownTool::AmapVectorShapefileWriter::WriteDirectory({}, emptyOutput, error) &&
                     Json::parse(ReadText(emptyOutput / "manifest.json"))["layers"].empty(),
                 "空范围没有生成合法的 SHP 清单"))
    {
        return false;
    }
    const auto canceled = root / "canceled";
    size_t checks = 0;
    if (!Require(!DownTool::AmapVectorShapefileWriter::WriteDirectory(
                     decoded, canceled, error, [&checks] { return ++checks > 12; }) &&
                     error == "SHP 导出已取消。" && std::filesystem::is_empty(canceled),
                 "取消 SHP 导出后遗留了部分输出"))
    {
        return false;
    }
    decoded.features.front().geometryParts = {{{std::numeric_limits<double>::quiet_NaN(), 0.}}};
    const auto invalid = root / "invalid";
    if (!Require(!DownTool::AmapVectorShapefileWriter::WriteDirectory(decoded, invalid, error) &&
                     !error.empty() && std::filesystem::is_empty(invalid),
                 "无效几何未被拒绝或遗留了部分输出"))
    {
        return false;
    }
    std::ofstream(root / "blocked") << "blocked";
    return Require(
        !DownTool::AmapVectorShapefileWriter::WriteDirectory({}, root / "blocked", error) &&
            !error.empty(),
        "不可写 SHP 路径没有报告错误");
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
