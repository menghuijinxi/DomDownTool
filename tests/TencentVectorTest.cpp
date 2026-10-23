#include "PostProcessTestSupport.h"

#include "openssl/evp.h"

#include "AmapVectorPngDownloader.h"
#include "TencentVectorDecoder.h"
#include "TencentVectorDownloader.h"

namespace
{
    using namespace DownTool;
    using Json = nlohmann::json;
    using postprocess_test::Require;

    std::string Envelope(const Json& payload)
    {
        std::string bytes = payload.dump();
        if (bytes.size() % 2 != 0)
        {
            bytes.push_back(' ');
        }
        const auto rotate = [](uint8_t value, unsigned bits) {
            return static_cast<uint8_t>((value << bits) | (value >> (8 - bits)));
        };
        for (size_t index = 0; index < bytes.size(); index += 2)
        {
            uint8_t left = static_cast<uint8_t>(bytes[index]);
            uint8_t right = static_cast<uint8_t>(bytes[index + 1]);
            for (unsigned round = 0; round < 3; ++round)
            {
                const uint8_t previous =
                    right ^ (rotate(left, 2) ^ (rotate(left, 1) & rotate(left, 3)));
                right = left;
                left = previous;
            }
            bytes[index] = static_cast<char>(left);
            bytes[index + 1] = static_cast<char>(right);
        }
        std::string encoded((bytes.size() + 2) / 3 * 4, '\0');
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                        reinterpret_cast<const unsigned char*>(bytes.data()),
                        static_cast<int>(bytes.size()));
        return Json{{"data", encoded}}.dump();
    }

    bool RunTest()
    {
        const AmapVectorTile tile{215849, 99348, 18};
        const Json payload = {
            {"ret", 0},
            {"features", Json::array({Json::array({196619, {100, 100, 20, 0, 0, 20, -20, 0}, 12.5}),
                                      Json::array({131084, {-10, 100, 300, 0}, 999}),
                                      Json::array({196621, {40, 40, 10, 0, 0, 10}, 100}),
                                      Json::array({196619, {80, 80, 10, 0}, 4}),
                                      Json::array({Json::array({262145, {120, 120}, "地点"})})})},
            {"pois", Json::array({Json::array({65537, {90, 90}, "POI"})})}};
        AmapVectorDecodedData decoded;
        std::string error;
        if (!Require(TencentVectorDecoder::DecodeEnvelope(Envelope(payload), tile, decoded, error),
                     error) ||
            !Require(decoded.features.size() == 6, "成组文字或补充 POI 丢失") ||
            !Require(decoded.features[0].GetHeightMeters() == 12.5 &&
                IsAmapVectorBuildingWithHeight(decoded.features[0]) &&
                VectorBuildingHeightCentimeters(decoded.features[0], 300.) == 1250.,
                     "腾讯米高度未正确传递") ||
            !Require(!decoded.features[1].GetHeightMeters() &&
                         ClassifyAmapVectorFeature(decoded.features[1]) ==
                             AmapVectorRenderCategory::Road,
                     "道路字段被误当高度或分类错误") ||
            !Require(ClassifyAmapVectorFeature(decoded.features[2]) ==
                         AmapVectorRenderCategory::Other,
                     "居住区被误当建筑") ||
            !Require(decoded.features[3].geometryParts.empty() &&
                         decoded.features[3].GetSourceProperties()["geometryIssue"].is_string(),
                     "退化面没有保留诊断属性") ||
            !Require(decoded.features[0].GetCoordinateSystem() == VectorCoordinateSystem::Wgs84 &&
                         decoded.features[0].geometryParts[0].size() == 5,
                     "WGS84 或闭环转换错误") ||
            !Require(decoded.features[1].GetSourceProperties()["localCoordinates"][0][1][0] == 290,
                     "差分或瓦片外缓冲坐标被截断"))
        {
            return false;
        }
        Json absolute = payload;
        absolute["features"] =
            Json::array({Json::array({196619, {100, 100, 120, 100, 120, 120, 100, 120}, 12.5})});
        absolute.erase("pois");
        AmapVectorDecodedData uncompressed;
        if (!Require(TencentVectorDecoder::DecodeEnvelope(absolute.dump(), tile, uncompressed,
                                                          error, false),
                     error))
        {
            return false;
        }
        const auto& first = decoded.features[0].geometryParts[0];
        const auto& second = uncompressed.features[0].geometryParts[0];
        for (size_t index = 0; index < first.size(); ++index)
        {
            if (!Require(first[index].longitude == second[index].longitude &&
                             first[index].latitude == second[index].latitude,
                         "压缩与绝对坐标不一致"))
            {
                return false;
            }
        }
        for (const std::string invalid :
             {"{}", "{\"data\":\"!!!!\"}", "{\"ret\":1,\"features\":[]}",
              "{\"ret\":0,\"features\":[[999999,[0,0]]]}",
              "{\"ret\":0,\"features\":[[196619,[0,0,1]]]}",
              "{\"ret\":0,\"features\":[[196619,[[0,0,1,0,0,1],[0,0,2,0,0,2]]]]}",
              "{\"ret\":0,\"features\":[],\"pois\":{}}"})
        {
            if (!Require(
                    !TencentVectorDecoder::DecodeEnvelope(invalid, tile, uncompressed, error) &&
                        !error.empty(),
                    "错误响应没有报错"))
            {
                return false;
            }
        }
        if (!Require(TencentVectorDownloader::TileUrl(tile).find("y=162795") != std::string::npos,
                     "XYZ 转 TMS Y 错误"))
        {
            return false;
        }
        const auto root = postprocess_test::MakeTestRoot("tencent-vector");
        std::filesystem::create_directories(root);
        if (!Require(AmapVectorGeoJsonWriter::WriteFile(decoded, root / "decoder.geojson", error),
                     error))
        {
            return false;
        }
        std::ifstream jsonFile(root / "decoder.geojson");
        const auto exported = Json::parse(jsonFile);
        if (!Require(exported["features"][3]["geometry"].is_null(), "退化面写出伪造几何"))
        {
            return false;
        }
        AmapVectorDecodedData emptyTencent;
        emptyTencent.version = "Tencent 405";
        if (!Require(AmapVectorGeoJsonWriter::WriteFile(
                emptyTencent, root / "empty.geojson", error), error))
        {
            return false;
        }
        std::ifstream emptyFile(root / "empty.geojson");
        if (!Require(Json::parse(emptyFile)["coordinateSystem"] == "WGS84",
                     "腾讯空结果错误标记为 GCJ-02"))
        {
            return false;
        }
        AmapVectorDownloadOptions options;
        options.SetIncludeAmapSource(false);
        options.SetIncludeTencentSource(true);
        options.centerWgs84 = {first[0].longitude, first[0].latitude};
        options.radiusMeters = 30.;
        options.SetTencentCachePath(root / "cache");
        const auto tiles = TencentVectorDownloader::SelectTiles(options);
        const Json empty = {{"ret", 0}, {"features", Json::array()}};
        for (const auto& selected : tiles)
        {
            const auto cache = options.GetTencentCachePath() / "v405-style100-compress1" /
                               std::to_string(selected.zoom) / std::to_string(selected.x) /
                               (std::to_string(selected.y) + ".json");
            std::filesystem::create_directories(cache.parent_path());
            std::ofstream output(cache, std::ios::binary);
            output << (selected.x == tile.x && selected.y == tile.y ? Envelope(absolute)
                                                                    : empty.dump());
        }
        const auto downloaded = AmapVectorDownloader().Download(options);
        if (!Require(downloaded.success, downloaded.error) ||
            !Require(downloaded.cachedTaskCount == tiles.size() &&
                         downloaded.downloadedTaskCount == 0 && downloaded.featureCount == 1,
                     "腾讯独立缓存或统一下载调度错误"))
        {
            return false;
        }
        options.cancellationRequested = [] { return true; };
        if (!Require(!AmapVectorDownloader().Download(options).success, "腾讯取消未生效"))
        {
            return false;
        }
        options.cancellationRequested = {};
        options.progressCallback = [](const auto&) { throw std::runtime_error("callback"); };
        if (!Require(!AmapVectorDownloader().Download(options).success,
                     "进度回调异常没有转换为失败结果"))
        {
            return false;
        }
        options.progressCallback = {};
        options.SetTencentDataZoom(19);
        return Require(!AmapVectorDownloader().Download(options).success, "非法数据层级未拒绝");
    }

    bool ComparePrototype(const std::filesystem::path& root)
    {
        size_t tiles = 0;
        size_t features = 0;
        size_t comparisons = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "1"))
        {
            if (entry.path().extension() != ".bin")
            {
                continue;
            }
            const auto xDirectory = entry.path().parent_path();
            AmapVectorTile tile{
                static_cast<uint32_t>(std::stoul(xDirectory.filename().string())),
                static_cast<uint32_t>(std::stoul(entry.path().stem().string())),
                static_cast<uint32_t>(std::stoul(xDirectory.parent_path().filename().string()))};
            std::ifstream input(entry.path(), std::ios::binary);
            const std::string envelope{std::istreambuf_iterator<char>(input), {}};
            AmapVectorDecodedData decoded;
            std::string error;
            if (!Require(TencentVectorDecoder::DecodeEnvelope(envelope, tile, decoded, error),
                         error))
            {
                return false;
            }
            ++tiles;
            features += decoded.features.size();
            const auto absolutePath = root / "0" / std::to_string(tile.zoom) /
                                      std::to_string(tile.x) / entry.path().filename();
            if (std::filesystem::exists(absolutePath))
            {
                std::ifstream absoluteFile(absolutePath, std::ios::binary);
                const std::string absoluteEnvelope{std::istreambuf_iterator<char>(absoluteFile),
                                                   {}};
                AmapVectorDecodedData absolute;
                if (!Require(TencentVectorDecoder::DecodeEnvelope(absoluteEnvelope, tile, absolute,
                                                                  error, false),
                             error) ||
                    !Require(absolute.features.size() == decoded.features.size(),
                             "真实压缩响应与绝对坐标要素数量不同"))
                {
                    return false;
                }
                for (size_t index = 0; index < decoded.features.size(); ++index)
                {
                    const auto& compressed = decoded.features[index];
                    const auto& plain = absolute.features[index];
                    if (!Require(compressed.GetSourceProperties() == plain.GetSourceProperties() &&
                                     compressed.GetHeightMeters() == plain.GetHeightMeters(),
                                 "真实压缩响应与绝对坐标属性、局部路径或高度不同"))
                    {
                        return false;
                    }
                }
                ++comparisons;
            }
        }
        std::cout << "prototype tiles=" << tiles << " features=" << features
                  << " comparisons=" << comparisons << '\n';
        return Require(tiles >= 16 && features > 6000 && comparisons >= 16, "真实前端样本不足");
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
