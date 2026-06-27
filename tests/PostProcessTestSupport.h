#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "fmt/format.h"
#include "opencv2/core.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

#include "GCJ02GoogleDownBase.h"
#include "GoogleDownBase.h"

namespace postprocess_test
{
    inline std::string PathToUtf8String(const std::filesystem::path& path)
    {
        const std::u8string utf8Path = path.u8string();
        std::string result;
        result.reserve(utf8Path.size());
        for (char8_t character : utf8Path)
        {
            result.push_back(static_cast<char>(character));
        }

        return result;
    }

    inline std::filesystem::path MakeTestRoot(const std::string& testName)
    {
        return std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) /
            "tmp" /
            "postprocess-tests" /
            testName;
    }

    inline std::filesystem::path MakePathWithUtf8Suffix(
        const std::filesystem::path& path,
        const std::string& suffix)
    {
        return std::filesystem::u8path(PathToUtf8String(path) + suffix);
    }

    inline bool ResetDirectory(const std::filesystem::path& path)
    {
        std::error_code removeError;
        std::filesystem::remove_all(path, removeError);
        if (removeError)
        {
            std::cerr << "remove_all failed: " << PathToUtf8String(path) << "\n";
            return false;
        }

        std::error_code createError;
        std::filesystem::create_directories(path, createError);
        if (createError)
        {
            std::cerr << "create_directories failed: " << PathToUtf8String(path) << "\n";
            return false;
        }

        return true;
    }

    inline bool Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << message << "\n";
            return false;
        }

        return true;
    }

    inline bool RequireFileExists(const std::filesystem::path& path)
    {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        return Require(!existsError && exists, "missing file: " + PathToUtf8String(path));
    }

    inline bool RequireNonEmptyFile(const std::filesystem::path& path)
    {
        if (!RequireFileExists(path))
        {
            return false;
        }

        std::error_code sizeError;
        const uintmax_t fileSize = std::filesystem::file_size(path, sizeError);
        return Require(
            !sizeError && fileSize > 0,
            "empty file: " + PathToUtf8String(path));
    }

    inline bool RequireImageSize(
        const std::filesystem::path& path,
        int expectedWidth,
        int expectedHeight)
    {
        const cv::Mat image = cv::imread(PathToUtf8String(path), cv::IMREAD_UNCHANGED);
        if (image.empty())
        {
            std::cerr << "image unreadable: " << PathToUtf8String(path) << "\n";
            return false;
        }

        return Require(
            image.cols == expectedWidth && image.rows == expectedHeight,
            fmt::format(
                "unexpected image size: {} is {}x{}, expected {}x{}",
                PathToUtf8String(path),
                image.cols,
                image.rows,
                expectedWidth,
                expectedHeight));
    }

    inline bool RequireReadableImage(const std::filesystem::path& path)
    {
        const cv::Mat image = cv::imread(PathToUtf8String(path), cv::IMREAD_UNCHANGED);
        if (image.empty())
        {
            std::cerr << "image unreadable: " << PathToUtf8String(path) << "\n";
            return false;
        }

        return true;
    }

    inline bool ContainsText(const std::filesystem::path& path, const std::string& needle)
    {
        std::ifstream fileStream(path, std::ios::binary);
        if (!fileStream.is_open())
        {
            return false;
        }

        const std::string content(
            (std::istreambuf_iterator<char>(fileStream)),
            std::istreambuf_iterator<char>());
        return content.find(needle) != std::string::npos;
    }

    inline bool HasAnyPng(const std::filesystem::path& path)
    {
        std::error_code existsError;
        if (!std::filesystem::exists(path, existsError) || existsError)
        {
            return false;
        }

        for (const std::filesystem::directory_entry& entry :
            std::filesystem::directory_iterator(path))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".png")
            {
                return true;
            }
        }

        return false;
    }

    inline bool ReadFbxMetadataInt(
        const aiScene& scene,
        const std::string& key,
        int32_t& value)
    {
        return scene.mMetaData != nullptr && scene.mMetaData->Get(key, value);
    }

    inline bool ReadFbxMetadataNumber(
        const aiScene& scene,
        const std::string& key,
        double& value)
    {
        if (scene.mMetaData == nullptr)
        {
            return false;
        }

        float floatValue = 0.f;
        if (scene.mMetaData->Get(key, floatValue))
        {
            value = floatValue;
            return true;
        }

        return scene.mMetaData->Get(key, value);
    }

    inline const aiScene* ReadFbxScene(
        Assimp::Importer& importer,
        const std::filesystem::path& fbxPath)
    {
        const aiScene* scene = importer.ReadFile(PathToUtf8String(fbxPath), 0);
        if (scene == nullptr)
        {
            std::cerr << "FBX import failed: " << importer.GetErrorString() << "\n";
        }

        return scene;
    }

    inline bool RequireFbxAxisAndUnit(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = ReadFbxScene(importer, fbxPath);
        if (!Require(scene != nullptr, "FBX scene is null: " + PathToUtf8String(fbxPath)))
        {
            return false;
        }

        int32_t intValue = 0;
        double numberValue = 0.;
        return Require(
            ReadFbxMetadataInt(*scene, "UpAxis", intValue) && intValue == 2,
            "FBX UpAxis is not Z") &&
            Require(
                ReadFbxMetadataInt(*scene, "FrontAxis", intValue) && intValue == 1,
                "FBX FrontAxis is not Y") &&
            Require(
                ReadFbxMetadataInt(*scene, "FrontAxisSign", intValue) && intValue == -1,
                "FBX FrontAxisSign is not -Y") &&
            Require(
                ReadFbxMetadataInt(*scene, "CoordAxis", intValue) && intValue == 0,
                "FBX CoordAxis is not X") &&
            Require(
                ReadFbxMetadataNumber(*scene, "UnitScaleFactor", numberValue) &&
                    std::abs(numberValue - 1.) < 1.e-6,
                "FBX UnitScaleFactor is not centimeter");
    }

    inline bool RequirePlaneFbxUvMatchesNorthAxis(const std::filesystem::path& fbxPath)
    {
        Assimp::Importer importer;
        const aiScene* scene = ReadFbxScene(importer, fbxPath);
        if (!Require(scene != nullptr, "FBX scene is null: " + PathToUtf8String(fbxPath)) ||
            !Require(scene->HasMeshes(), "FBX scene has no mesh"))
        {
            return false;
        }

        bool checkedMesh = false;
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
        {
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr || !mesh->HasTextureCoords(0) || mesh->mNumVertices == 0)
            {
                continue;
            }

            float minY = std::numeric_limits<float>::max();
            float maxY = std::numeric_limits<float>::lowest();
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; vertexIndex++)
            {
                minY = std::min(minY, mesh->mVertices[vertexIndex].y);
                maxY = std::max(maxY, mesh->mVertices[vertexIndex].y);
            }

            if (maxY - minY < 1.f)
            {
                continue;
            }

            double northV = 0.;
            double southV = 0.;
            uint32_t northCount = 0;
            uint32_t southCount = 0;
            for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices; vertexIndex++)
            {
                const float y = mesh->mVertices[vertexIndex].y;
                const float v = mesh->mTextureCoords[0][vertexIndex].y;
                if (std::abs(y - maxY) < 1.e-3f)
                {
                    northV += v;
                    northCount++;
                }
                if (std::abs(y - minY) < 1.e-3f)
                {
                    southV += v;
                    southCount++;
                }
            }

            if (northCount == 0 || southCount == 0)
            {
                continue;
            }

            checkedMesh = true;
            northV /= northCount;
            southV /= southCount;
            if (!Require(northV > southV, "FBX UV V is not aligned with north Y"))
            {
                return false;
            }
        }

        return Require(checkedMesh, "FBX plane UV test did not find a usable mesh");
    }

    inline bool WriteSourceTile(
        const std::filesystem::path& path,
        uint32_t tileSize,
        const cv::Scalar& color)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            return false;
        }

        cv::Mat image(static_cast<int>(tileSize), static_cast<int>(tileSize), CV_8UC4, color);
        cv::line(
            image,
            cv::Point(0, 0),
            cv::Point(static_cast<int>(tileSize) - 1, static_cast<int>(tileSize) - 1),
            cv::Scalar(255, 255, 255, 255),
            1);
        return cv::imwrite(PathToUtf8String(path), image);
    }

    class TestPostProcessDown : public DownTool::GoogleDownBase
    {
    public:
        void Configure(
            const std::filesystem::path& cachePath,
            const std::filesystem::path& outputPath,
            uint32_t zoom,
            uint32_t tileSize,
            const DownTool::Tile& minTile,
            const DownTool::Tile& maxTile,
            const DownTool::LonLat& centerLonLat)
        {
            CachePath = PathToUtf8String(cachePath);
            PostProcessOutputPath = PathToUtf8String(outputPath);
            Zoom = zoom;
            TileSize = tileSize;
            MinTile = minTile;
            MaxTile = maxTile;
            CenterLonLat = centerLonLat;

            const DownTool::LonLat northWest = DownTool::GoogleDownBase::TileToLonLat(minTile);
            const DownTool::LonLat southEast = DownTool::GoogleDownBase::TileToLonLat(
                DownTool::Tile(maxTile.X + 1, maxTile.Y + 1, zoom));
            MinLonLat = DownTool::LonLat(northWest.lon, southEast.lat);
            MaxLonLat = DownTool::LonLat(southEast.lon, northWest.lat);
        }

        void RunPostProcessForTest()
        {
            RunPostProcess();
        }

        std::filesystem::path TilePath(const DownTool::Tile& tile) const
        {
            return GetTileFilePath(tile);
        }

        std::filesystem::path PostProcessTilePath(const DownTool::Tile& tile) const
        {
            return GetPostProcessTileFilePath(tile);
        }

        std::filesystem::path PostProcessRootPath() const
        {
            return GetPostProcessRootPath();
        }

        std::filesystem::path MergedImagePath() const
        {
            return GetPostProcessMergedImagePath();
        }

        std::filesystem::path FbxRootPath() const
        {
            return GetFbxRootPath();
        }
    };

    class TestGcj02PostProcessDown : public DownTool::GCJ02GoogleDownBase
    {
    public:
        void Configure(
            const std::filesystem::path& cachePath,
            const std::filesystem::path& outputPath,
            uint32_t zoom,
            uint32_t tileSize,
            const DownTool::Tile& minTile,
            const DownTool::Tile& maxTile,
            const DownTool::LonLat& centerLonLat)
        {
            CachePath = PathToUtf8String(cachePath);
            PostProcessOutputPath = PathToUtf8String(outputPath);
            Zoom = zoom;
            TileSize = tileSize;
            MinTile = minTile;
            MaxTile = maxTile;
            CenterLonLat = centerLonLat;

            const DownTool::LonLat northWest = DownTool::GoogleDownBase::TileToLonLat(minTile);
            const DownTool::LonLat southEast = DownTool::GoogleDownBase::TileToLonLat(
                DownTool::Tile(maxTile.X + 1, maxTile.Y + 1, zoom));
            MinLonLat = DownTool::LonLat(northWest.lon, southEast.lat);
            MaxLonLat = DownTool::LonLat(southEast.lon, northWest.lat);
        }

        void RunPostProcessForTest()
        {
            RunPostProcess();
        }

        std::filesystem::path TilePath(const DownTool::Tile& tile) const
        {
            return GetTileFilePath(tile);
        }

        std::filesystem::path MergedImagePath() const
        {
            return GetPostProcessMergedImagePath();
        }
    };

    inline bool WriteSourceTiles(TestPostProcessDown& tool)
    {
        for (uint32_t x = tool.GetMinTile().X; x <= tool.GetMaxTile().X; x++)
        {
            for (uint32_t y = tool.GetMinTile().Y; y <= tool.GetMaxTile().Y; y++)
            {
                const cv::Scalar color(
                    static_cast<double>((x * 53) % 255),
                    static_cast<double>((y * 79) % 255),
                    static_cast<double>(((x + y) * 41) % 255),
                    255.);
                if (!WriteSourceTile(
                    tool.TilePath(DownTool::Tile(x, y, tool.GetZoom())),
                    tool.GetTileSize(),
                    color))
                {
                    std::cerr << "write source tile failed: "
                        << x << "," << y << "\n";
                    return false;
                }
            }
        }

        return true;
    }

    template<typename Tool>
    inline bool WriteOffsetSourceTiles(Tool& tool)
    {
        for (uint32_t x = tool.GetMinTile().X; x <= tool.GetMaxTile().X; x++)
        {
            for (uint32_t y = tool.GetMinTile().Y; y <= tool.GetMaxTile().Y; y++)
            {
                const DownTool::Tile targetTile(x, y, tool.GetZoom());
                const DownTool::TileRange offsetTiles = tool.GetOffsetTiles(targetTile);
                for (uint32_t sourceX = offsetTiles.MinTile.X;
                    sourceX <= offsetTiles.MaxTile.X;
                    sourceX++)
                {
                    for (uint32_t sourceY = offsetTiles.MinTile.Y;
                        sourceY <= offsetTiles.MaxTile.Y;
                        sourceY++)
                    {
                        const cv::Scalar color(
                            static_cast<double>((sourceX * 53) % 255),
                            static_cast<double>((sourceY * 79) % 255),
                            static_cast<double>(((sourceX + sourceY) * 41) % 255),
                            255.);
                        if (!WriteSourceTile(
                            tool.TilePath(DownTool::Tile(sourceX, sourceY, tool.GetZoom())),
                            tool.GetTileSize(),
                            color))
                        {
                            std::cerr << "write offset source tile failed: "
                                << sourceX << "," << sourceY << "\n";
                            return false;
                        }
                    }
                }
            }
        }

        return true;
    }

    inline void ConfigureTool(TestPostProcessDown& tool, const std::string& testName)
    {
        const std::filesystem::path testRoot = MakeTestRoot(testName);
        tool.Configure(
            testRoot / "cache",
            testRoot / "output",
            2,
            32,
            DownTool::Tile(1, 1, 2),
            DownTool::Tile(2, 2, 2),
            DownTool::LonLat(45., 0.));
        tool.SetMaxDownQueue(4);
    }

    inline int RunGuarded(bool (*testFunction)())
    {
        try
        {
            return testFunction() ? 0 : 1;
        }
        catch (const std::exception& exception)
        {
            std::cerr << "unexpected exception: " << exception.what() << "\n";
            return 1;
        }
        catch (...)
        {
            std::cerr << "unexpected unknown exception\n";
            return 1;
        }
    }
}
