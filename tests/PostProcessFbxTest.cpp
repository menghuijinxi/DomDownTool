#include "PostProcessTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include "CopernicusDem.h"
#include "GeographicLib/LocalCartesian.hpp"
#include "nlohmann/json.hpp"

namespace
{
    bool RequireDistanceLodOutputs(const std::filesystem::path& fbxRoot)
    {
        std::ifstream input(fbxRoot / "manifest.json");
        const auto manifest = nlohmann::json::parse(input);
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxRoot / "plane.fbx");
        if (!postprocess_test::Require(scene != nullptr, "distance LOD FBX cannot be read"))
        {
            return false;
        }
        std::map<std::string, const aiMesh*> meshes;
        for (unsigned int index = 0; index < scene->mNumMeshes; ++index)
        {
            meshes[scene->mMeshes[index]->mName.C_Str()] = scene->mMeshes[index];
        }
        std::set<uint32_t> seenSizes;
        std::set<uint32_t> seenBudgets;
        const auto settings = DownTool::FbxDistanceLodFromJson(manifest.at("distancelod"));
        if (!postprocess_test::Require(settings.has_value(), "manifest did not save LOD rules"))
        {
            return false;
        }
        for (const auto& texture : manifest.at("textures"))
        {
            const double distance = texture.at("distancemeters").get<double>();
            const uint32_t expectedSize = settings->ResolveTextureSize(distance, 512);
            const uint32_t expectedBudget = settings->ResolveMaximumTriangles(distance, 0);
            const auto meshName = fmt::format("tile_{}_{}",
                texture.at("gridx").get<int>(), texture.at("gridy").get<int>());
            const auto found = meshes.find(meshName);
            if (!postprocess_test::Require(
                    texture.at("outputtexturesize") == expectedSize &&
                        texture.at("terrainmaximumtriangles") == expectedBudget &&
                        found != meshes.end() &&
                        (expectedBudget == 0 || found->second->mNumFaces <= expectedBudget),
                    "distance band did not control actual texture size or mesh face count") ||
                !postprocess_test::RequireImageSize(fbxRoot / texture.at("path").get<std::string>(),
                                                    static_cast<int>(expectedSize),
                                                    static_cast<int>(expectedSize)))
            {
                return false;
            }
            seenSizes.insert(expectedSize);
            seenBudgets.insert(expectedBudget);
        }
        return postprocess_test::Require(
            seenSizes == std::set<uint32_t>{128, 256, 512} &&
                seenBudgets == std::set<uint32_t>{0, 31, 128},
            "distance test did not exercise near, medium and far blocks");
    }

    bool ExportDistanceLodFixture(const std::filesystem::path& testRoot)
    {
        postprocess_test::TestPostProcessDown tool;
        const auto root = testRoot / "distance-lod";
        tool.Configure(root / "cache", root / "XYZ", 13, 256,
            DownTool::Tile(4089, 4089, 13), DownTool::Tile(4102, 4102, 13), {0., 0.});
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMaxZoomOnlyEnabled(true);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(root / "FBX"));
        tool.SetFbxTextureSize(512);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        tool.SetFbxTerrainHeightEnabled(true);
        tool.SetFbxTerrainSampleRateMeters(400.);
        tool.SetFbxElevationSampler([](const DownTool::LonLat& point)
        {
            return std::optional<double>(
                200. + 100. * std::sin(point.lon * 500.) * std::cos(point.lat * 500.));
        });
        DownTool::FbxDistanceLod settings;
        settings.SetTextureRules({{30000., 128}, {10000., 256}});
        settings.SetTriangleRules({{30000., 31}, {10000., 128}});
        tool.SetFbxDistanceLod(settings);
        tool.SetMaxDownQueue(2);
        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }
        tool.RunPostProcessForTest();
        return RequireDistanceLodOutputs(root / "FBX");
    }

    bool RequireFbxTexturesBesideModel(
        const std::filesystem::path& fbxPath,
        const std::filesystem::path& fbxRoot)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!postprocess_test::Require(scene != nullptr, "FBX scene is null"))
        {
            return false;
        }

        bool foundTexture = false;
        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials;
             materialIndex++)
        {
            aiString texturePath;
            if (scene->mMaterials[materialIndex]->GetTexture(
                    aiTextureType_DIFFUSE, 0, &texturePath) != AI_SUCCESS)
            {
                continue;
            }

            foundTexture = true;
            const std::filesystem::path relativeTexturePath =
                std::filesystem::u8path(texturePath.C_Str());
            if (!postprocess_test::Require(
                    !relativeTexturePath.has_parent_path(),
                    std::string("FBX texture path is not flat: ") +
                        texturePath.C_Str()) ||
                !postprocess_test::RequireFileExists(fbxRoot / relativeTexturePath))
            {
                return false;
            }
        }

        return postprocess_test::Require(foundTexture, "FBX has no diffuse texture");
    }

    bool RequirePerTileFbxMatchesMerged(const std::filesystem::path& mergedRoot,
                                        const std::filesystem::path& splitRoot)
    {
        using postprocess_test::Require;
        size_t expectedFileCount = 0;
        for (const auto* shape : {"plane", "sphere"})
        {
            Assimp::Importer mergedImporter;
            const auto* merged = postprocess_test::ReadFbxScene(
                mergedImporter, mergedRoot / (std::string(shape) + ".fbx"));
            if (!Require(merged && merged->mNumMeshes > 1, "测试没有生成多个底板网格") ||
                !Require(!std::filesystem::exists(splitRoot / (std::string(shape) + ".fbx")),
                         "单块模式仍然生成了合并 FBX"))
            {
                return false;
            }
            for (unsigned meshIndex = 0; meshIndex < merged->mNumMeshes; ++meshIndex)
            {
                const auto& expected = *merged->mMeshes[meshIndex];
                const auto path =
                    splitRoot / fmt::format("{}_{}.fbx", shape, expected.mName.C_Str());
                Assimp::Importer splitImporter;
                const auto* split = postprocess_test::ReadFbxScene(splitImporter, path);
                if (!Require(split && split->mNumMeshes == 1,
                             "独立 FBX 缺失或包含多个网格：" + path.string()) ||
                    !RequireFbxTexturesBesideModel(path, splitRoot) ||
                    !postprocess_test::RequireFbxAxisAndUnit(path))
                {
                    return false;
                }
                const auto& actual = *split->mMeshes[0];
                if (!Require(actual.mName == expected.mName &&
                                 actual.mNumVertices == expected.mNumVertices &&
                                 actual.mNumFaces == expected.mNumFaces && actual.HasNormals() &&
                                 expected.HasNormals() && actual.HasTextureCoords(0) &&
                                 expected.HasTextureCoords(0),
                             "独立 FBX 的网格结构与合并结果不一致"))
                {
                    return false;
                }
                for (unsigned vertex = 0; vertex < expected.mNumVertices; ++vertex)
                {
                    if (!Require(
                            (actual.mVertices[vertex] - expected.mVertices[vertex]).SquareLength() <
                                    1.e-8F &&
                                (actual.mNormals[vertex] - expected.mNormals[vertex])
                                        .SquareLength() < 1.e-8F &&
                                (actual.mTextureCoords[0][vertex] -
                                 expected.mTextureCoords[0][vertex])
                                        .SquareLength() < 1.e-8F,
                            "拆分 FBX 改变了顶点坐标、边界法线或 UV"))
                    {
                        return false;
                    }
                }
                for (unsigned face = 0; face < expected.mNumFaces; ++face)
                {
                    for (unsigned corner = 0; corner < 3; ++corner)
                    {
                        if (!Require(actual.mFaces[face].mIndices[corner] ==
                                         expected.mFaces[face].mIndices[corner],
                                     "拆分 FBX 改变了三角面索引"))
                        {
                            return false;
                        }
                    }
                }
                const auto* expectedNode = merged->mRootNode->FindNode(expected.mName);
                const auto* actualNode = split->mRootNode->FindNode(actual.mName);
                if (!Require(expectedNode && actualNode &&
                                 expectedNode->mTransformation == actualNode->mTransformation &&
                                 merged->mRootNode->mTransformation ==
                                     split->mRootNode->mTransformation,
                             "独立底板的节点坐标原点发生变化"))
                {
                    return false;
                }
                ++expectedFileCount;
            }
        }
        size_t actualFileCount = 0;
        for (const auto& entry : std::filesystem::directory_iterator(splitRoot))
        {
            if (entry.path().extension() == ".fbx")
            {
                ++actualFileCount;
            }
        }
        std::ifstream input(splitRoot / "manifest.json");
        const auto manifest = nlohmann::json::parse(input);
        return Require(actualFileCount == expectedFileCount &&
                           manifest.value("filemode", "") == "per-tile",
                       "独立 FBX 文件数量或清单输出模式错误");
    }

    bool RequirePerTileFbxExport(const std::filesystem::path& testRoot)
    {
        class PerTileTestDownloader : public postprocess_test::TestPostProcessDown
        {
          public:
            bool GenerateFbx() const { return GeneratePostProcessFbx(true); }
        };
        for (const bool elevation : {false, true})
        {
            const auto root = testRoot / (elevation ? "per-tile-terrain" : "per-tile-flat");
            PerTileTestDownloader tool;
            tool.Configure(root / "cache", root / "XYZ", 15, 32, {16383, 16383, 15},
                           {16386, 16386, 15}, {0., 0.});
            tool.SetFbxTextureSize(64);
            tool.SetFbxTextureFormat(elevation ? "jpg" : "png");
            tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
            tool.SetFbxMeshSubdivision(4);
            tool.SetFbxTerrainHeightEnabled(elevation);
            tool.SetFbxTerrainSampleRateMeters(100.);
            tool.SetFbxTerrainMaximumTriangles(0);
            tool.SetFbxElevationSampler([](const DownTool::LonLat& point) {
                return std::optional<double>(100. + 40. * std::sin(point.lon * 800.) *
                                                        std::cos(point.lat * 800.));
            });
            if (!postprocess_test::WriteSourceTiles(tool))
            {
                return false;
            }
            tool.SetPostProcessEnabled(true);
            tool.SetPostProcessMaxZoomOnlyEnabled(true);
            tool.RunPostProcessForTest();
            tool.SetFbxOutputPath((root / "merged").string());
            if (!postprocess_test::Require(!tool.GetFbxPerTile() && tool.GenerateFbx(),
                                           "默认合并 FBX 导出失败"))
            {
                return false;
            }
            tool.SetFbxOutputPath((root / "split").string());
            tool.SetFbxPerTile(true);
            if (!postprocess_test::Require(tool.GenerateFbx(), "独立底板 FBX 导出失败") ||
                !RequirePerTileFbxMatchesMerged(root / "merged", root / "split"))
            {
                return false;
            }
            tool.SetFbxOutputPath((root / "canceled").string());
            tool.SetProgressCallback([&tool](const DownTool::DownToolProgress& progress) {
                if (progress.stage == "生成独立底板 FBX" && progress.completed == 1)
                {
                    tool.StopDown();
                }
            });
            if (!postprocess_test::Require(!tool.GenerateFbx(), "取消请求没有中止独立 FBX 写出"))
            {
                return false;
            }
            size_t canceledFiles = 0;
            for (const auto& entry : std::filesystem::directory_iterator(root / "canceled"))
            {
                if (entry.path().extension() == ".fbx")
                {
                    ++canceledFiles;
                }
            }
            if (!postprocess_test::Require(canceledFiles == 1, "取消后仍继续写入独立 FBX 文件"))
            {
                return false;
            }
        }
        return true;
    }

    bool HasAnyJpg(const std::filesystem::path& path)
    {
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError) || existsError)
        {
            return false;
        }

        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(path))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".jpg")
            {
                return true;
            }
        }

        return false;
    }

    bool RequireFbxTerrainMesh(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!postprocess_test::Require(scene != nullptr, "terrain FBX scene is null"))
        {
            return false;
        }

        bool foundTerrainMesh = false;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr || mesh->mNumVertices <= 4)
            {
                continue;
            }
            float minimumZ = std::numeric_limits<float>::max();
            float maximumZ = std::numeric_limits<float>::lowest();
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                 vertexIndex++)
            {
                minimumZ = std::min(minimumZ, mesh->mVertices[vertexIndex].z);
                maximumZ = std::max(maximumZ, mesh->mVertices[vertexIndex].z);
            }
            if (maximumZ - minimumZ > 1.F)
            {
                foundTerrainMesh = true;
                break;
            }
        }
        return postprocess_test::Require(foundTerrainMesh,
                                         "FBX 网格没有输出地形高差");
    }

    bool RequireFbxPlaneInsideBounds(
        const std::filesystem::path& fbxPath,
        const DownTool::LonLat& center,
        const DownTool::LonLat& minimum,
        const DownTool::LonLat& maximum)
    {
        constexpr double earthRadius = 6378137.;
        const auto mercatorX = [earthRadius](double longitude)
        {
            return earthRadius * longitude * std::numbers::pi / 180.;
        };
        const auto mercatorY = [earthRadius](double latitude)
        {
            const double radians = latitude * std::numbers::pi / 180.;
            return earthRadius * std::log(
                std::tan(std::numbers::pi / 4. + radians / 2.));
        };
        const double minX = (mercatorX(minimum.lon) - mercatorX(center.lon)) * 100.;
        const double maxX = (mercatorX(maximum.lon) - mercatorX(center.lon)) * 100.;
        const double minY = (mercatorY(minimum.lat) - mercatorY(center.lat)) * 100.;
        const double maxY = (mercatorY(maximum.lat) - mercatorY(center.lat)) * 100.;

        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!postprocess_test::Require(scene != nullptr, "范围检查无法读取 FBX"))
        {
            return false;
        }

        // Assimp 以 float 保存 FBX 坐标；大范围墨卡托坐标转换后允许一个 float ULP。
        constexpr double tolerance = 128.;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr)
            {
                continue;
            }
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                 vertexIndex++)
            {
                const aiVector3D& vertex = mesh->mVertices[vertexIndex];
                if (vertex.x < minX - tolerance || vertex.x > maxX + tolerance ||
                    vertex.y < minY - tolerance || vertex.y > maxY + tolerance)
                {
                    return postprocess_test::Require(
                        false,
                        fmt::format(
                            "FBX 网格包含导出范围外的顶点：({}, {})，范围 x [{}, {}]，y [{}, {}]",
                            vertex.x,
                            vertex.y,
                            minX,
                            maxX,
                            minY,
                            maxY));
                }
            }
        }
        return true;
    }

    size_t CountFbxFaces(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (scene == nullptr)
        {
            return 0;
        }

        size_t faceCount = 0;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh != nullptr)
            {
                faceCount += mesh->mNumFaces;
            }
        }
        return faceCount;
    }

    std::vector<size_t> CountFbxFacesByMesh(
        const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        std::vector<size_t> faceCounts;
        if (scene == nullptr)
        {
            return faceCounts;
        }
        faceCounts.reserve(scene->mNumMeshes);
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            faceCounts.push_back(mesh ? mesh->mNumFaces : 0);
        }
        return faceCounts;
    }

    float MaximumFbxHeight(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (scene == nullptr)
        {
            return std::numeric_limits<float>::lowest();
        }

        float maximumHeight = std::numeric_limits<float>::lowest();
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr)
            {
                continue;
            }
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                 vertexIndex++)
            {
                maximumHeight = std::max(
                    maximumHeight, mesh->mVertices[vertexIndex].z);
            }
        }
        return maximumHeight;
    }

    struct MeshBounds
    {
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        float maxX = std::numeric_limits<float>::lowest();
        float maxY = std::numeric_limits<float>::lowest();
    };

    MeshBounds CalculateMeshBounds(const aiMesh& mesh)
    {
        MeshBounds bounds;
        for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices;
             vertexIndex++)
        {
            const aiVector3D& vertex = mesh.mVertices[vertexIndex];
            bounds.minX = std::min(bounds.minX, vertex.x);
            bounds.minY = std::min(bounds.minY, vertex.y);
            bounds.maxX = std::max(bounds.maxX, vertex.x);
            bounds.maxY = std::max(bounds.maxY, vertex.y);
        }
        return bounds;
    }

    bool RequireTerrainMeshHasNoInternalOpenEdges(const aiMesh& mesh)
    {
        using PointKey = std::tuple<float, float, float>;
        using EdgeKey = std::pair<PointKey, PointKey>;
        const auto pointKey = [](const aiVector3D& vertex)
        {
            return PointKey{vertex.x, vertex.y, vertex.z};
        };
        std::map<EdgeKey, unsigned int> edgeCounts;
        for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; faceIndex++)
        {
            const aiFace& face = mesh.mFaces[faceIndex];
            if (!postprocess_test::Require(
                    face.mNumIndices == 3, "地形 FBX 包含非三角形面"))
            {
                return false;
            }
            for (unsigned int edgeIndex = 0; edgeIndex < 3; edgeIndex++)
            {
                const PointKey first = pointKey(
                    mesh.mVertices[face.mIndices[edgeIndex]]);
                const PointKey second = pointKey(
                    mesh.mVertices[face.mIndices[(edgeIndex + 1) % 3]]);
                edgeCounts[std::minmax(first, second)]++;
            }
        }

        const MeshBounds bounds = CalculateMeshBounds(mesh);
        constexpr float tolerance = 128.F;
        const auto isBoundaryEdge = [&](const auto& edge)
        {
            const auto isOnBoundary = [&](const PointKey& vertex)
            {
                return std::abs(std::get<0>(vertex) - bounds.minX) <= tolerance ||
                    std::abs(std::get<0>(vertex) - bounds.maxX) <= tolerance ||
                    std::abs(std::get<1>(vertex) - bounds.minY) <= tolerance ||
                    std::abs(std::get<1>(vertex) - bounds.maxY) <= tolerance;
            };
            return isOnBoundary(edge.first) && isOnBoundary(edge.second);
        };
        for (const auto& [edge, count] : edgeCounts)
        {
            if (count != 2 && !isBoundaryEdge(edge))
            {
                return postprocess_test::Require(
                    false,
                    fmt::format(
                        "自适应地形网格内部存在未闭合边 count={} p1=({}, {}) p2=({}, {}) bounds=({}, {}, {}, {})",
                        count,
                        std::get<0>(edge.first),
                        std::get<1>(edge.first),
                        std::get<0>(edge.second),
                        std::get<1>(edge.second),
                        bounds.minX,
                        bounds.minY,
                        bounds.maxX,
                        bounds.maxY));
            }
        }
        return true;
    }

    std::vector<std::pair<float, float>> CollectMeshEdge(
        const aiMesh& mesh,
        bool vertical,
        float coordinate,
        float overlapMin,
        float overlapMax)
    {
        constexpr float tolerance = 128.F;
        std::vector<std::pair<float, float>> points;
        for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices;
             vertexIndex++)
        {
            const aiVector3D& vertex = mesh.mVertices[vertexIndex];
            const float edgeCoordinate = vertical ? vertex.x : vertex.y;
            const float alongCoordinate = vertical ? vertex.y : vertex.x;
            if (std::abs(edgeCoordinate - coordinate) <= tolerance &&
                alongCoordinate >= overlapMin - tolerance &&
                alongCoordinate <= overlapMax + tolerance)
            {
                points.emplace_back(alongCoordinate, vertex.z);
            }
        }
        std::ranges::sort(points);
        points.erase(std::unique(points.begin(), points.end()), points.end());
        return points;
    }

    bool RequireMatchingTerrainEdge(
        const aiMesh& first,
        const aiMesh& second,
        bool vertical,
        float coordinate,
        float overlapMin,
        float overlapMax)
    {
        constexpr float tolerance = 128.F;
        const auto firstPoints = CollectMeshEdge(
            first, vertical, coordinate, overlapMin, overlapMax);
        const auto secondPoints = CollectMeshEdge(
            second, vertical, coordinate, overlapMin, overlapMax);
        if (!postprocess_test::Require(
                firstPoints.size() == secondPoints.size() &&
                    firstPoints.size() >= 2,
                "相邻地形块共享边的顶点数量不一致"))
        {
            return false;
        }
        for (size_t index = 0; index < firstPoints.size(); index++)
        {
            if (!postprocess_test::Require(
                    std::abs(firstPoints[index].first -
                             secondPoints[index].first) <= tolerance &&
                        std::abs(firstPoints[index].second -
                                 secondPoints[index].second) <= tolerance,
                    "相邻地形块共享边的位置或高度不一致"))
            {
                return false;
            }

            if (!postprocess_test::Require(
                    first.HasNormals() && second.HasNormals(),
                    "相邻地形块没有显式法线数据"))
            {
                return false;
            }
            const auto findNormal = [vertical, coordinate, overlapMin, overlapMax](
                const aiMesh& mesh,
                const std::pair<float, float>& point,
                aiVector3D& normal)
            {
                constexpr float edgeTolerance = 128.F;
                for (unsigned int vertexIndex = 0;
                     vertexIndex < mesh.mNumVertices;
                     vertexIndex++)
                {
                    const aiVector3D& vertex = mesh.mVertices[vertexIndex];
                    const float edgeCoordinate = vertical ? vertex.x : vertex.y;
                    const float alongCoordinate = vertical ? vertex.y : vertex.x;
                    if (std::abs(edgeCoordinate - coordinate) <= edgeTolerance &&
                        alongCoordinate >= overlapMin - edgeTolerance &&
                        alongCoordinate <= overlapMax + edgeTolerance &&
                        std::abs(alongCoordinate - point.first) <= edgeTolerance &&
                        std::abs(vertex.z - point.second) <= edgeTolerance)
                    {
                        normal = mesh.mNormals[vertexIndex];
                        return true;
                    }
                }
                return false;
            };

            aiVector3D firstNormal;
            aiVector3D secondNormal;
            if (!postprocess_test::Require(
                    findNormal(first, firstPoints[index], firstNormal) &&
                        findNormal(second, secondPoints[index], secondNormal),
                    "相邻地形块共享边缺少对应法线"))
            {
                return false;
            }
            const float firstLength = firstNormal.Length();
            const float secondLength = secondNormal.Length();
            const float normalDot = firstLength > 0.F && secondLength > 0.F ?
                (firstNormal.x * secondNormal.x +
                    firstNormal.y * secondNormal.y +
                    firstNormal.z * secondNormal.z) /
                    (firstLength * secondLength) :
                -1.F;
            if (!postprocess_test::Require(
                    normalDot >= 0.999F,
                    fmt::format(
                        "相邻地形块共享边法线不一致，点序号 {}，点积 {}",
                        index,
                        normalDot)))
            {
                return false;
            }
        }
        return true;
    }

    bool RequireFbxTerrainSeamsClosed(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!postprocess_test::Require(scene != nullptr, "接缝检查无法读取 FBX"))
        {
            return false;
        }

        std::vector<MeshBounds> meshBounds;
        meshBounds.reserve(scene->mNumMeshes);
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (!postprocess_test::Require(mesh != nullptr, "地形 FBX 包含空网格") ||
                !postprocess_test::Require(
                    mesh->HasNormals(),
                    "地形 FBX 网格没有显式法线") ||
                !RequireTerrainMeshHasNoInternalOpenEdges(*mesh))
            {
                return false;
            }
            meshBounds.push_back(CalculateMeshBounds(*mesh));
        }

        constexpr float tolerance = 128.F;
        size_t sharedEdgeCount = 0;
        for (unsigned int firstIndex = 0; firstIndex < scene->mNumMeshes;
             firstIndex++)
        {
            for (unsigned int secondIndex = firstIndex + 1;
                 secondIndex < scene->mNumMeshes; secondIndex++)
            {
                const MeshBounds& firstBounds = meshBounds[firstIndex];
                const MeshBounds& secondBounds = meshBounds[secondIndex];
                const float overlapMinY =
                    std::max(firstBounds.minY, secondBounds.minY);
                const float overlapMaxY =
                    std::min(firstBounds.maxY, secondBounds.maxY);
                const bool firstWest =
                    std::abs(firstBounds.maxX - secondBounds.minX) <= tolerance;
                const bool secondWest =
                    std::abs(secondBounds.maxX - firstBounds.minX) <= tolerance;
                if ((firstWest || secondWest) &&
                    overlapMaxY - overlapMinY > tolerance)
                {
                    sharedEdgeCount++;
                    if (!RequireMatchingTerrainEdge(
                            *scene->mMeshes[firstIndex],
                            *scene->mMeshes[secondIndex],
                            true,
                            firstWest ?
                                (firstBounds.maxX + secondBounds.minX) / 2.F :
                                (secondBounds.maxX + firstBounds.minX) / 2.F,
                            overlapMinY,
                            overlapMaxY))
                    {
                        return false;
                    }
                }

                const float overlapMinX =
                    std::max(firstBounds.minX, secondBounds.minX);
                const float overlapMaxX =
                    std::min(firstBounds.maxX, secondBounds.maxX);
                const bool firstSouth =
                    std::abs(firstBounds.maxY - secondBounds.minY) <= tolerance;
                const bool secondSouth =
                    std::abs(secondBounds.maxY - firstBounds.minY) <= tolerance;
                if ((firstSouth || secondSouth) &&
                    overlapMaxX - overlapMinX > tolerance)
                {
                    sharedEdgeCount++;
                    if (!RequireMatchingTerrainEdge(
                            *scene->mMeshes[firstIndex],
                            *scene->mMeshes[secondIndex],
                            false,
                            firstSouth ?
                                (firstBounds.maxY + secondBounds.minY) / 2.F :
                                (secondBounds.maxY + firstBounds.minY) / 2.F,
                            overlapMinX,
                            overlapMaxX))
                    {
                        return false;
                    }
                }
            }
        }
        return postprocess_test::Require(
            sharedEdgeCount > 0, "接缝测试没有找到相邻的地形块");
    }

    bool RequireFbxNormalsUseEqualFaceWeights(
        const std::filesystem::path& fbxPath)
    {
        using NormalKey = std::array<int64_t, 2>;
        struct NormalSums
        {
            std::array<double, 3> equalWeight{};
            std::array<double, 3> areaWeight{};
        };

        const auto makeNormalKey = [](const aiVector3D& vertex)
        {
            constexpr double keyScale = 1000.;
            return NormalKey{
                static_cast<int64_t>(std::llround(vertex.x * keyScale)),
                static_cast<int64_t>(std::llround(vertex.y * keyScale))};
        };
        const auto vectorLength = [](const std::array<double, 3>& vector)
        {
            return std::sqrt(
                vector[0] * vector[0] +
                vector[1] * vector[1] +
                vector[2] * vector[2]);
        };
        const auto normalizedDot = [&](const std::array<double, 3>& first,
                                       const std::array<double, 3>& second)
        {
            const double firstLength = vectorLength(first);
            const double secondLength = vectorLength(second);
            if (firstLength <= std::numeric_limits<double>::epsilon() ||
                secondLength <= std::numeric_limits<double>::epsilon())
            {
                return -1.;
            }
            return (
                first[0] * second[0] +
                first[1] * second[1] +
                first[2] * second[2]) /
                (firstLength * secondLength);
        };

        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!postprocess_test::Require(
                scene != nullptr, "法线权重检查无法读取 FBX"))
        {
            return false;
        }

        std::map<NormalKey, NormalSums> normalSums;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes;
             meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (!postprocess_test::Require(
                    mesh != nullptr && mesh->HasNormals(),
                    "地形 FBX 缺少可检查的显式法线"))
            {
                return false;
            }

            for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces;
                 faceIndex++)
            {
                const aiFace& face = mesh->mFaces[faceIndex];
                if (!postprocess_test::Require(
                        face.mNumIndices == 3, "地形 FBX 包含非三角形面"))
                {
                    return false;
                }

                const aiVector3D& first = mesh->mVertices[face.mIndices[0]];
                const aiVector3D& second = mesh->mVertices[face.mIndices[1]];
                const aiVector3D& third = mesh->mVertices[face.mIndices[2]];
                const std::array<double, 3> firstEdge{
                    second.x - first.x,
                    second.y - first.y,
                    second.z - first.z};
                const std::array<double, 3> secondEdge{
                    third.x - first.x,
                    third.y - first.y,
                    third.z - first.z};
                const std::array<double, 3> faceNormal{
                    firstEdge[1] * secondEdge[2] -
                        firstEdge[2] * secondEdge[1],
                    firstEdge[2] * secondEdge[0] -
                        firstEdge[0] * secondEdge[2],
                    firstEdge[0] * secondEdge[1] -
                        firstEdge[1] * secondEdge[0]};
                const double faceNormalLength = vectorLength(faceNormal);
                if (faceNormalLength <= std::numeric_limits<double>::epsilon())
                {
                    continue;
                }

                for (unsigned int faceVertexIndex = 0;
                     faceVertexIndex < face.mNumIndices;
                     faceVertexIndex++)
                {
                    const unsigned int vertexIndex =
                        face.mIndices[faceVertexIndex];
                    NormalSums& sums =
                        normalSums[makeNormalKey(mesh->mVertices[vertexIndex])];
                    for (size_t component = 0; component < 3; component++)
                    {
                        sums.equalWeight[component] +=
                            faceNormal[component] / faceNormalLength;
                        sums.areaWeight[component] += faceNormal[component];
                    }
                }
            }
        }

        bool foundAreaSensitiveVertex = false;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes;
             meshIndex++)
        {
            const aiMesh& mesh = *scene->mMeshes[meshIndex];
            for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices;
                 vertexIndex++)
            {
                const auto sums = normalSums.find(
                    makeNormalKey(mesh.mVertices[vertexIndex]));
                if (!postprocess_test::Require(
                        sums != normalSums.end(), "地形顶点缺少累计面法线"))
                {
                    return false;
                }

                const aiVector3D& actual = mesh.mNormals[vertexIndex];
                const std::array<double, 3> actualNormal{
                    actual.x, actual.y, actual.z};
                const double normalDot =
                    normalizedDot(sums->second.equalWeight, actualNormal);
                if (!postprocess_test::Require(
                        normalDot >= 0.999,
                        fmt::format(
                            "地形法线不是单位面法线等权平均，mesh={} vertex={} 点积={}",
                            meshIndex,
                            vertexIndex,
                            normalDot)))
                {
                    return false;
                }

                foundAreaSensitiveVertex =
                    foundAreaSensitiveVertex ||
                    normalizedDot(
                        sums->second.equalWeight,
                        sums->second.areaWeight) < 0.999;
            }
        }

        return postprocess_test::Require(
            foundAreaSensitiveVertex,
            "法线测试地形没有覆盖面积权重与等权平均的差异");
    }

    std::filesystem::path FindFirstPng(const std::filesystem::path& directory)
    {
        std::error_code iteratorError;
        for (std::filesystem::directory_iterator iterator(directory, iteratorError), end;
            !iteratorError && iterator != end;
            iterator.increment(iteratorError))
        {
            if (iterator->is_regular_file() && iterator->path().extension() == ".png")
            {
                return iterator->path();
            }
        }
        return {};
    }

    bool RequireSignedSeaLevelTerrain(const std::filesystem::path& testRoot)
    {
        const auto root = testRoot / "sea-level";
        postprocess_test::TestPostProcessDown tool;
        tool.Configure(root / "cache", root / "XYZ", 15, 32, DownTool::Tile(16384, 16384, 15),
                       DownTool::Tile(16385, 16385, 15), {0.005, -0.005});
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(root / "FBX"));
        tool.SetFbxTextureSize(64);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
        tool.SetFbxTerrainHeightEnabled(true);
        tool.SetFbxTerrainSampleRateMeters(100.);
        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        std::map<std::string, std::vector<aiVector3D>> seaLevelVertices;
        for (double elevation : {0., 150., -35.5})
        {
            tool.SetFbxElevationSampler(
                [elevation](const DownTool::LonLat&) { return std::optional<double>(elevation); });
            tool.RunPostProcessForTest();
            for (const std::string shape : {"plane", "sphere"})
            {
                Assimp::Importer importer;
                const aiScene* scene =
                    postprocess_test::ReadFbxScene(importer, root / "FBX" / (shape + ".fbx"));
                if (!postprocess_test::Require(scene && scene->mNumMeshes > 0,
                                               "无法读取海拔零面地形"))
                {
                    return false;
                }
                std::vector<aiVector3D> vertices;
                for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
                {
                    const aiMesh& mesh = *scene->mMeshes[meshIndex];
                    vertices.insert(vertices.end(), mesh.mVertices,
                                    mesh.mVertices + mesh.mNumVertices);
                }
                if (shape == "sphere")
                {
                    const GeographicLib::LocalCartesian reference(-0.005, 0.005, 0.);
                    for (const auto& vertex : vertices)
                    {
                        double latitude = 0.;
                        double longitude = 0.;
                        double height = 0.;
                        reference.Reverse(vertex.x * .01, vertex.y * .01, vertex.z * .01, latitude,
                                          longitude, height);
                        if (!postprocess_test::Require(std::abs(height - elevation) < .001,
                                                       "球面地形未保留有符号高程"))
                        {
                            return false;
                        }
                    }
                    continue;
                }
                if (elevation == 0.)
                {
                    seaLevelVertices[shape] = std::move(vertices);
                    continue;
                }
                const auto& baseline = seaLevelVertices.at(shape);
                if (!postprocess_test::Require(!vertices.empty() &&
                                                   vertices.size() == baseline.size(),
                                               "恒定高程改变了地形网格拓扑"))
                {
                    return false;
                }
                for (size_t index = 0; index < vertices.size(); ++index)
                {
                    const aiVector3D& vertex = vertices[index];
                    if (!postprocess_test::Require(
                            std::abs(vertex.x - baseline[index].x) < 0.1F &&
                                std::abs(vertex.y - baseline[index].y) < 0.1F &&
                                std::abs(vertex.z - baseline[index].z - elevation * 100.) < 0.1,
                            "地形未保留相对海拔 0 的有符号高程：" + shape))
                    {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    bool RequireTerrainExclusionMesh(const std::filesystem::path& testRoot)
    {
        const auto root = testRoot / "terrain-exclusion";
        postprocess_test::TestPostProcessDown tool;
        tool.Configure(root / "cache", root / "XYZ", 15, 32,
            DownTool::Tile(16384, 16384, 15), DownTool::Tile(16385, 16385, 15),
            {0.005, -0.005});
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMaxZoomOnlyEnabled(true);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxTextureSize(64);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
        tool.SetFbxTerrainSampleRateMeters(100.);
        const auto minimum = tool.GetMinLonLat();
        const auto maximum = tool.GetMaxLonLat();
        const auto rectangle = [](double west, double south, double east, double north)
        {
            return DownTool::Wgs84Ring{{west, south}, {east, south},
                                       {east, north}, {west, north}};
        };
        tool.SetDownloadPolygonsWgs84({
            rectangle(minimum.lon, minimum.lat, maximum.lon, maximum.lat)});
        const double middleLongitude = (minimum.lon + maximum.lon) * 0.5;
        const auto exclusion = rectangle(
            middleLongitude - (maximum.lon - minimum.lon) * 0.13,
            minimum.lat - 0.01, maximum.lon + 0.01, maximum.lat + 0.01);
        const double clippedLongitude = exclusion.front().lon;
        constexpr double earthRadius = 6378137.;
        const double expectedMaxX =
            (clippedLongitude - tool.GetCenterLonLat().lon) * earthRadius *
            std::numbers::pi / 180. * 100.;
        tool.SetFbxExcludedPolygonsWgs84({exclusion});
        tool.SetFbxElevationSampler([](const DownTool::LonLat& point)
        {
            return std::optional<double>(150. + point.lon * 10000.);
        });
        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }
        for (const auto [elevated, pureTerrain] :
             {std::pair(false, false), std::pair(true, false), std::pair(true, true)})
        {
            if (pureTerrain)
            {
                if (!postprocess_test::ResetDirectory(root / "XYZ"))
                {
                    return false;
                }
                for (int32_t y = 16384; y <= 16385; ++y)
                {
                    if (!postprocess_test::Require(
                            std::filesystem::remove(tool.TilePath({16385, y, 15})),
                            "无法移除排除区内的测试影像"))
                    {
                        return false;
                    }
                }
            }
            tool.SetPostProcessEnabled(!pureTerrain);
            tool.SetPostProcessMaxZoomOnlyEnabled(!pureTerrain);
            const auto fbxRoot = root / (pureTerrain ? "outer-only" : elevated ? "elevated" : "flat");
            tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(fbxRoot));
            tool.SetFbxTerrainHeightEnabled(elevated);
            tool.RunPostProcessForTest();
            Assimp::Importer importer;
            const auto* scene = postprocess_test::ReadFbxScene(importer, fbxRoot / "plane.fbx");
            if (!postprocess_test::Require(scene && scene->mNumMeshes > 0,
                    "排除区裁切没有生成底板网格") ||
                !postprocess_test::RequireFileExists(fbxRoot / "sphere.fbx"))
            {
                return false;
            }
            float actualMaxX = std::numeric_limits<float>::lowest();
            for (unsigned int index = 0; index < scene->mNumMeshes; ++index)
            {
                const auto& mesh = *scene->mMeshes[index];
                for (unsigned int vertexIndex = 0; vertexIndex < mesh.mNumVertices; ++vertexIndex)
                {
                    const auto& vertex = mesh.mVertices[vertexIndex];
                    actualMaxX = std::max(actualMaxX, vertex.x);
                    const double longitude = vertex.x / 100. / earthRadius * 180. /
                        std::numbers::pi + tool.GetCenterLonLat().lon;
                    const double expectedHeight = elevated ? (150. + longitude * 10000.) * 100. : 0.;
                    if (!postprocess_test::Require(vertex.x <= expectedMaxX + 0.02 &&
                            std::abs(vertex.z - expectedHeight) < 0.02,
                            "底板越过排除边界或裁切交点的高程插值错误"))
                    {
                        return false;
                    }
                }
            }
            if (!postprocess_test::Require(std::abs(actualMaxX - expectedMaxX) < 0.02,
                    "跨边界瓦块被整体丢弃，没有保留精确裁切边"))
            {
                return false;
            }
        }
        return true;
    }

    bool RequireFlattenedTerrainMesh(const std::filesystem::path& testRoot)
    {
        const auto root = testRoot / "flattened-terrain";
        postprocess_test::TestPostProcessDown tool;
        tool.Configure(root / "cache", root / "XYZ", 15, 32, DownTool::Tile(16384, 16384, 15),
                       DownTool::Tile(16387, 16387, 15), {0.02, -0.02});
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxTextureSize(64);
        tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        tool.SetFbxTerrainHeightEnabled(true);
        tool.SetFbxTerrainSampleRateMeters(30.);
        DownTool::CopernicusDemOptions options;
        options.centerWgs84 = tool.GetCenterLonLat();
        options.radiusMeters = 10000.;
        options.cachePath = root / "dem-cache";
        options.SetFlattenPolygonsWgs84({{{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}}});
        options.downloadCallback = [](std::string_view, const std::filesystem::path& path,
                                      const DownTool::CopernicusDemCancellationCallback&,
                                      std::string&) {
            cv::Mat image(8, 8, CV_32FC1);
            for (int y = 0; y < image.rows; ++y)
            {
                for (int x = 0; x < image.cols; ++x)
                {
                    image.at<float>(y, x) = x * 20.F;
                }
            }
            std::vector<unsigned char> encoded;
            cv::imencode(".tif", image, encoded);
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
            return output.good();
        };
        std::string error;
        const auto elevation = DownTool::CopernicusDemData::Prepare(options, error);
        if (!postprocess_test::Require(elevation != nullptr, "无法准备平整地形高程：" + error))
        {
            return false;
        }
        const double expected = elevation->Sample(tool.GetCenterLonLat()).value();
        tool.SetFbxElevationSampler(
            [elevation](const DownTool::LonLat& point) { return elevation->Sample(point); });
        tool.SetFbxFlattenRegions(elevation->GetFlattenedRegions());
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(root / "FBX"));
        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }
        tool.RunPostProcessForTest();
        Assimp::Importer importer;
        const auto* scene = postprocess_test::ReadFbxScene(importer, root / "FBX/plane.fbx");
        if (!postprocess_test::Require(scene && scene->mNumMeshes >= 4,
                                       "平整区域没有生成多块地形 FBX"))
        {
            return false;
        }
        for (unsigned index = 0; index < scene->mNumMeshes; ++index)
        {
            const auto& mesh = *scene->mMeshes[index];
            for (unsigned vertex = 0; vertex < mesh.mNumVertices; ++vertex)
            {
                if (!postprocess_test::Require(
                        std::abs(mesh.mVertices[vertex].z - expected * 100.) < 0.01,
                        "平整区域内不同网格块没有使用相同的平均海拔"))
                {
                    return false;
                }
            }
        }
        // 小范围未覆盖采样网格点时，也必须沿轮廓切出平整面。
        options.SetFlattenPolygonsWgs84(
            {{{0.02001, -0.02004}, {0.02004, -0.02004}, {0.02004, -0.02001}, {0.02001, -0.02001}}});
        const auto tiny = DownTool::CopernicusDemData::Prepare(options, error);
        if (!postprocess_test::Require(tiny != nullptr, "无法准备小平整区域"))
        {
            return false;
        }
        tool.SetFbxElevationSampler(
            [tiny](const DownTool::LonLat& point) { return tiny->Sample(point); });
        tool.SetFbxFlattenRegions(tiny->GetFlattenedRegions());
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(root / "tiny-FBX"));
        tool.RunPostProcessForTest();
        Assimp::Importer tinyImporter;
        const auto* tinyScene =
            postprocess_test::ReadFbxScene(tinyImporter, root / "tiny-FBX/plane.fbx");
        if (!postprocess_test::Require(tinyScene && tinyScene->mNumMeshes > 0,
                                       "小平整区域没有生成 FBX"))
        {
            return false;
        }
        const auto& polygon = options.GetFlattenPolygonsWgs84().front();
        const double tinyHeight = tiny->Sample({0.020025, -0.020025}).value();
        size_t flattenedFaces = 0;
        const auto coordinate = [&](const aiVector3D& vertex) {
            constexpr double earthRadius = 6378137.;
            const double centerY =
                earthRadius *
                std::log(std::tan(std::numbers::pi / 4. +
                                  tool.GetCenterLonLat().lat * std::numbers::pi / 360.));
            return DownTool::LonLat(
                tool.GetCenterLonLat().lon +
                    vertex.x / (100. * earthRadius) * 180. / std::numbers::pi,
                (2. * std::atan(std::exp((vertex.y / 100. + centerY) / earthRadius)) -
                 std::numbers::pi / 2.) *
                    180. / std::numbers::pi);
        };
        for (unsigned index = 0; index < tinyScene->mNumMeshes; ++index)
        {
            const auto& mesh = *tinyScene->mMeshes[index];
            for (unsigned face = 0; face < mesh.mNumFaces; ++face)
            {
                const auto& triangle = mesh.mFaces[face];
                const auto& first = mesh.mVertices[triangle.mIndices[0]];
                const auto& second = mesh.mVertices[triangle.mIndices[1]];
                const auto& third = mesh.mVertices[triangle.mIndices[2]];
                if (DownTool::GeographicRingContainsPoint(
                        polygon, coordinate((first + second + third) / 3.F)))
                {
                    ++flattenedFaces;
                    if (!postprocess_test::Require(std::abs(first.z - tinyHeight * 100.) < 0.01 &&
                                                       std::abs(second.z - tinyHeight * 100.) <
                                                           0.01 &&
                                                       std::abs(third.z - tinyHeight * 100.) < 0.01,
                                                   "跨平整区域轮廓的三角面没有被切开并统一高度"))
                    {
                        return false;
                    }
                }
            }
        }
        if (!postprocess_test::Require(flattenedFaces > 0, "小于采样间距的平整区被网格简化漏掉"))
        {
            return false;
        }
        return true;
    }

    bool RunTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("fbx");
        if (!postprocess_test::ResetDirectory(testRoot) ||
            !RequireSignedSeaLevelTerrain(testRoot) || !RequireTerrainExclusionMesh(testRoot) ||
            !RequireFlattenedTerrainMesh(testRoot) || !RequirePerTileFbxExport(testRoot))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown tool;
        const std::filesystem::path outputRoot = testRoot / "output";
        const std::filesystem::path xyzRoot = outputRoot / "XYZ";
        const std::filesystem::path fbxRoot = outputRoot / "FBX";
        tool.Configure(
            testRoot / "cache",
            xyzRoot,
            2,
            32,
            DownTool::Tile(1, 1, 2),
            DownTool::Tile(2, 2, 2),
            DownTool::LonLat(45., 0.));
        tool.SetPostProcessEnabled(true);
        tool.SetPostProcessMergedImageEnabled(true);
        tool.SetPostProcessMergedImageResolution(64);
        tool.SetPostProcessMergedImageOutputPath(
            postprocess_test::PathToUtf8String(outputRoot / "merged.png"));
        tool.SetPostProcessFbxEnabled(true);
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(fbxRoot));
        tool.SetFbxTextureSize(64);
        // 全球尺度的合成样例用于平面流程；球面精度由区域回归测试单独约束。
        tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        tool.SetFbxMeshSubdivision(8);
        tool.SetMaxDownQueue(2);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        const std::filesystem::path planeFbxPath = fbxRoot / "plane.fbx";

        if (!postprocess_test::RequireFileExists(xyzRoot / "meta.json") ||
            !postprocess_test::RequireFileExists(xyzRoot / "tilemapresource.xml") ||
            !postprocess_test::RequireImageSize(outputRoot / "merged.png", 64, 64) ||
            !postprocess_test::RequireFileExists(fbxRoot / "manifest.json") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(fbxRoot / "manifest.json", "textures"),
                "manifest.json does not list textures") ||
            !postprocess_test::Require(
                postprocess_test::HasAnyPng(fbxRoot),
                "FBX texture directory has no png output") ||
            !postprocess_test::RequireNonEmptyFile(planeFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(planeFbxPath) ||
            !postprocess_test::RequirePlaneFbxUvMatchesNorthAxis(planeFbxPath) ||
            !RequireFbxTexturesBesideModel(planeFbxPath, fbxRoot) ||
            !postprocess_test::Require(
                !std::filesystem::exists(fbxRoot / "mesh") &&
                    !std::filesystem::exists(fbxRoot / "textures"),
                "FBX output still contains legacy nested directories"))
        {
            return false;
        }

        const std::filesystem::path texturePath = FindFirstPng(fbxRoot);
        const cv::Scalar staleColor(13, 17, 251, 255);
        if (!postprocess_test::Require(!texturePath.empty(), "FBX texture path is empty") ||
            !postprocess_test::WriteSourceTile(texturePath, 64, staleColor))
        {
            return false;
        }

        tool.RunPostProcessForTest();
        if (!postprocess_test::RequirePixelNotColor(
            texturePath, 1, 2, cv::Vec4b(13, 17, 251, 255)))
        {
            return false;
        }

        const std::filesystem::path jpgFbxRoot = outputRoot / "FBX-JPG";
        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(jpgFbxRoot));
        tool.SetFbxTextureFormat("jpg");
        tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        tool.RunPostProcessForTest();
        const std::filesystem::path jpgPlaneFbxPath = jpgFbxRoot / "plane.fbx";
        if (!postprocess_test::RequireFileExists(jpgFbxRoot / "manifest.json") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(
                    jpgFbxRoot / "manifest.json", "\"textureformat\":\"jpg\""),
                "JPG FBX manifest does not record jpg texture format") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(
                    jpgFbxRoot / "manifest.json", "\"texturecontenttype\":\"image/jpeg\""),
                "JPG FBX manifest does not record image/jpeg content type") ||
            !postprocess_test::Require(HasAnyJpg(jpgFbxRoot),
                                      "FBX texture directory has no jpg output") ||
            !postprocess_test::Require(
                !postprocess_test::HasAnyPng(jpgFbxRoot),
                "JPG FBX texture directory still contains png output") ||
            !RequireFbxTexturesBesideModel(jpgPlaneFbxPath, jpgFbxRoot))
        {
            return false;
        }

        tool.SetFbxOutputPath(postprocess_test::PathToUtf8String(fbxRoot));
        tool.SetFbxTextureFormat("png");
        tool.SetFbxShape(DownTool::FbxPlanetShape::Plane);

        tool.SetFbxTerrainHeightEnabled(true);
        tool.SetFbxElevationReferenceMeters(100.);
        tool.SetFbxElevationSampler(
            [](const DownTool::LonLat& lonLat)
            {
                return std::optional<double>(100. + lonLat.lat * 10.);
            });
        tool.RunPostProcessForTest();
        if (!postprocess_test::Require(
                postprocess_test::ContainsText(
                    fbxRoot / "manifest.json", "\"terrainheight\":true"),
                "FBX 清单没有记录导出高度状态") ||
            !RequireFbxTerrainMesh(planeFbxPath) ||
            !RequireFbxPlaneInsideBounds(
                planeFbxPath,
                tool.GetCenterLonLat(),
                tool.GetMinLonLat(),
                tool.GetMaxLonLat()))
        {
            return false;
        }

        tool.SetFbxElevationSampler(
            [](const DownTool::LonLat&)
            {
                return std::optional<double>(100.);
            });
        tool.RunPostProcessForTest();
        const size_t flatFaceCount = CountFbxFaces(planeFbxPath);
        if (!postprocess_test::Require(
                flatFaceCount > 0 && flatFaceCount <= 8 * 64,
                "平坦 DEM 未按误差进行网格简化"))
        {
            return false;
        }

        tool.SetFbxElevationSampler(
            [](const DownTool::LonLat& lonLat)
            {
                return std::optional<double>(100. + lonLat.lat * lonLat.lat);
            });
        tool.RunPostProcessForTest();
        const size_t detailedFaceCount = CountFbxFaces(planeFbxPath);
        if (!postprocess_test::Require(
                detailedFaceCount > flatFaceCount,
                "高程变化区域没有增加网格细节") ||
            !RequireFbxTerrainSeamsClosed(planeFbxPath))
        {
            return false;
        }

        const DownTool::LonLat minimum = tool.GetMinLonLat();
        const DownTool::LonLat maximum = tool.GetMaxLonLat();
        constexpr double earthRadius = 6378137.;
        const auto mercatorY = [earthRadius](double latitude)
        {
            const double radians = latitude * std::numbers::pi / 180.;
            return earthRadius * std::log(
                std::tan(std::numbers::pi / 4. + radians / 2.));
        };
        const auto latitudeFromMercator = [earthRadius](double value)
        {
            return (2. * std::atan(std::exp(value / earthRadius)) -
                    std::numbers::pi / 2.) *
                180. / std::numbers::pi;
        };
        const double peakLongitude = minimum.lon +
            (maximum.lon - minimum.lon) * 0.25;
        const double peakLatitude = latitudeFromMercator(
            mercatorY(maximum.lat) +
            (mercatorY(minimum.lat) - mercatorY(maximum.lat)) * 0.25);
        const double longitudeTolerance =
            std::abs(maximum.lon - minimum.lon) / 2048.;
        const double latitudeTolerance =
            std::abs(maximum.lat - minimum.lat) / 2048.;
        tool.SetFbxElevationSampler(
            [=](const DownTool::LonLat& lonLat)
            {
                const bool atPeak =
                    std::abs(lonLat.lon - peakLongitude) <= longitudeTolerance &&
                    std::abs(lonLat.lat - peakLatitude) <= latitudeTolerance;
                return std::optional<double>(atPeak ? 600. : 100.);
            });
        tool.RunPostProcessForTest();
        if (!postprocess_test::Require(
                MaximumFbxHeight(planeFbxPath) > 40000.F,
                "完整高度场采样没有保留内部孤立山峰"))
        {
            return false;
        }

        postprocess_test::TestPostProcessDown sampleRateTool;
        const std::filesystem::path sampleRateOutput =
            testRoot / "sample-rate-output";
        const std::filesystem::path sampleRateFbx = sampleRateOutput / "FBX";
        sampleRateTool.Configure(
            testRoot / "sample-rate-cache",
            sampleRateOutput / "XYZ",
            15,
            32,
            DownTool::Tile(16384, 16384, 15),
            DownTool::Tile(16385, 16385, 15),
            DownTool::LonLat(0.005, -0.005));
        sampleRateTool.SetPostProcessEnabled(true);
        sampleRateTool.SetPostProcessFbxEnabled(true);
        sampleRateTool.SetFbxOutputPath(
            postprocess_test::PathToUtf8String(sampleRateFbx));
        sampleRateTool.SetFbxTextureSize(64);
        sampleRateTool.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        sampleRateTool.SetFbxTerrainHeightEnabled(true);
        sampleRateTool.SetFbxElevationReferenceMeters(100.);
        sampleRateTool.SetFbxElevationSampler(
            [](const DownTool::LonLat& lonLat)
            {
                const double wave =
                    std::sin(lonLat.lon * 25000.) *
                    std::sin(lonLat.lat * 25000.);
                return std::optional<double>(200. + wave * 100.);
            });
        sampleRateTool.SetMaxDownQueue(2);
        if (!postprocess_test::WriteSourceTiles(sampleRateTool))
        {
            return false;
        }

        sampleRateTool.SetFbxTerrainSampleRateMeters(100.);
        sampleRateTool.RunPostProcessForTest();
        const size_t coarseFaceCount =
            CountFbxFaces(sampleRateFbx / "plane.fbx");
        sampleRateTool.SetFbxTerrainSampleRateMeters(10.);
        sampleRateTool.RunPostProcessForTest();
        const size_t fineFaceCount =
            CountFbxFaces(sampleRateFbx / "plane.fbx");
        const std::vector<size_t> fineFacesByMesh =
            CountFbxFacesByMesh(sampleRateFbx / "plane.fbx");
        if (!postprocess_test::Require(
                fineFaceCount > coarseFaceCount,
                "降低高度采样间距没有增加可用网格细节") ||
            !postprocess_test::Require(
                fineFacesByMesh.size() > 1 &&
                    std::ranges::min(fineFacesByMesh) <
                        std::ranges::max(fineFacesByMesh),
                "不同物理范围的地形块仍使用相同面数") ||
            !postprocess_test::Require(
                postprocess_test::ContainsText(
                    sampleRateFbx / "manifest.json", "\"terrainsamplerate\":10"),
                "FBX 清单没有记录高度采样率") ||
            !RequireFbxNormalsUseEqualFaceWeights(
                sampleRateFbx / "plane.fbx"))
        {
            return false;
        }

        for (uint32_t budget : {2u, 31u, 128u})
        {
            sampleRateTool.SetFbxTerrainMaximumTriangles(budget);
            sampleRateTool.RunPostProcessForTest();
            const auto faces = CountFbxFacesByMesh(sampleRateFbx / "plane.fbx");
            if (!postprocess_test::Require(
                    !faces.empty() && std::ranges::max(faces) <= budget,
                    "每块地形面数超出预算") ||
                !RequireFbxTerrainSeamsClosed(sampleRateFbx / "plane.fbx") ||
                !postprocess_test::Require(
                    postprocess_test::ContainsText(sampleRateFbx / "manifest.json",
                        "\"terrainmaximumtriangles\":" + std::to_string(budget)),
                    "FBX 清单没有记录地形面数预算"))
            {
                return false;
            }
        }

        if (!ExportDistanceLodFixture(testRoot) ||
            !RequireFbxTerrainSeamsClosed(testRoot / "distance-lod" / "FBX" / "plane.fbx"))
        {
            return false;
        }

        std::cout << "PostProcessFbxTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
