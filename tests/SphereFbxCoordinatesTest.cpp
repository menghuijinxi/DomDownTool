#include "PostProcessTestSupport.h"

#include <cmath>
#include <map>
#include <numbers>

#include "AmapBuildingFbxExporter.h"
#include "GeographicLib/LocalCartesian.hpp"
#include "nlohmann/json.hpp"

namespace
{
    bool checkMeshTopology(const aiMesh& mesh)
    {
        using Vertex = std::array<float, 5>;
        std::map<std::pair<Vertex, Vertex>, unsigned> edges;
        float minU = 1.;
        float maxU = 0.;
        float minV = 1.;
        float maxV = 0.;
        const auto vertexAt = [&](unsigned index) {
            const auto& p = mesh.mVertices[index];
            const auto& uv = mesh.mTextureCoords[0][index];
            return Vertex{p.x, p.y, p.z, uv.x, uv.y};
        };
        for (unsigned index = 0; index < mesh.mNumVertices; ++index)
        {
            const auto& uv = mesh.mTextureCoords[0][index];
            minU = std::min(minU, uv.x);
            maxU = std::max(maxU, uv.x);
            minV = std::min(minV, uv.y);
            maxV = std::max(maxV, uv.y);
        }
        for (unsigned index = 0; index < mesh.mNumFaces; ++index)
        {
            const auto& face = mesh.mFaces[index];
            const auto a = vertexAt(face.mIndices[0]);
            const auto b = vertexAt(face.mIndices[1]);
            const auto c = vertexAt(face.mIndices[2]);
            if (!postprocess_test::Require((double(b[0]) - a[0]) * (double(c[1]) - a[1]) -
                                                   (double(b[1]) - a[1]) * (double(c[0]) - a[0]) >
                                               0.,
                                           "球面网格出现退化或反向三角形"))
            {
                return false;
            }
            ++edges[std::minmax(a, b)];
            ++edges[std::minmax(b, c)];
            ++edges[std::minmax(c, a)];
        }
        for (const auto& [edge, count] : edges)
        {
            const auto& [a, b] = edge;
            const bool boundary = (a[3] == b[3] && (a[3] == minU || a[3] == maxU)) ||
                                  (a[4] == b[4] && (a[4] == minV || a[4] == maxV));
            if (!postprocess_test::Require(count == 2 || (count == 1 && boundary),
                                           "球面网格出现内部裂缝或非流形边"))
            {
                return false;
            }
        }
        return true;
    }

    bool checkBuilding(const std::filesystem::path& root)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::Polygon;
        feature.layerName = "building";
        feature.SetCoordinateSystem(DownTool::VectorCoordinateSystem::Wgs84);
        feature.SetHeightMeters(1000.);
        feature.geometryParts = {
            {{117.2, 40.}, {117.201, 40.}, {117.201, 40.001}, {117.2, 40.001}, {117.2, 40.}}};
        DownTool::AmapVectorDecodedData decoded;
        decoded.features.push_back(feature);
        DownTool::AmapBuildingFbxExportOptions options;
        options.centerWgs84 = {116.415799, 39.888175};
        options.wallTexturePath =
            std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / std::filesystem::u8path("贴图/墙体");
        options.roofTexturePath =
            std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / std::filesystem::u8path("贴图/房顶");
        options.terrainHeightEnabled = true;
        options.elevationSampler = [](const DownTool::LonLat&) {
            return std::optional<double>(120.);
        };
        std::vector<aiVector3D> planar;
        for (const auto shape : {DownTool::FbxPlanetShape::Plane, DownTool::FbxPlanetShape::Sphere})
        {
            options.shape = shape;
            options.outputPath = root / "buildings.fbx";
            const auto result = DownTool::ExportAmapBuildingScene(
                decoded, options, [&](aiScene& scene, std::string&) {
                    std::vector<aiVector3D> vertices;
                    for (unsigned index = 0; index < scene.mNumMeshes; ++index)
                    {
                        const auto& mesh = *scene.mMeshes[index];
                        vertices.insert(vertices.end(), mesh.mVertices,
                                        mesh.mVertices + mesh.mNumVertices);
                    }
                    if (shape == DownTool::FbxPlanetShape::Plane)
                    {
                        planar = std::move(vertices);
                        return !planar.empty();
                    }
                    if (vertices.size() != planar.size())
                    {
                        return false;
                    }
                    constexpr double radius = 6378137.;
                    const double originX =
                        options.centerWgs84.lon * std::numbers::pi / 180. * radius;
                    const double originY =
                        radius *
                        std::log(std::tan(std::numbers::pi * .25 +
                                          options.centerWgs84.lat * std::numbers::pi / 360.));
                    const GeographicLib::LocalCartesian reference(options.centerWgs84.lat,
                                                                  options.centerWgs84.lon, 0.);
                    double maximumError = 0.;
                    for (size_t index = 0; index < vertices.size(); ++index)
                    {
                        const auto& p = planar[index];
                        const double longitude =
                            (originX + p.x * .01) / radius * 180. / std::numbers::pi;
                        const double latitude =
                            std::atan(std::sinh((originY + p.y * .01) / radius)) * 180. /
                            std::numbers::pi;
                        std::array<double, 3> expected{};
                        reference.Forward(latitude, longitude, p.z * .01, expected[0], expected[1],
                                          expected[2]);
                        const auto& actual = vertices[index];
                        maximumError =
                            std::max(maximumError, std::hypot(actual.x * .01 - expected[0],
                                                              actual.y * .01 - expected[1],
                                                              actual.z * .01 - expected[2]));
                    }
                    return postprocess_test::Require(maximumError < .02,
                                                     "远处高建筑的底部或顶部未按实际经纬高转换");
                });
            if (!postprocess_test::Require(result.success, result.error))
            {
                return false;
            }
        }
        return true;
    }

    bool checkSphere(const std::filesystem::path& root, bool terrainEnabled)
    {
        constexpr double heightMeters = 1000.;
        const DownTool::LonLat center(116.415799, 39.888175);
        const auto tile = DownTool::GoogleDownBase::LonLatToTile(center, 10);
        postprocess_test::TestPostProcessDown tool;
        tool.Configure(root / "cache", root / "XYZ", 10, 256,
                       DownTool::Tile(tile.X + 2, tile.Y, 10),
                       DownTool::Tile(tile.X + 2, tile.Y, 10), center);
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(root / "FBX"));
        tool.SetFbxTextureSize(256);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Sphere);
        tool.SetFbxMeshSubdivision(1);
        tool.SetFbxTerrainHeightEnabled(terrainEnabled);
        tool.SetFbxTerrainSampleRateMeters(1000.);
        tool.SetFbxElevationSampler(
            [](const DownTool::LonLat&) { return std::optional<double>(heightMeters); });
        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }
        tool.RunPostProcessForTest();
        std::ifstream input(root / "FBX" / "manifest.json");
        const auto manifest = nlohmann::json::parse(input);
        Assimp::Importer importer;
        const aiScene* scene =
            postprocess_test::ReadFbxScene(importer, root / "FBX" / "sphere.fbx");
        if (!postprocess_test::Require(scene && scene->mNumMeshes > 0, "球面导出没有生成网格"))
        {
            return false;
        }
        const GeographicLib::LocalCartesian reference(center.lat, center.lon, 0.);
        double vertexError = 0.;
        double interiorError = 0.;
        for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        {
            const auto& mesh = *scene->mMeshes[meshIndex];
            if (!checkMeshTopology(mesh))
            {
                return false;
            }
            const auto& pixels = manifest.at("textures").at(meshIndex).at("pixelbounds");
            const auto expectedPosition = [&](double u, double v) {
                DownTool::LonLat geographic;
                if (!tool.TryGetLonLatForTilePixel(
                        DownTool::Tile(0, 0, 10),
                        pixels.at("minx").get<double>() +
                            (pixels.at("maxx").get<double>() - pixels.at("minx").get<double>()) * u,
                        pixels.at("miny").get<double>() +
                            (pixels.at("maxy").get<double>() - pixels.at("miny").get<double>()) *
                                (1. - v),
                        geographic))
                {
                    throw std::runtime_error("无法根据纹理像元还原经纬度");
                }
                std::array<double, 3> position{};
                reference.Forward(geographic.lat, geographic.lon,
                                  terrainEnabled ? heightMeters : 0., position[0], position[1],
                                  position[2]);
                return position;
            };
            const auto errorMeters = [&](const std::array<double, 3>& position, double u,
                                         double v) {
                const auto expected = expectedPosition(u, v);
                return std::hypot(position[0] * .01 - expected[0], position[1] * .01 - expected[1],
                                  position[2] * .01 - expected[2]);
            };
            for (unsigned index = 0; index < mesh.mNumVertices; ++index)
            {
                const auto& position = mesh.mVertices[index];
                const auto& uv = mesh.mTextureCoords[0][index];
                vertexError = std::max(
                    vertexError, errorMeters({position.x, position.y, position.z}, uv.x, uv.y));
            }
            for (unsigned index = 0; index < mesh.mNumFaces; ++index)
            {
                const auto& face = mesh.mFaces[index];
                for (const std::array<double, 3> weights :
                     {std::array{1. / 3., 1. / 3., 1. / 3.}, std::array{.17, .31, .52}})
                {
                    std::array<double, 3> position{};
                    double u = 0.;
                    double v = 0.;
                    for (unsigned corner = 0; corner < 3; ++corner)
                    {
                        const auto vertex = face.mIndices[corner];
                        position[0] += mesh.mVertices[vertex].x * weights[corner];
                        position[1] += mesh.mVertices[vertex].y * weights[corner];
                        position[2] += mesh.mVertices[vertex].z * weights[corner];
                        u += mesh.mTextureCoords[0][vertex].x * weights[corner];
                        v += mesh.mTextureCoords[0][vertex].y * weights[corner];
                    }
                    interiorError = std::max(interiorError, errorMeters(position, u, v));
                }
            }
        }
        std::cout << "terrain=" << terrainEnabled << " vertex=" << vertexError
                  << "m interior=" << interiorError << "m\n";
        if (terrainEnabled)
        {
            tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(root / "budget-rejected"));
            tool.SetFbxTerrainMaximumTriangles(2);
            bool rejected = false;
            try
            {
                tool.RunPostProcessForTest();
            }
            catch (const std::runtime_error& error)
            {
                rejected = std::string(error.what()).find("三维采样误差") != std::string::npos;
            }
            if (!postprocess_test::Require(rejected, "球面精度超限时仍接受过小的面数上限"))
            {
                return false;
            }
        }
        return postprocess_test::Require(vertexError < .02,
                                         "球面顶点未按同一经纬高参与 ECEF 转换") &&
               postprocess_test::Require(interiorError < 1.,
                                         "球面三角面内部与参考曲面的误差超过一米");
    }
} // namespace

int main()
{
    try
    {
        const auto root = postprocess_test::MakeTestRoot("sphere-coordinates");
        if (!postprocess_test::ResetDirectory(root))
        {
            return 1;
        }
        return checkSphere(root / "height", true) && checkSphere(root / "zero", false) &&
                       checkBuilding(root / "building")
                   ? 0
                   : 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
