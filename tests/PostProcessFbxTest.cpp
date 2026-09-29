#include "PostProcessTestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
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

    bool RunTest()
    {
        const std::filesystem::path testRoot =
            postprocess_test::MakeTestRoot("fbx");
        if (!postprocess_test::ResetDirectory(testRoot))
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
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);
        tool.SetFbxMeshSubdivision(8);
        tool.SetMaxDownQueue(2);

        if (!postprocess_test::WriteSourceTiles(tool))
        {
            return false;
        }

        tool.RunPostProcessForTest();

        const std::filesystem::path planeFbxPath = fbxRoot / "plane.fbx";
        const std::filesystem::path sphereFbxPath = fbxRoot / "sphere.fbx";

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
            !postprocess_test::RequireNonEmptyFile(sphereFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(planeFbxPath) ||
            !postprocess_test::RequireFbxAxisAndUnit(sphereFbxPath) ||
            !postprocess_test::RequirePlaneFbxUvMatchesNorthAxis(planeFbxPath) ||
            !RequireFbxTexturesBesideModel(planeFbxPath, fbxRoot) ||
            !RequireFbxTexturesBesideModel(sphereFbxPath, fbxRoot) ||
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
        tool.SetFbxShape(DownTool::FbxPlanetShape::Both);

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

        std::cout << "PostProcessFbxTest passed\n";
        return true;
    }
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
