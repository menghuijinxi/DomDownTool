#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "assimp/Importer.hpp"
#include "assimp/material.h"
#include "assimp/scene.h"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

#include "AmapBuildingFbxExporter.h"
#include "AmapVectorPngDownloader.h"
#include "GCJ02GoogleDownBase.h"
#include "PostProcessTestSupport.h"

namespace
{
    bool IsNearColor(uint8_t actual, uint8_t expected)
    {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 2;
    }

    bool Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    bool HasColor(
        const cv::Mat& image,
        uint8_t red,
        uint8_t green,
        uint8_t blue)
    {
        for (int row = 0; row < image.rows; ++row)
        {
            for (int column = 0; column < image.cols; ++column)
            {
                const cv::Vec4b pixel = image.at<cv::Vec4b>(row, column);
                if (IsNearColor(pixel[0], blue) &&
                    IsNearColor(pixel[1], green) &&
                    IsNearColor(pixel[2], red) && pixel[3] != 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool IsPowerOfTwo(int value)
    {
        return value > 0 && (value & (value - 1)) == 0;
    }

    bool RequirePowerOfTwoTexture(const std::filesystem::path& path)
    {
        const cv::Mat image = cv::imread(
            postprocess_test::PathToUtf8String(path),
            cv::IMREAD_COLOR);
        return Require(!image.empty(),
                "建筑贴图无法读取：" + postprocess_test::PathToUtf8String(path)) &&
            Require(IsPowerOfTwo(image.cols) && IsPowerOfTwo(image.rows),
                "建筑贴图宽高必须都是 2 的次幂：" +
                    postprocess_test::PathToUtf8String(path));
    }

    bool RequireTextureSize(const std::filesystem::path& path, int width, int height)
    {
        const cv::Mat image = cv::imread(
            postprocess_test::PathToUtf8String(path),
            cv::IMREAD_COLOR);
        return Require(!image.empty(),
                "建筑贴图无法读取：" + postprocess_test::PathToUtf8String(path)) &&
            Require(image.cols == width && image.rows == height,
                "建筑贴图尺寸不符合预期：" + postprocess_test::PathToUtf8String(path));
    }

    bool RequireResizedTextureUsesFullCanvas(const std::filesystem::path& path)
    {
        const cv::Mat image = cv::imread(
            postprocess_test::PathToUtf8String(path),
            cv::IMREAD_COLOR);
        if (!Require(!image.empty(),
                "建筑贴图无法读取：" + postprocess_test::PathToUtf8String(path)))
        {
            return false;
        }

        const int leftColumn = std::max(0, image.cols * 3 / 4);
        const int rightColumn = image.cols - 1;
        const int centerRow = image.rows / 2;
        const int blueDelta = std::abs(
            static_cast<int>(image.at<cv::Vec3b>(centerRow, rightColumn)[0]) -
            static_cast<int>(image.at<cv::Vec3b>(centerRow, leftColumn)[0]));
        const int upperRow = std::max(0, image.rows * 3 / 4);
        const int bottomRow = image.rows - 1;
        const int centerColumn = image.cols / 2;
        const int greenDelta = std::abs(
            static_cast<int>(image.at<cv::Vec3b>(bottomRow, centerColumn)[1]) -
            static_cast<int>(image.at<cv::Vec3b>(upperRow, centerColumn)[1]));
        return Require(blueDelta > 20 && greenDelta > 20,
            "建筑散图贴图疑似补空白/复制边缘，而不是重采样拉伸到 2 的次幂：" +
                postprocess_test::PathToUtf8String(path));
    }

    bool IsVisibleAtlasSample(const cv::Mat& image, const aiVector3D& uv)
    {
        if (image.empty())
        {
            return false;
        }
        const int column = std::clamp(
            static_cast<int>(std::lround(uv.x * static_cast<float>(image.cols - 1))),
            0,
            image.cols - 1);
        const int row = std::clamp(
            static_cast<int>(std::lround((1.f - uv.y) * static_cast<float>(image.rows - 1))),
            0,
            image.rows - 1);
        const cv::Vec3b pixel = image.at<cv::Vec3b>(row, column);
        return static_cast<int>(pixel[0]) + static_cast<int>(pixel[1]) +
            static_cast<int>(pixel[2]) > 16;
    }

    float RepeatAtlasCoordinate(float value)
    {
        const float repeated = value - std::floor(value);
        return repeated < 0.00001f && value > 0.00001f ? 1.f : repeated;
    }

    aiVector3D MakeAtlasSampleUv(
        const aiVector3D& repeatUv,
        const aiVector3D& atlasOffset,
        const aiVector3D& atlasMax)
    {
        const float repeatedU = RepeatAtlasCoordinate(repeatUv.x);
        const float repeatedV = RepeatAtlasCoordinate(repeatUv.y);
        return aiVector3D(
            atlasOffset.x + (atlasMax.x - atlasOffset.x) * repeatedU,
            atlasOffset.y + (atlasMax.y - atlasOffset.y) * repeatedV,
            0.f);
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

    std::optional<float> MinimumFbxVertexZ(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (scene == nullptr)
        {
            return std::nullopt;
        }

        std::optional<float> minimumZ;
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
                const float z = mesh->mVertices[vertexIndex].z;
                minimumZ = minimumZ.has_value() ? std::min(*minimumZ, z) : z;
            }
        }
        return minimumZ;
    }

    bool TryGetPixelAtGcj02(
        const cv::Mat& image,
        const DownTool::LonLat& coordinate,
        uint32_t zoom,
        const DownTool::Tile& expectedTile,
        cv::Vec4b& pixel)
    {
        const DownTool::LonLat wgs84Coordinate =
            DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(coordinate);
        DownTool::GoogleDownBase projection;
        DownTool::Tile tile;
        double pixelX = 0.;
        double pixelY = 0.;
        if (!projection.TryGetTilePixelForLonLat(
                wgs84Coordinate, zoom, tile, pixelX, pixelY) ||
            tile.X != expectedTile.X || tile.Y != expectedTile.Y ||
            pixelX < 0. || pixelY < 0. ||
            pixelX >= static_cast<double>(image.cols) ||
            pixelY >= static_cast<double>(image.rows))
        {
            return false;
        }
        pixel = image.at<cv::Vec4b>(
            static_cast<int>(pixelY), static_cast<int>(pixelX));
        return pixel[3] != 0;
    }

    bool SameRgb(const cv::Vec4b& first, const cv::Vec4b& second)
    {
        return first[0] == second[0] && first[1] == second[1] &&
            first[2] == second[2];
    }

    bool WriteJpegTextureWithoutExtension(
        const std::filesystem::path& path,
        int width,
        int height,
        const cv::Scalar& color)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            return false;
        }

        cv::Mat image(height, width, CV_8UC3, color);
        std::vector<unsigned char> encodedImage;
        if (!cv::imencode(".jpg", image, encodedImage) || encodedImage.empty())
        {
            return false;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(encodedImage.data()),
            static_cast<std::streamsize>(encodedImage.size()));
        return output.good();
    }

    bool WriteJpegTexture(
        const std::filesystem::path& path,
        int width,
        int height,
        const cv::Scalar& color)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            return false;
        }

        cv::Mat image(height, width, CV_8UC3, color);
        std::vector<unsigned char> encodedImage;
        if (!cv::imencode(".jpg", image, encodedImage) || encodedImage.empty())
        {
            return false;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(encodedImage.data()),
            static_cast<std::streamsize>(encodedImage.size()));
        return output.good();
    }

    bool WriteGradientJpegTexture(
        const std::filesystem::path& path,
        int width,
        int height)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            return false;
        }

        cv::Mat image(height, width, CV_8UC3);
        for (int row = 0; row < height; row++)
        {
            for (int column = 0; column < width; column++)
            {
                image.at<cv::Vec3b>(row, column) = cv::Vec3b(
                    static_cast<uint8_t>(column * 255 / std::max(1, width - 1)),
                    static_cast<uint8_t>(row * 255 / std::max(1, height - 1)),
                    80);
            }
        }

        std::vector<unsigned char> encodedImage;
        if (!cv::imencode(".jpg", image, encodedImage) || encodedImage.empty())
        {
            return false;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(encodedImage.data()),
            static_cast<std::streamsize>(encodedImage.size()));
        return output.good();
    }

    bool RequireBuildingFbx(
        const std::filesystem::path& fbxPath,
        const std::filesystem::path& fbxRoot,
        double minimumExpectedZ = 0.,
        double minimumExpectedHeightRange = 0.,
        bool expectMergedAtlas = false,
        const std::string& atlasExtension = ".png")
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!Require(scene != nullptr, "建筑 FBX 无法读取") ||
            !Require(scene->HasMeshes(), "建筑 FBX 没有网格") ||
            !Require(scene->mNumMaterials >= 2, "建筑 FBX 没有墙体和屋顶材质"))
        {
            return false;
        }
        if (expectMergedAtlas &&
            !Require(scene->mNumMaterials == 2, "建筑合并贴图 FBX 应只有墙体和屋顶两个材质"))
        {
            return false;
        }

        bool foundPositiveHeight = false;
        bool foundDiffuseTexture = false;
        bool foundRepeatingWallUv = false;
        bool foundAtlasWallUv = false;
        cv::Mat wallAtlasImage;
        size_t atlasSampleCount = 0;
        size_t visibleAtlasSampleCount = 0;
        float sceneMinimumZ = std::numeric_limits<float>::max();
        float sceneMaximumZ = std::numeric_limits<float>::lowest();
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
                foundPositiveHeight = foundPositiveHeight || mesh->mVertices[vertexIndex].z > 1.f;
                sceneMinimumZ = std::min(sceneMinimumZ, mesh->mVertices[vertexIndex].z);
                sceneMaximumZ = std::max(sceneMaximumZ, mesh->mVertices[vertexIndex].z);
            }

            if (!mesh->HasTextureCoords(0) || mesh->mMaterialIndex >= scene->mNumMaterials)
            {
                continue;
            }
            aiString meshTexturePath;
            if (scene->mMaterials[mesh->mMaterialIndex]->GetTexture(
                    aiTextureType_DIFFUSE,
                    0,
                    &meshTexturePath) != AI_SUCCESS ||
                !std::string(meshTexturePath.C_Str()).starts_with("building_wall_"))
            {
                continue;
            }

            const bool hasAtlasTransformUv = mesh->HasTextureCoords(1) &&
                mesh->HasTextureCoords(2);
            if (expectMergedAtlas &&
                !Require(hasAtlasTransformUv,
                    "建筑合并贴图 FBX 墙体缺少 UV1/UV2 atlas 变换通道"))
            {
                return false;
            }

            float minimumV = std::numeric_limits<float>::max();
            float maximumV = std::numeric_limits<float>::lowest();
            float maximumU = 0.f;
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                 vertexIndex++)
            {
                const aiVector3D& uv = mesh->mTextureCoords[0][vertexIndex];
                maximumU = std::max(maximumU, uv.x);
                minimumV = std::min(minimumV, uv.y);
                maximumV = std::max(maximumV, uv.y);
                if (expectMergedAtlas)
                {
                    const aiVector3D& atlasOffset = mesh->mTextureCoords[1][vertexIndex];
                    const aiVector3D& atlasMax = mesh->mTextureCoords[2][vertexIndex];
                    const aiVector3D atlasUv = MakeAtlasSampleUv(
                        uv, atlasOffset, atlasMax);
                    foundAtlasWallUv = foundAtlasWallUv ||
                        (atlasOffset.x >= -0.001f && atlasOffset.y >= -0.001f &&
                         atlasMax.x > atlasOffset.x && atlasMax.y > atlasOffset.y &&
                         atlasMax.x <= 1.001f && atlasMax.y <= 1.001f);
                    if (wallAtlasImage.empty())
                    {
                        wallAtlasImage = cv::imread(
                            postprocess_test::PathToUtf8String(
                                fbxRoot / std::filesystem::u8path(meshTexturePath.C_Str())),
                            cv::IMREAD_COLOR);
                        if (!Require(!wallAtlasImage.empty(),
                                "建筑合并贴图 FBX 墙体 atlas 无法读取"))
                        {
                            return false;
                        }
                    }
                    atlasSampleCount++;
                    if (IsVisibleAtlasSample(wallAtlasImage, atlasUv))
                    {
                        visibleAtlasSampleCount++;
                    }
                }
            }
            foundRepeatingWallUv = foundRepeatingWallUv ||
                (maximumU > 1.1f && minimumV <= 0.01f && maximumV >= 0.99f);
            if (!Require(maximumV <= 1.001f, "建筑墙体 UV 不应向下超过贴图底边"))
            {
                return false;
            }
        }

        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials;
             materialIndex++)
        {
            aiString texturePath;
            if (scene->mMaterials[materialIndex]->GetTexture(
                    aiTextureType_DIFFUSE,
                    0,
                    &texturePath) != AI_SUCCESS)
            {
                continue;
            }
            foundDiffuseTexture = true;
            const std::filesystem::path relativeTexturePath =
                std::filesystem::u8path(texturePath.C_Str());
            if (!Require(!relativeTexturePath.has_parent_path(),
                    std::string("建筑 FBX 贴图路径不是平铺相对路径：") +
                        texturePath.C_Str()) ||
                !Require(std::filesystem::exists(fbxRoot / relativeTexturePath),
                    std::string("建筑 FBX 贴图缺失：") + texturePath.C_Str()))
            {
                return false;
            }
        }

        const aiMesh* firstMesh = scene->mMeshes[0];
        if (!Require(firstMesh != nullptr && firstMesh->mMaterialIndex < scene->mNumMaterials,
                "建筑 FBX 第一个 mesh 没有关联材质"))
        {
            return false;
        }
        aiString firstTexturePath;
        const aiMaterial* firstMaterial = scene->mMaterials[firstMesh->mMaterialIndex];
        if (!Require(firstMaterial != nullptr &&
                    firstMaterial->GetTexture(
                        aiTextureType_DIFFUSE, 0, &firstTexturePath) == AI_SUCCESS,
                "建筑 FBX 第一个 mesh 没有关联 diffuse 贴图") ||
            !Require(std::string(firstTexturePath.C_Str()).starts_with("building_wall_"),
                "建筑 FBX 应优先输出墙体 mesh，便于低角度检查立面"))
        {
            return false;
        }

        if (expectMergedAtlas &&
            (!Require(atlasSampleCount > 0, "建筑合并贴图 FBX 没有可检查的墙体 UV") ||
             !Require(atlasSampleCount == visibleAtlasSampleCount,
                 "建筑合并贴图 FBX 墙体 UV 会采样到 atlas 空白区")))
        {
            return false;
        }

        return Require(foundPositiveHeight, "建筑 FBX 没有正高度顶点") &&
            Require(sceneMinimumZ >= minimumExpectedZ,
                "建筑 FBX 没有按地形最低点抬高底部") &&
            Require(sceneMaximumZ - sceneMinimumZ >= minimumExpectedHeightRange,
                "建筑 FBX 没有按地形最高点加建筑高度生成顶部") &&
            Require(foundDiffuseTexture, "建筑 FBX 没有 diffuse 贴图") &&
            Require(foundRepeatingWallUv,
                expectMergedAtlas ?
                    "建筑合并贴图 FBX 墙体 UV0 没有保留横向重复坐标" :
                    "建筑 FBX 墙体 UV 没有横向重复或纵向完整映射") &&
            (!expectMergedAtlas || Require(
                foundAtlasWallUv,
                "建筑合并贴图 FBX 墙体 UV1/UV2 没有提供有效 atlas 变换")) &&
            Require(std::filesystem::exists(
                    fbxRoot / (expectMergedAtlas ?
                        ("building_wall_atlas" + atlasExtension) :
                        "building_wall_0.jpg")),
                expectMergedAtlas ? "建筑 FBX 没有生成墙体合并贴图" :
                    "建筑 FBX 没有复制无后缀墙体 JPEG") &&
            Require(std::filesystem::exists(
                    fbxRoot / (expectMergedAtlas ?
                        ("building_roof_atlas" + atlasExtension) :
                        "building_roof_0.jpg")),
                expectMergedAtlas ? "建筑 FBX 没有生成屋顶合并贴图" :
                    "建筑 FBX 没有复制屋顶 JPEG") &&
            (!expectMergedAtlas ||
              (RequirePowerOfTwoTexture(
                   fbxRoot / ("building_wall_atlas" + atlasExtension)) &&
               RequirePowerOfTwoTexture(
                   fbxRoot / ("building_roof_atlas" + atlasExtension))));
    }

    bool RequireScatteredBuildingOriginalUv0Only(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!Require(scene != nullptr, "建筑散图 FBX 无法读取"))
        {
            return false;
        }

        bool foundWallUv = false;
        bool foundRoofUv = false;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr || !mesh->HasTextureCoords(0) ||
                mesh->mMaterialIndex >= scene->mNumMaterials)
            {
                continue;
            }

            aiString texturePath;
            if (scene->mMaterials[mesh->mMaterialIndex]->GetTexture(
                    aiTextureType_DIFFUSE,
                    0,
                    &texturePath) != AI_SUCCESS)
            {
                continue;
            }

            float minimumU = std::numeric_limits<float>::max();
            float maximumU = std::numeric_limits<float>::lowest();
            float minimumV = std::numeric_limits<float>::max();
            float maximumV = std::numeric_limits<float>::lowest();
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                 vertexIndex++)
            {
                const aiVector3D& uv = mesh->mTextureCoords[0][vertexIndex];
                minimumU = std::min(minimumU, uv.x);
                maximumU = std::max(maximumU, uv.x);
                minimumV = std::min(minimumV, uv.y);
                maximumV = std::max(maximumV, uv.y);
            }

            const std::string textureName = texturePath.C_Str();
            if (textureName == "building_wall_0.jpg")
            {
                foundWallUv = true;
                if (!Require(!mesh->HasTextureCoords(1) && !mesh->HasTextureCoords(2),
                        "散图墙体不应写入 UV1/UV2 atlas 通道") ||
                    !Require(maximumU > 1.1f,
                        "散图墙体 UV0 没有保留按原始贴图比例计算的横向重复") ||
                    !Require(minimumV <= 0.01f && maximumV >= 0.99f,
                        "散图墙体 UV0 没有铺满完整贴图高度"))
                {
                    return false;
                }
            }
            else if (textureName == "building_roof_0.jpg")
            {
                foundRoofUv = true;
                if (!Require(!mesh->HasTextureCoords(1) && !mesh->HasTextureCoords(2),
                        "散图屋顶不应写入 UV1/UV2 atlas 通道") ||
                    !Require(minimumU <= 0.01f && maximumU >= 0.99f,
                        "散图屋顶 UV0 没有铺满完整贴图宽度") ||
                    !Require(minimumV <= 0.01f && maximumV >= 0.99f,
                        "散图屋顶 UV0 没有铺满完整贴图高度"))
                {
                    return false;
                }
            }
        }
        return Require(foundWallUv, "散图墙体 FBX 缺少可检查 UV") &&
            Require(foundRoofUv, "散图屋顶 FBX 缺少可检查 UV");
    }

    bool RequireSatelliteRoofFbx(
        const std::filesystem::path& fbxPath,
        size_t minimumSatelliteRoofMaterialCount)
    {
        Assimp::Importer importer;
        const aiScene* scene = postprocess_test::ReadFbxScene(importer, fbxPath);
        if (!Require(scene != nullptr, "卫星屋顶建筑 FBX 无法读取") ||
            !Require(scene->HasMeshes(), "卫星屋顶建筑 FBX 没有网格"))
        {
            return false;
        }

        size_t satelliteRoofMaterialCount = 0;
        bool foundTerrainAlignedRoofUv = false;
        for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials;
             materialIndex++)
        {
            const aiMaterial* material = scene->mMaterials[materialIndex];
            if (material == nullptr)
            {
                continue;
            }
            aiString materialName;
            aiString texturePath;
            if (material->Get(AI_MATKEY_NAME, materialName) == AI_SUCCESS &&
                material->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath) == AI_SUCCESS &&
                std::string(materialName.C_Str()).starts_with("building_roof_satellite_"))
            {
                if (std::string(texturePath.C_Str()).find("building_roof_satellite_") !=
                    std::string::npos)
                {
                    std::cerr << "卫星屋顶材质不应引用额外复制贴图："
                              << texturePath.C_Str() << '\n';
                    return false;
                }
                satelliteRoofMaterialCount++;
            }
        }
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr || mesh->mMaterialIndex >= scene->mNumMaterials ||
                mesh->mTextureCoords[0] == nullptr)
            {
                continue;
            }
            const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            aiString materialName;
            if (material == nullptr || material->Get(AI_MATKEY_NAME, materialName) !=
                    AI_SUCCESS ||
                !std::string(materialName.C_Str()).starts_with("building_roof_satellite_"))
            {
                continue;
            }

            double minimumX = std::numeric_limits<double>::max();
            double maximumX = std::numeric_limits<double>::lowest();
            double minimumY = std::numeric_limits<double>::max();
            double maximumY = std::numeric_limits<double>::lowest();
            double minimumXU = 0.;
            double maximumXU = 0.;
            double minimumYV = 0.;
            double maximumYV = 0.;
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                 vertexIndex++)
            {
                const double x = mesh->mVertices[vertexIndex].x;
                const double y = mesh->mVertices[vertexIndex].y;
                const double u = mesh->mTextureCoords[0][vertexIndex].x;
                const double v = mesh->mTextureCoords[0][vertexIndex].y;
                if (!Require(
                        u >= -1.e-5 && u <= 1. + 1.e-5 &&
                            v >= -1.e-5 && v <= 1. + 1.e-5,
                        "卫星屋顶 UV 超出卫星贴图范围"))
                {
                    return false;
                }
                if (x < minimumX)
                {
                    minimumX = x;
                    minimumXU = u;
                }
                if (x > maximumX)
                {
                    maximumX = x;
                    maximumXU = u;
                }
                if (y < minimumY)
                {
                    minimumY = y;
                    minimumYV = v;
                }
                if (y > maximumY)
                {
                    maximumY = y;
                    maximumYV = v;
                }
            }
            if (maximumX > minimumX + 1.e-4 && maximumXU > minimumXU + 1.e-5 &&
                maximumY > minimumY + 1.e-4 && maximumYV > minimumYV + 1.e-5)
            {
                foundTerrainAlignedRoofUv = true;
            }
        }
        return Require(
            satelliteRoofMaterialCount >= minimumSatelliteRoofMaterialCount,
            "卫星屋顶 FBX 没有按卫星图范围拆分屋顶 mesh") &&
            Require(
                foundTerrainAlignedRoofUv,
                "卫星屋顶 UV 方向没有和地形 FBX 保持一致");
    }

    DownTool::AmapVectorFeature MakePolygon(
        const std::string& layerName,
        const DownTool::LonLat& center,
        double halfWidth,
        double halfHeight,
        uint32_t mainKey)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::Polygon;
        feature.layerName = layerName;
        feature.mainKey = mainKey;
        feature.minZoom = 1;
        feature.maxZoom = 20;
        feature.geometryParts.push_back({
            { center.lon - halfWidth, center.lat - halfHeight },
            { center.lon + halfWidth, center.lat - halfHeight },
            { center.lon + halfWidth, center.lat + halfHeight },
            { center.lon - halfWidth, center.lat + halfHeight },
            { center.lon - halfWidth, center.lat - halfHeight }});
        return feature;
    }

    DownTool::AmapVectorFeature MakeHeightBuilding(
        const DownTool::LonLat& center,
        uint64_t height,
        bool hasHeight,
        uint32_t subKey)
    {
        DownTool::AmapVectorFeature feature = MakePolygon(
            "building", center, 0.0007, 0.00035, 200);
        feature.subKey = subKey;
        feature.hasHeight = hasHeight;
        feature.height = height;
        return feature;
    }

    DownTool::AmapVectorFeature MakeLine(
        const DownTool::LonLat& center,
        double halfWidth,
        uint32_t mainKey)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::LineString;
        feature.layerName = "road";
        feature.mainKey = mainKey;
        feature.minZoom = 1;
        feature.maxZoom = 20;
        feature.geometryParts.push_back({
            { center.lon - halfWidth, center.lat },
            { center.lon + halfWidth, center.lat }});
        return feature;
    }

    DownTool::AmapVectorFeature MakeWaterLine(
        const DownTool::LonLat& center,
        double latitudeOffset,
        uint32_t subKey,
        uint32_t minimumZoom)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::LineString;
        feature.layerName = "region-line";
        feature.mainKey = 20014;
        feature.subKey = subKey;
        feature.minZoom = minimumZoom;
        feature.maxZoom = 30;
        feature.geometryParts.push_back({
            { center.lon - 0.0015, center.lat + latitudeOffset },
            { center.lon, center.lat + latitudeOffset + 0.0002 },
            { center.lon + 0.0015, center.lat + latitudeOffset }});
        return feature;
    }

    DownTool::AmapVectorFeature MakePoint(const DownTool::LonLat& center)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::Point;
        feature.layerName = "poi";
        feature.minZoom = 1;
        feature.maxZoom = 20;
        feature.geometryParts.push_back({ { center.lon, center.lat } });
        return feature;
    }

    bool RunTest()
    {
        const DownTool::AmapVectorDownloadOptions defaultVectorOptions;
        const DownTool::AmapVectorRenderOptions defaultRenderOptions;
        if (!Require(
            defaultVectorOptions.rawTileCachePath ==
                std::filesystem::u8path("cache/高德地图/gis数据/pbf缓存"),
            "默认 PBF 缓存目录错误") ||
            !Require(
                defaultRenderOptions.pngTileCachePath ==
                    std::filesystem::u8path("cache/高德地图/gis数据/临时"),
                "默认 PNG 临时目录错误"))
        {
            return false;
        }

        const std::filesystem::path outputRoot =
            std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) / "tmp" / "amap-vector-png-tests";
        std::error_code removeError;
        std::filesystem::remove_all(outputRoot, removeError);
        if (!Require(!removeError, "无法清理高德 GIS PNG 测试目录"))
        {
            return false;
        }

        const DownTool::LonLat centerWgs84(116.0500, 39.9760);
        const DownTool::LonLat centerGcj02 =
            DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(centerWgs84);
        DownTool::AmapVectorDecodedData decodedData;
        decodedData.status = true;
        DownTool::AmapVectorFeature waterFeature = MakePolygon(
            "region", centerGcj02, 0.002, 0.001, 30001);
        waterFeature.subKey = 2;
        waterFeature.minZoom = 15;
        decodedData.features.push_back(std::move(waterFeature));
        DownTool::AmapVectorFeature greenFeature = MakePolygon(
            "region",
            { centerGcj02.lon + 0.005, centerGcj02.lat },
            0.001,
            0.0005,
            30001);
        greenFeature.subKey = 3;
        decodedData.features.push_back(std::move(greenFeature));
        DownTool::AmapVectorFeature buildingFeature = MakePolygon(
            "building",
            { centerGcj02.lon, centerGcj02.lat - 0.002 },
            0.0004,
            0.00025,
            200);
        buildingFeature.minZoom = 15;
        decodedData.features.push_back(std::move(buildingFeature));
        decodedData.features.push_back(MakeLine(centerGcj02, 0.0015, 300));
        decodedData.features.push_back(MakeWaterLine(centerGcj02, 0.0006, 2, 10));
        decodedData.features.push_back(MakeWaterLine(centerGcj02, -0.0006, 1, 14));
        decodedData.features.push_back(MakePoint(centerGcj02));

        DownTool::AmapVectorPngDownloader downloader;
        DownTool::AmapVectorDownloadOptions vectorOptions;
        vectorOptions.centerWgs84 = centerWgs84;
        vectorOptions.radiusMeters = 100.;
        vectorOptions.zoom = 14;
        const DownTool::Tile tile =
            DownTool::GoogleDownBase::LonLatToTile(centerWgs84, vectorOptions.zoom);

        DownTool::AmapVectorRenderOptions renderOptions;
        renderOptions.pngTileCachePath = outputRoot / "png-cache";
        std::vector<DownTool::AmapVectorRenderProgress> renderProgress;
        renderOptions.progressCallback =
            [&](const DownTool::AmapVectorRenderProgress& progress)
            {
                renderProgress.push_back(progress);
            };

        DownTool::AmapVectorRenderOptions unicodeRenderOptions = renderOptions;
        unicodeRenderOptions.pngTileCachePath =
            outputRoot / std::filesystem::u8path("高德地图/临时");
        unicodeRenderOptions.progressCallback = {};
        const DownTool::AmapVectorPngRenderResult unicodeRenderResult =
            downloader.Render(decodedData, vectorOptions, unicodeRenderOptions);
        const std::filesystem::path unicodeTilePath =
            unicodeRenderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        if (!Require(
            unicodeRenderResult.success && std::filesystem::exists(unicodeTilePath),
            "中文临时目录无法写入高德 GIS PNG：" + unicodeRenderResult.error))
        {
            return false;
        }

        DownTool::AmapVectorPngRenderResult renderResult =
            downloader.Render(decodedData, vectorOptions, renderOptions);
        if (!Require(renderResult.success, "高德 GIS PNG 渲染失败：" + renderResult.error) ||
            !Require(renderResult.renderedTileCount == 1, "测试范围应渲染一张 PNG 瓦片") ||
            !Require(
                renderProgress.size() == 2 &&
                    renderProgress.front().completedTileCount == 0 &&
                    renderProgress.back().completedTileCount == 1 &&
                    renderProgress.back().failedTileCount == 0,
                "PNG 渲染进度错误"))
        {
            return false;
        }

        const std::filesystem::path tilePath =
            renderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat image = cv::imread(tilePath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(!image.empty() && image.channels() == 4, "PNG 瓦片不可读取") ||
            !Require(HasColor(image, 106, 142, 202), "缺少水体默认蓝色") ||
            !Require(HasColor(image, 83, 133, 78), "缺少绿地默认绿色") ||
            !Require(HasColor(image, 255, 218, 113), "缺少道路默认黄色"))
        {
            return false;
        }

#ifdef _WIN32
        // 锚点在左瓦片里，地名的文字应自然延伸到右瓦片。
        auto edgeOptions = vectorOptions;
        auto edgeRender = renderOptions;
        edgeRender.pngTileCachePath = outputRoot / "point-label-edge";
        const DownTool::Tile rightTile{tile.X + 1, tile.Y, tile.Zoom};
        const auto edge = DownTool::GoogleDownBase::TileToLonLat(rightTile);
        edgeOptions.centerWgs84 = {edge.lon - 360. / (256. * (1u << tile.Zoom)), centerWgs84.lat};
        edgeOptions.radiusMeters = 150.;
        auto named = MakePoint(edgeOptions.centerWgs84);
        named.SetCoordinateSystem(DownTool::VectorCoordinateSystem::Wgs84);
        named.names = {{"停车场", "zh"}};
        named.SetDataSource(DownTool::VectorDataSource::Tencent);
        named.SetSourceProperties({{"tag", "carpark"}});
        DownTool::AmapVectorDecodedData edgeData;
        edgeData.features.push_back(std::move(named));
        const auto edgeResult = downloader.Render(edgeData, edgeOptions, edgeRender);
        const auto rightPath = edgeRender.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(rightTile.X) / (std::to_string(rightTile.Y) + ".png");
        const auto rightImage = cv::imread(rightPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(edgeResult.success && !rightImage.empty() &&
                     HasColor(rightImage, 35, 39, 43) && HasColor(rightImage, 255, 255, 255),
                     "GIS 导出没有在相邻瓦片画出带描边的中文点名"))
        {
            return false;
        }
#endif
        DownTool::AmapVectorDecodedData heightData;
        heightData.status = true;
        const DownTool::LonLat sameHeightCenterA =
            { centerGcj02.lon - 0.004, centerGcj02.lat - 0.001 };
        const DownTool::LonLat sameHeightCenterB =
            { centerGcj02.lon + 0.004, centerGcj02.lat - 0.001 };
        const DownTool::LonLat differentHeightCenter =
            { centerGcj02.lon, centerGcj02.lat - 0.003 };
        const DownTool::LonLat zeroHeightCenter =
            { centerGcj02.lon - 0.003, centerGcj02.lat - 0.005 };
        const DownTool::LonLat missingHeightCenter =
            { centerGcj02.lon + 0.003, centerGcj02.lat - 0.005 };
        heightData.features.push_back(
            MakeHeightBuilding(sameHeightCenterA, 18, true, 1));
        heightData.features.push_back(
            MakeHeightBuilding(sameHeightCenterB, 18, true, 2));
        heightData.features.push_back(
            MakeHeightBuilding(differentHeightCenter, 30, true, 3));
        heightData.features.push_back(
            MakeHeightBuilding(zeroHeightCenter, 0, true, 4));
        heightData.features.push_back(
            MakeHeightBuilding(missingHeightCenter, 30, false, 5));

        DownTool::AmapVectorRenderOptions heightRenderOptions;
        heightRenderOptions.categories = { DownTool::AmapVectorRenderCategory::Building };
        heightRenderOptions.ignoreFeatureZoomRange = true;
        heightRenderOptions.colorBuildingsByHeight = true;
        heightRenderOptions.pngTileCachePath = outputRoot / "height-colors";
        const DownTool::AmapVectorPngRenderResult heightRenderResult =
            downloader.Render(heightData, vectorOptions, heightRenderOptions);
        const std::filesystem::path heightTilePath =
            heightRenderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat heightImage = cv::imread(heightTilePath.string(), cv::IMREAD_UNCHANGED);
        cv::Vec4b sameHeightPixelA;
        cv::Vec4b sameHeightPixelB;
        cv::Vec4b differentHeightPixel;
        cv::Vec4b zeroHeightPixel;
        cv::Vec4b missingHeightPixel;
        if (!Require(heightRenderResult.success, "建筑高度颜色渲染失败：" + heightRenderResult.error) ||
            !Require(!heightImage.empty() && heightImage.channels() == 4,
                "建筑高度颜色 PNG 不可读取") ||
            !Require(TryGetPixelAtGcj02(
                heightImage, sameHeightCenterA, vectorOptions.zoom, tile, sameHeightPixelA),
                "无法读取第一个同高建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage, sameHeightCenterB, vectorOptions.zoom, tile, sameHeightPixelB),
                "无法读取第二个同高建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage,
                differentHeightCenter,
                vectorOptions.zoom,
                tile,
                differentHeightPixel),
                "无法读取异高建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage, zeroHeightCenter, vectorOptions.zoom, tile, zeroHeightPixel),
                "无法读取零高度建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage,
                missingHeightCenter,
                vectorOptions.zoom,
                tile,
                missingHeightPixel),
                "无法读取无高度属性建筑像素") ||
            !Require(SameRgb(sameHeightPixelA, sameHeightPixelB),
                "相同高度建筑没有保持相同颜色") ||
            !Require(!SameRgb(sameHeightPixelA, differentHeightPixel),
                "不同高度建筑没有使用不同颜色") ||
            !Require(
                zeroHeightPixel[0] == 255 && zeroHeightPixel[1] == 255 &&
                    zeroHeightPixel[2] == 255,
                "零高度建筑没有沿用普通建筑颜色") ||
            !Require(
                missingHeightPixel[0] == 255 && missingHeightPixel[1] == 255 &&
                    missingHeightPixel[2] == 255,
                "无高度属性建筑没有沿用普通建筑颜色"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Water };
        renderOptions.colors[DownTool::AmapVectorRenderCategory::Water] = { 10, 20, 30, 255 };
        renderOptions.featureCodeColors[{ 30001, 3 }] = { 250, 10, 240, 255 };
        renderOptions.featureCodeColors[{ 20014, 2 }] = { 70, 80, 90, 255 };
        renderOptions.featureCodeColors[{ 20014, 1 }] = { 100, 110, 120, 255 };
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const cv::Mat waterOnly = cv::imread(tilePath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success, "水体类别渲染失败：" + renderResult.error) ||
            !Require(!HasColor(waterOnly, 10, 20, 30),
                "z14 rendered water whose minZoom is 15") ||
            !Require(HasColor(waterOnly, 70, 80, 90), "missing visible water line") ||
            !Require(HasColor(waterOnly, 100, 110, 120), "missing visible water line") ||
            !Require(!HasColor(waterOnly, 250, 10, 240), "水体筛选后仍包含绿地"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Building };
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        if (!Require(renderResult.success, "建筑类别渲染失败：" + renderResult.error))
        {
            return false;
        }
        const std::filesystem::path buildingOnlyPath =
            renderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat buildingOnly =
            cv::imread(buildingOnlyPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(!HasColor(buildingOnly, 255, 255, 255),
            "z14 rendered a building whose minZoom is 15"))
        {
            return false;
        }

        renderOptions.ignoreFeatureZoomRange = true;
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const cv::Mat forcedBuilding =
            cv::imread(buildingOnlyPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success, "forcing all feature zooms failed") ||
            !Require(HasColor(forcedBuilding, 255, 255, 255),
                "forcing all feature zooms did not render the building") ||
            !Require(!HasColor(forcedBuilding, 106, 142, 202),
                "the building category rendered water"))
        {
            return false;
        }

        cv::Mat staleImage(256, 256, CV_8UC4, cv::Scalar(13, 17, 251, 255));
        if (!Require(cv::imwrite(buildingOnlyPath.string(), staleImage),
            "无法写入临时 PNG 测试数据"))
        {
            return false;
        }

        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const cv::Mat rebuiltBuildingOnly =
            cv::imread(buildingOnlyPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success && renderResult.renderedTileCount == 1,
            "临时 PNG 没有重新渲染") ||
            !Require(HasColor(rebuiltBuildingOnly, 255, 255, 255),
                "临时 PNG 仍然保留旧内容"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Road };
        renderOptions.featureCodeColors[{ 300, 0 }] = { 10, 20, 30, 255 };
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const std::filesystem::path customRoadPath =
            renderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat customRoad = cv::imread(customRoadPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success, "道路自定义颜色渲染失败：" + renderResult.error) ||
            !Require(HasColor(customRoad, 10, 20, 30), "主/子分类自定义颜色没有生效"))
        {
            return false;
        }

        DownTool::AmapVectorDownloadOptions invalidVectorOptions = vectorOptions;
        invalidVectorOptions.radiusMeters = -1.;
        const DownTool::AmapVectorPngRenderResult invalidRender =
            downloader.Render(decodedData, invalidVectorOptions, renderOptions);
        if (!Require(!invalidRender.success, "负数半径必须被 PNG 渲染入口拒绝"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Building };
        renderOptions.featureCodeColors.clear();

        const std::filesystem::path postProcessRoot = outputRoot / "postprocess";
        const std::filesystem::path xyzRoot = postProcessRoot / "XYZ";
        const std::filesystem::path fbxRoot = postProcessRoot / "FBX";
        const std::filesystem::path textureRoot = outputRoot / std::filesystem::u8path("贴图");
        if (!Require(WriteGradientJpegTexture(
                textureRoot / std::filesystem::u8path("墙体") /
                    "wall_texture_without_suffix",
                96,
                128),
                "无法写入无后缀墙体 JPEG 测试贴图") ||
            !Require(WriteGradientJpegTexture(
                textureRoot / std::filesystem::u8path("房顶") / "roof_texture.jpg",
                160,
                96),
                "无法写入屋顶 JPEG 测试贴图"))
        {
            return false;
        }
        renderOptions.buildingWallTexturePath = textureRoot / std::filesystem::u8path("墙体");
        renderOptions.buildingRoofTexturePath = textureRoot / std::filesystem::u8path("房顶");
        renderOptions.exportBuildingsToFbx = true;
        renderOptions.buildingTextureRandomSeed = 1234;
        downloader.SetPostProcessOutputPath(xyzRoot.string());
        downloader.SetPostProcessEnabled(true);
        downloader.SetPostProcessMergedImageEnabled(true);
        downloader.SetPostProcessMergedImageResolution(256);
        downloader.SetPostProcessMergedImageOutputPath(
            (postProcessRoot / "merged.png").string());
        downloader.SetPostProcessFbxEnabled(true);
        downloader.SetFbxOutputPath(fbxRoot.string());
        downloader.SetFbxTextureSize(256);
        downloader.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        downloader.SetFbxMeshSubdivision(2);
        downloader.SetFbxTerrainHeightEnabled(true);
        downloader.SetFbxElevationReferenceMeters(100.);
        downloader.SetFbxElevationSampler(
            [centerWgs84](const DownTool::LonLat& lonLat)
            {
                return std::optional<double>(
                    100. + (lonLat.lon - centerWgs84.lon) * 10000.);
            });
        DownTool::AmapVectorDecodedData fbxData = decodedData;
        fbxData.features.push_back(
            MakeHeightBuilding({ centerGcj02.lon + 0.0015, centerGcj02.lat - 0.0015 },
                20,
                true,
                9));
        renderResult = downloader.Render(fbxData, vectorOptions, renderOptions);
        if (!Require(renderResult.success, "PNG 后处理渲染失败：" + renderResult.error) ||
            !Require(std::filesystem::exists(
                xyzRoot / std::to_string(tile.Zoom) / std::to_string(tile.X) /
                (std::to_string(tile.Y) + ".png")),
                "后处理金字塔缺少最高层级瓦片") ||
            !Require(std::filesystem::exists(postProcessRoot / "merged.png"),
                "后处理缺少合并大图") ||
            !Require(std::filesystem::exists(
                fbxRoot / "plane.fbx"),
                "后处理缺少 FBX") ||
            !Require(std::filesystem::exists(
                fbxRoot / "buildings.fbx"),
                "后处理缺少建筑 FBX") ||
            !RequireTextureSize(fbxRoot / "building_wall_0.jpg", 128, 128) ||
            !RequireTextureSize(fbxRoot / "building_roof_0.jpg", 256, 128) ||
            !RequireResizedTextureUsesFullCanvas(fbxRoot / "building_wall_0.jpg") ||
            !RequireResizedTextureUsesFullCanvas(fbxRoot / "building_roof_0.jpg") ||
            !postprocess_test::RequireFbxAxisAndUnit(fbxRoot / "buildings.fbx") ||
            !RequireScatteredBuildingOriginalUv0Only(fbxRoot / "buildings.fbx") ||
            !RequireBuildingFbx(fbxRoot / "buildings.fbx", fbxRoot, 100., 1000.))
        {
            return false;
        }
        const size_t scatteredBuildingFaceCount = CountFbxFaces(fbxRoot / "buildings.fbx");
        if (!Require(scatteredBuildingFaceCount > 0, "建筑散图 FBX 面数统计失败"))
        {
            return false;
        }

        const std::filesystem::path atlasPostProcessRoot = outputRoot / "postprocess-atlas";
        const std::filesystem::path atlasXyzRoot = atlasPostProcessRoot / "XYZ";
        const std::filesystem::path atlasFbxRoot = atlasPostProcessRoot / "FBX";
        renderOptions.buildingFbxWallTextureMode =
            DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
        renderOptions.buildingFbxRoofTextureMode =
            DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
        downloader.SetPostProcessOutputPath(atlasXyzRoot.string());
        downloader.SetPostProcessMergedImageOutputPath(
            (atlasPostProcessRoot / "merged.png").string());
        downloader.SetFbxOutputPath(atlasFbxRoot.string());
        renderResult = downloader.Render(fbxData, vectorOptions, renderOptions);
        if (!Require(renderResult.success,
                "PNG 后处理合并贴图建筑渲染失败：" + renderResult.error) ||
            !Require(std::filesystem::exists(atlasFbxRoot / "buildings.fbx"),
                "后处理缺少合并贴图建筑 FBX") ||
            !Require(std::filesystem::exists(atlasFbxRoot / "building_wall_atlas.png"),
                "后处理缺少墙体合并贴图") ||
            !Require(std::filesystem::exists(atlasFbxRoot / "building_roof_atlas.png"),
                "后处理缺少屋顶合并贴图") ||
            !Require(!std::filesystem::exists(atlasFbxRoot / "building_wall_0.jpg"),
                "合并贴图模式不应复制散落墙体贴图") ||
            !Require(!std::filesystem::exists(atlasFbxRoot / "building_roof_0.jpg"),
                "合并贴图模式不应复制散落屋顶贴图") ||
            !postprocess_test::RequireFbxAxisAndUnit(atlasFbxRoot / "buildings.fbx") ||
            !RequireBuildingFbx(
                atlasFbxRoot / "buildings.fbx", atlasFbxRoot, 100., 1000., true))
        {
            return false;
        }
        if (!Require(
                CountFbxFaces(atlasFbxRoot / "buildings.fbx") <= scatteredBuildingFaceCount,
                "建筑合并贴图 FBX 不应通过切分墙面增加总面数"))
        {
            return false;
        }

        const std::filesystem::path jpgAtlasPostProcessRoot =
            outputRoot / "postprocess-atlas-jpg";
        const std::filesystem::path jpgAtlasXyzRoot = jpgAtlasPostProcessRoot / "XYZ";
        const std::filesystem::path jpgAtlasFbxRoot = jpgAtlasPostProcessRoot / "FBX";
        renderOptions.buildingFbxAtlasImageFormat =
            DownTool::AmapBuildingFbxAtlasImageFormat::Jpg;
        downloader.SetPostProcessOutputPath(jpgAtlasXyzRoot.string());
        downloader.SetPostProcessMergedImageOutputPath(
            (jpgAtlasPostProcessRoot / "merged.png").string());
        downloader.SetFbxOutputPath(jpgAtlasFbxRoot.string());
        renderResult = downloader.Render(fbxData, vectorOptions, renderOptions);
        if (!Require(renderResult.success,
                "PNG 后处理 JPG 合并贴图建筑渲染失败：" + renderResult.error) ||
            !Require(std::filesystem::exists(jpgAtlasFbxRoot / "buildings.fbx"),
                "后处理缺少 JPG 合并贴图建筑 FBX") ||
            !Require(std::filesystem::exists(jpgAtlasFbxRoot / "building_wall_atlas.jpg"),
                "后处理缺少 JPG 墙体合并贴图") ||
            !Require(std::filesystem::exists(jpgAtlasFbxRoot / "building_roof_atlas.jpg"),
                "后处理缺少 JPG 屋顶合并贴图") ||
            !Require(!std::filesystem::exists(jpgAtlasFbxRoot / "building_wall_atlas.png"),
                "JPG 合并贴图模式不应生成 PNG 墙体 atlas") ||
            !Require(!std::filesystem::exists(jpgAtlasFbxRoot / "building_roof_atlas.png"),
                "JPG 合并贴图模式不应生成 PNG 屋顶 atlas") ||
            !postprocess_test::RequireFbxAxisAndUnit(jpgAtlasFbxRoot / "buildings.fbx") ||
            !RequireBuildingFbx(
                jpgAtlasFbxRoot / "buildings.fbx",
                jpgAtlasFbxRoot,
                100.,
                1000.,
                true,
                ".jpg"))
        {
            return false;
        }
        if (!Require(
                CountFbxFaces(jpgAtlasFbxRoot / "buildings.fbx") <= scatteredBuildingFaceCount,
                "JPG 建筑合并贴图 FBX 不应通过切分墙面增加总面数"))
        {
            return false;
        }

        const std::filesystem::path satelliteRoofFbxRoot = outputRoot / "satellite-roof-fbx";
        const std::filesystem::path satelliteRoofLeftTexture =
            satelliteRoofFbxRoot / "satellite_roof_left.jpg";
        const std::filesystem::path satelliteRoofRightTexture =
            satelliteRoofFbxRoot / "satellite_roof_right.jpg";
        if (!Require(
                WriteJpegTexture(
                    satelliteRoofLeftTexture,
                    256,
                    256,
                    cv::Scalar(20, 140, 220)),
                "无法写入左侧卫星屋顶测试贴图") ||
            !Require(
                WriteJpegTexture(
                    satelliteRoofRightTexture,
                    256,
                    256,
                    cv::Scalar(120, 80, 40)),
                "无法写入右侧卫星屋顶测试贴图"))
        {
            return false;
        }
        DownTool::AmapVectorDecodedData satelliteRoofData;
        satelliteRoofData.features.push_back(MakeHeightBuilding(centerGcj02, 12, true, 30));
        DownTool::AmapBuildingFbxExportOptions satelliteRoofOptions;
        satelliteRoofOptions.outputPath = satelliteRoofFbxRoot / "buildings.fbx";
        satelliteRoofOptions.centerWgs84 = centerWgs84;
        satelliteRoofOptions.wallTexturePath = textureRoot / std::filesystem::u8path("墙体");
        satelliteRoofOptions.roofTextureMode =
            DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
        satelliteRoofOptions.satelliteRoofTextures.push_back({
            satelliteRoofLeftTexture,
            centerWgs84.lon - 0.02,
            centerWgs84.lat - 0.02,
            centerWgs84.lon,
            centerWgs84.lat + 0.02});
        satelliteRoofOptions.satelliteRoofTextures.push_back({
            satelliteRoofRightTexture,
            centerWgs84.lon,
            centerWgs84.lat - 0.02,
            centerWgs84.lon + 0.02,
            centerWgs84.lat + 0.02});
        const DownTool::AmapBuildingFbxExportResult satelliteRoofResult =
            DownTool::ExportAmapBuildingsToFbx(satelliteRoofData, satelliteRoofOptions);
        if (!Require(
                satelliteRoofResult.success,
                "卫星屋顶建筑 FBX 导出失败：" + satelliteRoofResult.error) ||
            !Require(!std::filesystem::exists(
                    satelliteRoofFbxRoot / "building_roof_satellite_0.jpg"),
                "卫星屋顶 FBX 不应额外复制左侧卫星贴图") ||
            !Require(!std::filesystem::exists(
                    satelliteRoofFbxRoot / "building_roof_satellite_1.jpg"),
                "卫星屋顶 FBX 不应额外复制右侧卫星贴图") ||
            !RequireSatelliteRoofFbx(satelliteRoofOptions.outputPath, 2))
        {
            return false;
        }

        const std::filesystem::path buildingShapeRoot =
            outputRoot / "building-shape";
        DownTool::AmapVectorDecodedData buildingShapeData;
        buildingShapeData.features.push_back(MakeHeightBuilding(
            { centerGcj02.lon + 0.03, centerGcj02.lat }, 12, true, 31));
        DownTool::AmapBuildingFbxExportOptions planeBuildingOptions;
        planeBuildingOptions.outputPath =
            buildingShapeRoot / "plane" / "buildings.fbx";
        planeBuildingOptions.centerWgs84 = centerWgs84;
        planeBuildingOptions.wallTexturePath =
            textureRoot / std::filesystem::u8path("墙体");
        planeBuildingOptions.roofTexturePath =
            textureRoot / std::filesystem::u8path("房顶");
        planeBuildingOptions.shape = DownTool::FbxPlanetShape::Plane;
        DownTool::AmapBuildingFbxExportOptions sphereBuildingOptions =
            planeBuildingOptions;
        sphereBuildingOptions.outputPath =
            buildingShapeRoot / "sphere" / "buildings.fbx";
        sphereBuildingOptions.shape = DownTool::FbxPlanetShape::Sphere;
        const DownTool::AmapBuildingFbxExportResult planeBuildingResult =
            DownTool::ExportAmapBuildingsToFbx(
                buildingShapeData, planeBuildingOptions);
        const DownTool::AmapBuildingFbxExportResult sphereBuildingResult =
            DownTool::ExportAmapBuildingsToFbx(
                buildingShapeData, sphereBuildingOptions);
        const std::optional<float> planeMinimumZ =
            MinimumFbxVertexZ(planeBuildingOptions.outputPath);
        const std::optional<float> sphereMinimumZ =
            MinimumFbxVertexZ(sphereBuildingOptions.outputPath);
        if (!Require(
                planeBuildingResult.success,
                "平面建筑 FBX 导出失败：" + planeBuildingResult.error) ||
            !Require(
                sphereBuildingResult.success,
                "球面建筑 FBX 导出失败：" + sphereBuildingResult.error) ||
            !Require(
                planeMinimumZ.has_value() && sphereMinimumZ.has_value(),
                "无法读取建筑形态测试 FBX 顶点") ||
            !Require(
                *sphereMinimumZ < *planeMinimumZ - 10.F,
                "球面建筑没有按椭球曲面生成不同于平面的高度坐标"))
        {
            return false;
        }

        const std::filesystem::path buildingExporterPath =
            std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) / "src" /
            "AmapBuildingFbxExporter.cpp";
        const std::filesystem::path exportTaskManagerPath =
            std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) / "src" / "preview" /
            "ExportTaskManager.cpp";
        if (!Require(
                !postprocess_test::ContainsText(buildingExporterPath, "IsWallMeshTexturePath") &&
                    postprocess_test::ContainsText(buildingExporterPath, "wallMeshBuckets") &&
                    postprocess_test::ContainsText(buildingExporterPath, "roofMeshBuckets"),
                "建筑 FBX 导出不应再靠贴图文件名前缀区分墙体和屋顶 mesh"))
        {
            return false;
        }
        if (!Require(
                postprocess_test::ContainsText(
                    exportTaskManagerPath, "ExportSatelliteRoofBuildingFbxToRoot") &&
                    postprocess_test::ContainsText(
                        exportTaskManagerPath, "compositeRoot / \"FBX\"") &&
                    postprocess_test::ContainsText(
                        exportTaskManagerPath, "!satelliteRoofBuildingFbx"),
                "卫星屋顶建筑 FBX 应输出到对应卫星 FBX 目录"))
        {
            return false;
        }

        std::cout << "AmapVectorPngTileTest passed\n";
        return true;
    }
}

int main()
{
    try
    {
        return RunTest() ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "unexpected exception: " << exception.what() << '\n';
        return 1;
    }
}
