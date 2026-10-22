#include "Wgs84ShapefileReader.h"

#include <cmath>
#include <memory>
#include <string>
#include <type_traits>

#include "AmapVectorDownloader.h"
#include "AmapVectorPngDownloader.h"
#include "AmapVectorShapefileWriter.h"
#include "AmapBuildingFbxExporter.h"
#include "ShapefileTestSupport.h"

namespace
{
using postprocess_test::Require;
using namespace DownTool;

bool VerifyUnsupportedPrj(const std::filesystem::path& path)
{
    auto prj = path;
    prj.replace_extension(".prj");
    const std::vector<std::string> rejected = {
        R"(PROJCS["WGS 84 / Pseudo-Mercator",GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]],UNIT["metre",1]])",
        R"(GEOGCS["CGCS2000",DATUM["China_2000",SPHEROID["CGCS2000",6378137,298.257222101]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])",
        R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["radian",1]])",
        R"(GEOCCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],UNIT["metre",1]])",
        R"(GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563],TOWGS84[1,0,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433]])",
        std::string(shapefile_test::kWgs84Prj) + "garbage",
        "GEOGCS[\"WGS84\"",
        ""};
    for (const auto& definition : rejected)
    {
        shapefile_test::WriteText(prj, definition);
        ShapefileInfo info;
        std::string error;
        if (!Require(!Wgs84ShapefileReader::Inspect(path, info, error) &&
                         error.find("暂不支持") != std::string::npos,
                     "错误接受未知或不支持的坐标系：" + definition))
        {
            return false;
        }
    }
    std::filesystem::remove(prj);
    ShapefileInfo info;
    std::string error;
    if (!Require(!Wgs84ShapefileReader::Inspect(path, info, error) &&
                     error.find(".prj") != std::string::npos,
                 "缺少 PRJ 时没有报告无法确认坐标系"))
    {
        return false;
    }
    const std::string wkt2 =
        R"wkt(GEOGCRS["WGS 84",ENSEMBLE["World Geodetic System 1984 ensemble",MEMBER["World Geodetic System 1984 (G1762)"],ELLIPSOID["WGS 84",6378137,298.257223563,LENGTHUNIT["metre",1]],ENSEMBLEACCURACY[2]],PRIMEM["Greenwich",0,ANGLEUNIT["degree",0.0174532925199433]],CS[ellipsoidal,2],AXIS["geodetic latitude (Lat)",north,ORDER[1],ANGLEUNIT["degree",0.0174532925199433]],AXIS["geodetic longitude (Lon)",east,ORDER[2],ANGLEUNIT["degree",0.0174532925199433]],ID["EPSG",4326]])wkt";
    shapefile_test::WriteText(prj, wkt2);
    if (!Require(Wgs84ShapefileReader::Inspect(path, info, error), "有效 WKT2 未识别：" + error))
    {
        return false;
    }
    shapefile_test::WriteText(prj, shapefile_test::kWgs84Prj);
    return true;
}

bool RunTest()
{
    const auto root = postprocess_test::MakeTestRoot("shapefile-import");
    if (!postprocess_test::ResetDirectory(root))
    {
        return false;
    }
    const auto path = root / std::filesystem::u8path("中文目录/本地建筑.shp");
    if (!Require(shapefile_test::WriteFixture(path), "创建独立 SHP fixture 失败"))
    {
        return false;
    }
    ShapefileInfo info;
    std::string error;
    if (!Require(Wgs84ShapefileReader::Inspect(path, info, error) && info.GetFeatureCount() == 2 &&
                     info.GetGeometryType() == AmapVectorGeometryType::Polygon &&
                     info.GetNumericFields() == std::vector<std::string>{"height_m"},
                 "SHP 元数据读取失败：" + error) ||
        !VerifyUnsupportedPrj(path))
    {
        return false;
    }
    ShapefileImportOptions source;
    source.SetPath(path);
    AmapVectorDecodedData decoded;
    if (!Require(Wgs84ShapefileReader::Read(source, decoded, error) &&
                     decoded.features.size() == 2 &&
                     decoded.features[0].geometryParts.size() == 2 &&
                     decoded.features[0].GetCoordinateSystem() == VectorCoordinateSystem::Wgs84 &&
                     decoded.features[0].geometryParts[0][0].longitude == 116.42 &&
                     decoded.features[0].names[0].text == "中文建筑" &&
                     decoded.features[0].GetSourceProperties()["owner"] == "业主甲" &&
                     decoded.features[0].GetSourceProperties()["uid"] == "18446744073709551615" &&
                     std::abs(*decoded.features[0].GetHeightMeters() - 12.345) < 1.e-12 &&
                     std::abs(VectorBuildingHeightCentimeters(decoded.features[0], 45.) - 1234.5) <
                         1.e-9,
                 "本地 SHP 的坐标、孔洞、中文属性或米制高度丢失：" + error))
    {
        return false;
    }
    const auto coordinate =
        VectorCoordinateWgs84(decoded.features[0], decoded.features[0].geometryParts[0][0]);
    if (!Require(coordinate.lon == 116.42 && coordinate.lat == 39.89,
                 "本地 WGS84 被错误应用 GCJ-02 偏移") ||
        !Require(!Wgs84ShapefileReader::Read(source, decoded, error, [] { return true; }) &&
                     error.find("取消") != std::string::npos,
                 "导入取消没有及时退出"))
    {
        return false;
    }
    source.SetHeightField("missing");
    if (!Require(!Wgs84ShapefileReader::Read(source, decoded, error) &&
                     error.find("高度字段") != std::string::npos,
                 "不存在的高度字段没有报错"))
    {
        return false;
    }
    source.SetHeightField("-");
    if (!Require(Wgs84ShapefileReader::Read(source, decoded, error) &&
                     !HasVectorBuildingHeight(decoded.features[0]),
                 "关闭高度映射失败"))
    {
        return false;
    }
    source.SetHeightField("height_m");
    AmapVectorDownloadOptions options;
    options.SetIncludeAmapSource(false);
    options.SetShapefiles({source});
    options.centerWgs84 = {116.421, 39.891};
    options.radiusMeters = 300.;
    options.zoom = 17;
    options.webApiKey.clear();
    GisSpatialRange range;
    range.SetIncludedPolygons(
        {{{116.419, 39.889}, {116.423, 39.889}, {116.423, 39.893}, {116.419, 39.893}}});
    options.SetGisSpatialRange(range);
    const auto downloaded = AmapVectorDownloader{}.Download(options);
    if (!Require(downloaded.success && downloaded.downloadedTaskCount == 0 &&
                     downloaded.cachedTaskCount == 0 && downloaded.decodedData.features.size() == 1,
                 "仅本地 SHP 仍依赖高德服务，或下载范围过滤错误：" + downloaded.error))
    {
        return false;
    }
    auto mixed = downloaded.decodedData;
    auto remote = mixed.features.front();
    remote.SetCoordinateSystem(VectorCoordinateSystem::Gcj02);
    for (auto& part : remote.geometryParts)
    {
        for (auto& point : part)
        {
            const auto gcj = GCJ02GoogleDownBase::WGS84ToGCJ02({point.longitude, point.latitude});
            point = {gcj.lon, gcj.lat};
        }
    }
    mixed.features.push_back(std::move(remote));
    if (!Require(ApplyGisSpatialFilter(mixed, range, error) && mixed.features.size() == 2,
                 "混合高德和本地数据的范围过滤发生错位：" + error))
    {
        return false;
    }
    const auto geojson = root / "imported.geojson";
    AmapBuildingFbxExportOptions building;
    building.outputPath = root / "fbx/buildings.fbx";
    building.centerWgs84 = options.centerWgs84;
    building.wallTexturePath = root / "walls";
    building.roofTexturePath = root / "roofs";
    std::filesystem::create_directories(building.wallTexturePath);
    std::filesystem::create_directories(building.roofTexturePath);
    if (!Require(cv::imwrite((building.wallTexturePath / "wall.png").string(),
                             cv::Mat(32, 32, CV_8UC3, cv::Scalar(80, 120, 160))) &&
                     cv::imwrite((building.roofTexturePath / "roof.png").string(),
                                 cv::Mat(32, 32, CV_8UC3, cv::Scalar(130, 150, 100))),
                 "创建本地建筑贴图失败"))
    {
        return false;
    }
    double minX = std::numeric_limits<double>::max();
    double maxZ = 0.;
    const auto model = ExportAmapBuildingScene(
        downloaded.decodedData, building, [&](aiScene& scene, std::string&) {
            for (unsigned mesh = 0; mesh < scene.mNumMeshes; ++mesh)
            {
                for (unsigned vertex = 0; vertex < scene.mMeshes[mesh]->mNumVertices; ++vertex)
                {
                    const auto& position = scene.mMeshes[mesh]->mVertices[vertex];
                    minX = std::min(minX, double(position.x));
                    maxZ = std::max(maxZ, double(position.z));
                }
            }
            return true;
        });
    if (!Require(model.success && model.buildingCount == 1 &&
                     std::abs(minX - (-11131.949079)) < 0.1 && std::abs(maxZ - 1234.5) < 0.01,
                 "实际建筑网格发生坐标偏移或米制高度错误：" + model.error))
    {
        return false;
    }
    if (!Require(AmapVectorGeoJsonWriter::WriteFile(downloaded.decodedData, geojson, error),
                 "导入后 GeoJSON 导出失败：" + error))
    {
        return false;
    }
    std::ifstream input(geojson);
    const auto json = nlohmann::json::parse(input);
    if (!Require(json["coordinateSystem"] == "WGS84" &&
                     json["features"][0]["properties"]["owner"] == "业主甲" &&
                     json["features"][0]["geometry"]["coordinates"][0][0][0] == 116.42,
                 "GeoJSON 没有保留 WGS84 原始坐标和属性"))
    {
        return false;
    }
    const auto exported = root / "exported";
    if (!Require(AmapVectorShapefileWriter::WriteDirectory(downloaded.decodedData, exported, error),
                 "导入后 SHP 导出失败：" + error))
    {
        return false;
    }
    source.SetPath(exported / "polygons.shp");
    source.SetHeightField("height_m");
    if (!Require(
            Wgs84ShapefileReader::Read(source, decoded, error) && decoded.features.size() == 1 &&
                decoded.features[0].geometryParts[0][0].longitude == 116.42 &&
                decoded.features[0].GetSourceProperties()["owner"] == "业主甲" &&
                decoded.features[0].GetSourceProperties()["src_uid"] == "18446744073709551615" &&
                std::abs(*decoded.features[0].GetHeightMeters() - 12.345) < 1.e-9,
            "SHP 往返导出后原始属性、精确 ID、坐标或高度丢失：" + error))
    {
        return false;
    }
    options.SetShapefiles({source});
    AmapVectorPngDownloadOptions png;
    png.vectorDownload = options;
    png.render.pngTileCachePath = root / "png";
    const auto rendered = AmapVectorPngDownloader{}.DownloadPngTiles(png);
    if (!Require(rendered.success && rendered.render.renderedTileCount > 0 &&
                     rendered.vectorDownload.downloadedTaskCount == 0,
                 "本地 SHP 渲染未复用现有瓦片流程：" + rendered.error))
    {
        return false;
    }
    range.SetExcludedPolygons(range.GetIncludedPolygons());
    options.SetGisSpatialRange(range);
    const auto excluded = AmapVectorDownloader{}.Download(options);
    if (!Require(excluded.success && excluded.decodedData.features.empty(),
                 "本地建筑没有遵守 GIS 排除区"))
    {
        return false;
    }
    if (!Require(ApplyGisSpatialFilter(mixed, range, error) && mixed.features.empty(),
                 "混合数据没有遵守 GIS 排除区：" + error))
    {
        return false;
    }
    for (const auto type : {SHPT_POINT, SHPT_MULTIPOINT, SHPT_ARC})
    {
        const auto other = root / (std::to_string(type) + ".shp");
        if (!Require(shapefile_test::WriteFixture(other, type), "创建点线 fixture 失败"))
        {
            return false;
        }
        source.SetPath(other);
        if (!Require(Wgs84ShapefileReader::Read(source, decoded, error),
                     "点、多点或多段线导入失败：" + error) ||
            !Require(AmapVectorGeoJsonWriter::WriteFile(decoded, geojson, error),
                     "多点或多线导出失败：" + error))
        {
            return false;
        }
        std::ifstream geometryInput(geojson);
        const auto geometry = nlohmann::json::parse(geometryInput)["features"][0]["geometry"];
        if (!Require(geometry["type"] == (type == SHPT_POINT        ? "Point"
                                          : type == SHPT_MULTIPOINT ? "MultiPoint"
                                                                    : "MultiLineString"),
                     "GeoJSON 丢失多点或多线的后续部分"))
        {
            return false;
        }
    }
    auto multipart = downloaded.decodedData;
    auto island = multipart.features[0].geometryParts.front();
    for (auto& coordinate : island)
    {
        coordinate.longitude += 0.004;
    }
    multipart.features[0].geometryParts.push_back(std::move(island));
    if (!Require(AmapVectorGeoJsonWriter::WriteFile(multipart, geojson, error),
                 "多外环面导出失败：" + error))
    {
        return false;
    }
    std::ifstream multipartInput(geojson);
    const auto polygon = nlohmann::json::parse(multipartInput)["features"][0]["geometry"];
    if (!Require(polygon["type"] == "MultiPolygon" && polygon["coordinates"].size() == 2 &&
                     polygon["coordinates"][0].size() == 2 && polygon["coordinates"][1].size() == 1,
                 "多外环面被误写成孔洞或丢失真实孔洞"))
    {
        return false;
    }
    auto dbf = path;
    dbf.replace_extension(".dbf");
    auto savedDbf = path;
    savedDbf.replace_extension(".dbf.saved");
    std::filesystem::rename(dbf, savedDbf);
    const bool missingRejected = !Wgs84ShapefileReader::Inspect(path, info, error);
    std::filesystem::rename(savedDbf, dbf);
    if (!Require(missingRejected && error.find("配套文件") != std::string::npos,
                 "缺少 DBF 未明确报错"))
    {
        return false;
    }
    return true;
}
} // namespace

int main()
{
    if (!RunTest())
    {
        return 1;
    }
    std::cout << "Wgs84ShapefileReaderTest passed\n";
    return 0;
}
