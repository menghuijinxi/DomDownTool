#include "PostProcessTestSupport.h"

#include "AmapVectorPngDownloader.h"
#include "BaiduDown.h"
#include "BaiduVectorClassification.h"
#include "BaiduVectorDecoder.h"
#include "BaiduVectorTestSupport.h"

namespace
{
using namespace DownTool;
using Json = nlohmann::json;
using postprocess_test::Require;

double RingArea(const std::vector<AmapVectorCoordinate>& ring)
{
    double area = 0.;
    const auto local = [&](const AmapVectorCoordinate& point) {
        return std::pair{(point.longitude - ring.front().longitude) * 1.e6,
                         (point.latitude - ring.front().latitude) * 1.e6};
    };
    for (size_t index = 1; index < ring.size(); ++index)
    {
        const auto first = local(ring[index - 1]);
        const auto second = local(ring[index]);
        area += first.first * second.second - second.first * first.second;
    }
    return area * 0.5;
}

BaiduDown::MercatorPoint RecoverSourceMercator(const AmapVectorCoordinate& point)
{
    auto bd09 = BaiduDown::WGS84ToBD09({point.longitude, point.latitude});
    // 公共投影的正反变换不是精确互逆；对照源网格时先迭代消除往返误差。
    for (unsigned iteration = 0; iteration < 8; ++iteration)
    {
        const auto converted = BaiduDown::BD09ToWGS84(bd09);
        bd09.lon += point.longitude - converted.lon;
        bd09.lat += point.latitude - converted.lat;
    }
    return BaiduDown::BD09ToMercator(bd09);
}

bool RunTest()
{
    const BaiduVectorTile tile(25313, 9417, 19);
    BaiduVectorDecodeOptions decodeOptions(tile);
    AmapVectorDecodedData decoded;
    std::string error;
    if (!Require(BaiduVectorDecoder::DecodeTile(baidu_vector_test::Fixture(), decodeOptions,
                                                decoded, error),
                 error) ||
        !Require(decoded.features.size() == 3, "百度分组、样式或地点丢失"))
    {
        return false;
    }
    const auto& road = decoded.features[0];
    double area = 0.;
    for (const auto& ring : road.geometryParts)
    {
        area += RingArea(ring);
    }
    const auto squareArea = [&](double minimum, double maximum) {
        std::vector<AmapVectorCoordinate> ring;
        for (const auto& [x, y] : {std::pair{minimum, minimum},
                                   {maximum, minimum},
                                   {maximum, maximum},
                                   {minimum, maximum},
                                   {minimum, minimum}})
        {
            const auto point = BaiduVectorTileGrid::LocalToWgs84(tile, x, y);
            ring.push_back({point.lon, point.lat});
        }
        return std::abs(RingArea(ring));
    };
    const double expectedArea = squareArea(0., 100.) - squareArea(25., 75.);
    if (!Require(road.geometryParts.size() == 2 && std::abs(std::abs(area) - expectedArea) < 1.e-6,
                 fmt::format("三角网格外环或孔洞恢复错误：rings={} area={}",
                             road.geometryParts.size(), area)) ||
        !Require(road.GetCoordinateSystem() == VectorCoordinateSystem::Wgs84 &&
                     road.GetDataSource() == VectorDataSource::Baidu,
                 "百度坐标或来源标记错误") ||
        !Require(!HasVectorBuildingHeight(decoded.features[1]) &&
                     decoded.features[1].GetSourceProperties()["heightRaw"] == 12,
                 "未经确认的百度原始高度被当作米或楼层") ||
        !Require(decoded.features[2].names[0].text == "测试地点", "地点 UTF-8 丢失"))
    {
        return false;
    }
    BaiduFeatureTypes types;
    if (!Require(BaiduVectorClassification::DecodeResponse(
                     "baiduFeatureTypes([\"EDUCATION\",\"BUILDING\",\"\"]);", {42, 55, 504812},
                     types, error),
                 error) ||
        !Require(BaiduVectorClassification::Apply(decoded, types, error), error) ||
        !Require(ClassifyAmapVectorFeature(decoded.features[0]) == AmapVectorRenderCategory::Road &&
                     ClassifyAmapVectorFeature(decoded.features[1]) ==
                         AmapVectorRenderCategory::Building &&
                     ClassifyAmapVectorFeature(decoded.features[2]) ==
                         AmapVectorRenderCategory::Point,
                 "百度业务分类、内部道路兜底或地点分类错误") ||
        !Require(decoded.features[0].GetSourceProperties()["classificationSource"] ==
                     "verified-road-surface",
                 "内部道路分类来源丢失"))
    {
        return false;
    }
    auto future = decoded;
    auto properties = future.features[0].GetSourceProperties();
    properties["updateDate"] = "20990101";
    future.features[0].SetSourceProperties(properties);
    if (!Require(BaiduVectorClassification::Apply(future, types, error) &&
                     ClassifyAmapVectorFeature(future.features[0]) ==
                         AmapVectorRenderCategory::Other,
                 "内部道路规则未按版本隔离") ||
        !Require(!BaiduVectorClassification::Apply(future, {}, error),
                 "缺失分类结果被当作未分类继续执行"))
    {
        return false;
    }
    for (const std::string response : {"{}", "baiduFeatureTypes([]);",
                                       "baiduFeatureTypes([\"bad\"]);", "baiduFeatureTypes([42]);"})
    {
        if (!Require(!BaiduVectorClassification::DecodeResponse(response, {42}, types, error) &&
                         !error.empty(),
                     "无效分类响应没有报错"))
        {
            return false;
        }
    }
    const auto fixture = baidu_vector_test::Fixture();
    for (const auto& bytes : {std::string("{}"), fixture.substr(0, fixture.size() - 8),
                              baidu_vector_test::Fixture(false, true)})
    {
        AmapVectorDecodedData preserved = decoded;
        if (!Require(!BaiduVectorDecoder::DecodeTile(bytes, decodeOptions, preserved, error) &&
                         preserved.features.size() == decoded.features.size() && !error.empty(),
                     "无效二进制没有明确失败或污染已有数据"))
        {
            return false;
        }
    }
    decodeOptions.SetCancellation([] { return true; });
    if (!Require(!BaiduVectorDecoder::DecodeTile(fixture, decodeOptions, future, error),
                 "百度解码取消未生效"))
    {
        return false;
    }
    const auto root = postprocess_test::MakeTestRoot("baidu-vector");
    std::filesystem::create_directories(root);
    if (!Require(AmapVectorGeoJsonWriter::WriteFile(decoded, root / "decoder.geojson", error),
                 error))
    {
        return false;
    }
    std::ifstream input(root / "decoder.geojson");
    const auto exported = Json::parse(input);
    if (!Require(exported["coordinateSystem"] == "WGS84" &&
                     exported["features"][0]["geometry"]["coordinates"].size() == 2 &&
                     exported["features"][0]["properties"]["provider"] == "baidu" &&
                     exported["features"][1]["properties"]["heightRaw"] == 12,
                 "百度 GeoJSON 孔洞、来源或原始高度丢失"))
    {
        return false;
    }
    AmapVectorDownloadOptions options;
    options.SetIncludeAmapSource(false);
    options.SetIncludeBaiduSource(true);
    options.SetBaiduCachePath(root / "cache");
    options.centerWgs84 = BaiduVectorTileGrid::LocalToWgs84(tile, 50, 50);
    options.radiusMeters = 150.;
    baidu_vector_test::SeedCache(options, tile);
    const auto downloaded = AmapVectorDownloader().Download(options);
    if (!Require(downloaded.success, downloaded.error) ||
        !Require(downloaded.featureCount == 3 && downloaded.downloadedTaskCount == 0 &&
                     downloaded.cachedTaskCount ==
                         BaiduVectorDownloader::SelectTiles(options).size(),
                 "百度独立缓存、分类缓存或统一调度错误"))
    {
        return false;
    }
    options.cancellationRequested = [] { return true; };
    if (!Require(!AmapVectorDownloader().Download(options).success, "百度下载取消未生效"))
    {
        return false;
    }
    options.cancellationRequested = {};
    options.progressCallback = [](const auto&) { throw std::runtime_error("callback"); };
    if (!Require(!AmapVectorDownloader().Download(options).success, "百度回调异常没有转换为失败"))
    {
        return false;
    }
    options.progressCallback = {};
    options.SetBaiduDataZoom(18);
    if (!Require(!AmapVectorDownloader().Download(options).success, "百度不支持的原生层级未拒绝") ||
        !Require(BaiduVectorTileGrid::ResolveDataZoom(18) == 19 &&
                     BaiduVectorTileGrid::ResolveDataZoom(13) == 12,
                 "百度显示层级与数据层级混淆"))
    {
        return false;
    }
    for (const uint32_t zoom : {3, 5, 7, 9, 10, 12, 15, 17, 19})
    {
        const auto native = BaiduVectorTileGrid::LonLatToTile({116.425, 39.886}, zoom);
        if (!Require(native.GetZoom() == zoom &&
                         BaiduVectorTileGrid::ResolveDataZoom(
                             BaiduVectorTileGrid::StartZoom(zoom)) == zoom &&
                         BaiduVectorTileGrid::ResolveDataZoom(BaiduVectorTileGrid::EndZoom(zoom)) ==
                             zoom,
                     "百度原生格网与显示层级范围不一致"))
        {
            return false;
        }
    }
    return true;
}

bool ComparePrototype(const std::filesystem::path& root)
{
    size_t tiles = 0;
    size_t features = 0;
    size_t vertices = 0;
    Json summaries = Json::array();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (entry.path().extension() != ".bin")
        {
            continue;
        }
        const auto xDirectory = entry.path().parent_path();
        const BaiduVectorTile tile(
            std::stoi(xDirectory.filename().string()), std::stoi(entry.path().stem().string()),
            static_cast<uint32_t>(std::stoul(xDirectory.parent_path().filename().string())));
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        AmapVectorDecodedData decoded;
        std::string error;
        if (!Require(BaiduVectorDecoder::DecodeTile(bytes, BaiduVectorDecodeOptions(tile), decoded,
                                                    error),
                     entry.path().string() + ": " + error))
        {
            return false;
        }
        Json items = Json::array();
        for (const auto& feature : decoded.features)
        {
            const auto& properties = feature.GetSourceProperties();
            vertices += properties.value("sourceVertexCount", size_t{0});
            Json geometry = Json::array();
            for (const auto& part : feature.geometryParts)
            {
                Json points = Json::array();
                for (const auto& point : part)
                {
                    const auto mercator = RecoverSourceMercator(point);
                    points.push_back({mercator.x, mercator.y});
                }
                geometry.push_back(points);
            }
            items.push_back(
                {{"styleId", feature.mainKey},
                 {"groupId", feature.subKey},
                 {"sourceVertexCount", properties.value("sourceVertexCount", size_t{0})},
                 {"empty", feature.geometryParts.empty()},
                 {"geometryMercator", geometry}});
        }
        summaries.push_back(
            {{"x", tile.GetX()}, {"y", tile.GetY()}, {"z", tile.GetZoom()}, {"features", items}});
        ++tiles;
        features += decoded.features.size();
    }
    const auto output = postprocess_test::MakeTestRoot("baidu-vector") / "prototype-summary.json";
    baidu_vector_test::Write(output, summaries.dump());
    std::cout << "prototype tiles=" << tiles << " features=" << features << " vertices=" << vertices
              << '\n';
    return Require(tiles >= 16 && features >= 36000, "真实前端样本不足");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2)
    {
        try
        {
            return ComparePrototype(std::filesystem::u8path(argv[1])) ? 0 : 1;
        }
        catch (const std::exception& exception)
        {
            std::cerr << exception.what() << '\n';
            return 1;
        }
    }
    return postprocess_test::RunGuarded(RunTest);
}
