#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "assimp/material.h"

#include "AmapBuildingFbxExporter.h"
#include "PostProcessTestSupport.h"

namespace
{
    using VertexSignature = std::array<float, 15>;
    using TriangleSignature = std::array<VertexSignature, 3>;

    struct SceneSnapshot
    {
        std::vector<std::pair<std::string, VertexSignature>> vertices;
        std::vector<std::pair<std::string, TriangleSignature>> triangles;
        std::set<std::string> materials;
        std::map<int, std::set<std::string>> buildingChunks;
        std::set<std::string> nodeNames;
        unsigned int meshCount = 0;
        bool crossingBuildingIntact = false;
    };

    DownTool::LonLat Wgs84AtMeters(double x, double y)
    {
        constexpr double radius = 6378137.;
        return {
            x / radius * 180. / std::numbers::pi,
            (2. * std::atan(std::exp(y / radius)) - std::numbers::pi / 2.) *
                180. / std::numbers::pi};
    }

    DownTool::AmapVectorFeature MakeBuilding(double x, double y, uint64_t floors)
    {
        DownTool::AmapVectorFeature feature;
        feature.layerName = "building";
        feature.geometryType = DownTool::AmapVectorGeometryType::Polygon;
        feature.hasHeight = true;
        feature.height = floors;
        feature.geometryParts.emplace_back();
        for (const auto& [dx, dy] :
             std::array<std::pair<double, double>, 5>{
                 {{-20., -20.}, {20., -20.}, {20., 20.}, {-20., 20.}, {-20., -20.}}})
        {
            const DownTool::LonLat coordinate =
                DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(Wgs84AtMeters(x + dx, y + dy));
            feature.geometryParts.back().push_back({coordinate.lon, coordinate.lat});
        }
        return feature;
    }

    VertexSignature SignatureFor(const aiMesh& mesh, unsigned int vertexIndex)
    {
        const aiVector3D& position = mesh.mVertices[vertexIndex];
        const aiVector3D& normal = mesh.mNormals[vertexIndex];
        VertexSignature result = {
            position.x, position.y, position.z, normal.x, normal.y, normal.z};
        for (unsigned int channel = 0; channel < 3; ++channel)
        {
            if (mesh.HasTextureCoords(channel))
            {
                const aiVector3D& uv = mesh.mTextureCoords[channel][vertexIndex];
                result[6 + channel * 3] = uv.x;
                result[7 + channel * 3] = uv.y;
                result[8 + channel * 3] = uv.z;
            }
        }
        return result;
    }

    bool ReadSnapshot(const std::filesystem::path& path, SceneSnapshot& snapshot)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, path);
        if (!postprocess_test::Require(scene != nullptr, "cannot read chunked FBX"))
        {
            return false;
        }
        snapshot.meshCount = scene->mNumMeshes;
        for (unsigned int index = 0; index < scene->mNumMaterials; ++index)
        {
            aiString name;
            scene->mMaterials[index]->Get(AI_MATKEY_NAME, name);
            snapshot.materials.insert(name.C_Str());
        }
        if (!postprocess_test::Require(
                snapshot.materials.size() == scene->mNumMaterials,
                "chunked FBX contains duplicate materials"))
        {
            return false;
        }
        for (unsigned int index = 0; index < scene->mNumMeshes; ++index)
        {
            const aiMesh& mesh = *scene->mMeshes[index];
            const std::string name = mesh.mName.C_Str();
            const size_t suffix = name.rfind("_chunk_");
            if (!postprocess_test::Require(
                    suffix != std::string::npos && mesh.HasNormals() &&
                        mesh.HasTextureCoords(0),
                    "mesh has no chunk name, normals or UVs"))
            {
                return false;
            }
            snapshot.nodeNames.insert(name);
            const std::string chunk = name.substr(suffix);
            aiString materialName;
            scene->mMaterials[mesh.mMaterialIndex]->Get(AI_MATKEY_NAME, materialName);
            for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices; ++vertexIndex)
            {
                snapshot.vertices.emplace_back(
                    materialName.C_Str(), SignatureFor(mesh, vertexIndex));
                const aiVector3D& vertex = mesh.mVertices[vertexIndex];
                if (vertex.z > 1.f)
                {
                    const int floors = static_cast<int>(std::lround(vertex.z / 45.f));
                    snapshot.buildingChunks[floors].insert(chunk);
                    if (floors == 9 && vertex.x > 10000.f)
                    {
                        snapshot.crossingBuildingIntact = true;
                    }
                }
            }
            for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex)
            {
                const aiFace& face = mesh.mFaces[faceIndex];
                if (!postprocess_test::Require(face.mNumIndices == 3, "non-triangle face"))
                {
                    return false;
                }
                TriangleSignature triangle;
                for (unsigned int corner = 0; corner < 3; ++corner)
                {
                    triangle[corner] = SignatureFor(mesh, face.mIndices[corner]);
                }
                snapshot.triangles.emplace_back(materialName.C_Str(), triangle);
            }
        }
        const aiNode& root = *scene->mRootNode;
        if (!postprocess_test::Require(
                root.mNumChildren == scene->mNumMeshes,
                "FBX does not retain independently importable chunk nodes"))
        {
            return false;
        }
        for (unsigned int index = 0; index < root.mNumChildren; ++index)
        {
            const aiNode& node = *root.mChildren[index];
            if (!postprocess_test::Require(
                    node.mNumMeshes == 1 && node.mTransformation.IsIdentity() &&
                        snapshot.nodeNames.contains(node.mName.C_Str()),
                    "chunk node changed the shared origin or mesh assignment"))
            {
                return false;
            }
        }
        std::sort(snapshot.vertices.begin(), snapshot.vertices.end());
        std::sort(snapshot.triangles.begin(), snapshot.triangles.end());
        return true;
    }

    bool RequireWholeBuildings(const SceneSnapshot& snapshot)
    {
        const std::map<int, std::set<std::string>> expected = {
            {5, {"_chunk_0_0"}}, {7, {"_chunk_1_0"}}, {9, {"_chunk_0_1"}},
            {11, {"_chunk_-1_0"}}, {13, {"_chunk_0_-1"}}};
        return postprocess_test::Require(
            snapshot.buildingChunks == expected && snapshot.crossingBuildingIntact,
            "whole buildings were split, clipped, or assigned to incorrect grid cells");
    }

    bool RequireSameGeometry(const SceneSnapshot& first, const SceneSnapshot& second)
    {
        return postprocess_test::Require(
            first.vertices == second.vertices && first.triangles == second.triangles &&
                first.materials == second.materials,
            "chunk size changed positions, normals, UVs, triangles or materials");
    }

    bool CheckExportPair(
        const DownTool::AmapVectorDecodedData& data,
        const DownTool::AmapBuildingFbxExportOptions& baseOptions,
        const std::filesystem::path& root)
    {
        DownTool::AmapBuildingFbxExportOptions coarse = baseOptions;
        coarse.outputPath = root / "coarse" / "buildings.fbx";
        const auto coarseResult = DownTool::ExportAmapBuildingsToFbx(data, coarse);
        DownTool::AmapBuildingFbxExportOptions fine = baseOptions;
        fine.SetChunkSizeMeters(100.);
        fine.outputPath = root / "fine" / "buildings.fbx";
        const auto fineResult = DownTool::ExportAmapBuildingsToFbx(data, fine);
        SceneSnapshot coarseSnapshot;
        SceneSnapshot fineSnapshot;
        if (!postprocess_test::Require(
                coarseResult.success && fineResult.success,
                "building export failed: " + coarseResult.error + fineResult.error) ||
            !ReadSnapshot(coarse.outputPath, coarseSnapshot) ||
            !ReadSnapshot(fine.outputPath, fineSnapshot) ||
            !RequireSameGeometry(coarseSnapshot, fineSnapshot) ||
            !postprocess_test::Require(
                coarseSnapshot.meshCount == 6 && fineSnapshot.meshCount == 10 &&
                    coarseResult.materialCount == 2 && fineResult.materialCount == 2 &&
                    fineResult.buildingCount == 5 && fineResult.meshCount == 10,
                "chunking did not split meshes while sharing the two materials"))
        {
            return false;
        }
        if (baseOptions.shape == DownTool::FbxPlanetShape::Plane &&
            !RequireWholeBuildings(fineSnapshot))
        {
            return false;
        }
        return postprocess_test::RequireFbxAxisAndUnit(fine.outputPath);
    }

    bool RequireSignedSeaLevelBuildings(const DownTool::AmapBuildingFbxExportOptions& baseOptions,
                                        const std::filesystem::path& root)
    {
        DownTool::AmapVectorDecodedData data;
        data.features = {MakeBuilding(50., 50., 5)};
        for (const auto shape : {DownTool::FbxPlanetShape::Plane, DownTool::FbxPlanetShape::Sphere})
        {
            std::vector<aiVector3D> seaLevelVertices;
            for (double elevation : {0., 150., -35.5})
            {
                auto options = baseOptions;
                options.outputPath = root / "sea-level" / "buildings.fbx";
                options.shape = shape;
                options.terrainHeightEnabled = true;
                options.elevationSampler = [elevation](const DownTool::LonLat&) {
                    return std::optional<double>(elevation);
                };
                const auto result = DownTool::ExportAmapBuildingScene(
                    data, options, [&](aiScene& scene, std::string&) {
                        std::vector<aiVector3D> vertices;
                        for (unsigned int index = 0; index < scene.mNumMeshes; ++index)
                        {
                            const aiMesh& mesh = *scene.mMeshes[index];
                            vertices.insert(vertices.end(), mesh.mVertices,
                                            mesh.mVertices + mesh.mNumVertices);
                        }
                        if (elevation == 0.)
                        {
                            seaLevelVertices = std::move(vertices);
                            return !seaLevelVertices.empty();
                        }
                        if (vertices.size() != seaLevelVertices.size())
                        {
                            return false;
                        }
                        for (size_t index = 0; index < vertices.size(); ++index)
                        {
                            const aiVector3D& vertex = vertices[index];
                            const aiVector3D& baseline = seaLevelVertices[index];
                            if (std::abs(vertex.x - baseline.x) >= 0.1F ||
                                std::abs(vertex.y - baseline.y) >= 0.1F ||
                                std::abs(vertex.z - baseline.z - elevation * 100.) >= 0.1)
                            {
                                return false;
                            }
                        }
                        return true;
                    });
                if (!postprocess_test::Require(
                        result.success, "建筑未保留相对海拔 0 的有符号高程：" + result.error))
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool RequireUprightBuildingTextures(const std::filesystem::path& path)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, path);
        if (!postprocess_test::Require(scene != nullptr, "cannot read texture orientation FBX"))
        {
            return false;
        }
        bool checkedWall = false;
        bool checkedRoof = false;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        {
            const aiMesh& mesh = *scene->mMeshes[meshIndex];
            aiString texturePath;
            if (!postprocess_test::Require(
                    mesh.HasNormals() && mesh.HasTextureCoords(0) &&
                        scene->mMaterials[mesh.mMaterialIndex]->GetTexture(
                            aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS,
                    "orientation mesh has no normals, UVs or texture"))
            {
                return false;
            }
            const cv::Mat image = cv::imread(postprocess_test::PathToUtf8String(
                path.parent_path() / std::filesystem::u8path(texturePath.C_Str())));
            if (!postprocess_test::Require(!image.empty(), "cannot read orientation texture"))
            {
                return false;
            }
            for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex)
            {
                const aiFace& face = mesh.mFaces[faceIndex];
                const bool roof = mesh.mNormals[face.mIndices[0]].z > 0.5f;
                float minimum = std::numeric_limits<float>::max();
                float maximum = std::numeric_limits<float>::lowest();
                float center = 0.f;
                aiVector3D uv;
                for (unsigned int corner = 0; corner < face.mNumIndices; ++corner)
                {
                    const unsigned int index = face.mIndices[corner];
                    const aiVector3D& point = mesh.mVertices[index];
                    const float heightOrNorth = roof ? point.y : point.z;
                    minimum = std::min(minimum, heightOrNorth);
                    maximum = std::max(maximum, heightOrNorth);
                    center += heightOrNorth / 3.f;
                    uv += mesh.mTextureCoords[0][index] / 3.f;
                }
                uv.x -= std::floor(uv.x);
                uv.y = std::clamp(uv.y, 0.f, 1.f);
                if (mesh.HasTextureCoords(1) && mesh.HasTextureCoords(2))
                {
                    const auto& offset = mesh.mTextureCoords[1][face.mIndices[0]];
                    const auto& upper = mesh.mTextureCoords[2][face.mIndices[0]];
                    uv.x = offset.x + uv.x * (upper.x - offset.x);
                    uv.y = offset.y + uv.y * (upper.y - offset.y);
                }
                const int column = std::clamp(
                    static_cast<int>(uv.x * image.cols), 0, image.cols - 1);
                const int row = std::clamp(
                    static_cast<int>((1.f - uv.y) * image.rows), 0, image.rows - 1);
                const cv::Vec3b color = image.at<cv::Vec3b>(row, column);
                const bool expectedRed = center > (minimum + maximum) * 0.5f;
                if (!postprocess_test::Require(
                        expectedRed ? color[2] > 200 && color[0] < 50 :
                                      color[0] > 200 && color[2] < 50,
                        std::string(roof ? "roof north/south" : "wall top/bottom") +
                            " texture is vertically inverted: " +
                            postprocess_test::PathToUtf8String(path)))
                {
                    return false;
                }
                checkedWall = checkedWall || !roof;
                checkedRoof = checkedRoof || roof;
            }
        }
        return postprocess_test::Require(
            checkedWall && checkedRoof, "orientation test did not sample walls and roofs");
    }

    bool CheckBuildingTextureOrientation(const std::filesystem::path& root)
    {
        DownTool::AmapVectorDecodedData data;
        data.features = {MakeBuilding(0., 0., 5)};
        DownTool::AmapBuildingFbxExportOptions options;
        options.centerWgs84 = {0., 0.};
        options.wallTexturePath = root / "sources" / "wall";
        options.roofTexturePath = root / "sources" / "roof";
        cv::Mat image(96, 96, CV_8UC3, cv::Scalar(255, 0, 0));
        image(cv::Rect(0, 0, 96, 48)).setTo(cv::Scalar(0, 0, 255));
        for (const auto& directory : {options.wallTexturePath, options.roofTexturePath})
        {
            std::filesystem::create_directories(directory);
            if (!postprocess_test::Require(
                    cv::imwrite(postprocess_test::PathToUtf8String(directory / "texture.png"),
                        image),
                    "cannot write orientation fixture"))
            {
                return false;
            }
        }
        for (const bool atlas : {false, true})
        {
            options.wallTextureMode = atlas ?
                DownTool::AmapBuildingFbxTextureMode::MergedAtlas :
                DownTool::AmapBuildingFbxTextureMode::Scattered;
            options.roofTextureMode = atlas ?
                DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas :
                DownTool::AmapBuildingFbxRoofTextureMode::Scattered;
            options.outputPath = root / (atlas ? "atlas" : "scattered") / "buildings.fbx";
            const auto result = DownTool::ExportAmapBuildingsToFbx(data, options);
            if (!postprocess_test::Require(result.success, result.error) ||
                !RequireUprightBuildingTextures(options.outputPath))
            {
                return false;
            }
        }
        options.roofTextureMode = DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
        const auto southWest = Wgs84AtMeters(-40., -40.);
        const auto northEast = Wgs84AtMeters(40., 40.);
        options.satelliteRoofTextures.push_back({
            options.roofTexturePath / "texture.png",
            southWest.lon, southWest.lat, northEast.lon, northEast.lat});
        options.outputPath = root / "satellite" / "buildings.fbx";
        const auto result = DownTool::ExportAmapBuildingsToFbx(data, options);
        if (!postprocess_test::Require(result.success, result.error) ||
            !RequireUprightBuildingTextures(options.outputPath))
        {
            return false;
        }
        return true;
    }

    bool RunTest()
    {
        const auto root = postprocess_test::MakeTestRoot("building-chunks");
        if (!postprocess_test::ResetDirectory(root))
        {
            return false;
        }
        if (!CheckBuildingTextureOrientation(root / "texture-orientation"))
        {
            return false;
        }
        DownTool::AmapVectorDecodedData data;
        data.features = {
            MakeBuilding(50., 50., 5), MakeBuilding(180., 50., 7),
            MakeBuilding(95., 160., 9), MakeBuilding(-50., 50., 11),
            MakeBuilding(50., -50., 13)};
        DownTool::AmapBuildingFbxExportOptions options;
        options.centerWgs84 = {0., 0.};
        options.wallTexturePath = root / "textures" / "wall";
        options.roofTexturePath = root / "textures" / "roof";
        if (!postprocess_test::Require(
                options.GetChunkSizeMeters() == 2200., "chunk default is not 2200 meters") ||
            !postprocess_test::WriteSourceTile(
                options.wallTexturePath / "wall.png", 64, cv::Scalar(100, 150, 200, 255)) ||
            !postprocess_test::WriteSourceTile(
                options.roofTexturePath / "roof.png", 64, cv::Scalar(200, 100, 150, 255)) ||
            !CheckExportPair(data, options, root / "scattered") ||
            !RequireSignedSeaLevelBuildings(options, root))
        {
            return false;
        }
        options.wallTextureMode = DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
        options.roofTextureMode = DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
        if (!CheckExportPair(data, options, root / "atlas"))
        {
            return false;
        }
        options.shape = DownTool::FbxPlanetShape::Sphere;
        if (!CheckExportPair(data, options, root / "sphere-atlas"))
        {
            return false;
        }
        options.shape = DownTool::FbxPlanetShape::Plane;
        options.roofTextureMode = DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
        const auto southWest = Wgs84AtMeters(-500., -500.);
        const auto northEast = Wgs84AtMeters(500., 500.);
        options.satelliteRoofTextures.push_back({
            options.roofTexturePath / "roof.png",
            southWest.lon, southWest.lat, northEast.lon, northEast.lat});
        if (!CheckExportPair(data, options, root / "satellite"))
        {
            return false;
        }
        options.outputPath = root / "invalid" / "buildings.fbx";
        for (double size : {0., -1., std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN(), 1.e-100})
        {
            options.SetChunkSizeMeters(size);
            const auto result = DownTool::ExportAmapBuildingsToFbx(data, options);
            if (!postprocess_test::Require(
                    !result.success && !result.error.empty() &&
                        !std::filesystem::exists(options.outputPath),
                    "invalid chunk size was not rejected before FBX output"))
            {
                return false;
            }
        }
        std::cout << "AmapBuildingChunkTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
