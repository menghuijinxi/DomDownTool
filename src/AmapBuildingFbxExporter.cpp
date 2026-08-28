#include "AmapBuildingFbxExporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "assimp/Exporter.hpp"
#include "assimp/material.h"
#include "assimp/scene.h"
#include "fmt/format.h"
#include "mapbox/earcut.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

#include "GCJ02GoogleDownBase.h"

namespace
{
    struct Point2
    {
        double x = 0.;
        double y = 0.;
    };

    using Ring = std::vector<Point2>;

    struct PolygonRing
    {
        Ring ring;
        bool hole = false;
    };

    struct BuildingPolygon
    {
        std::vector<PolygonRing> rings;
    };

    struct Bounds2
    {
        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();

        void Add(const Point2& point)
        {
            minX = std::min(minX, point.x);
            minY = std::min(minY, point.y);
            maxX = std::max(maxX, point.x);
            maxY = std::max(maxY, point.y);
        }

        [[nodiscard]] bool IsValid() const
        {
            return minX <= maxX && minY <= maxY;
        }
    };

    struct BuildingVertex
    {
        double x = 0.;
        double y = 0.;
        double z = 0.;
        double u = 0.;
        double v = 0.;
        std::array<double, 3> normal = { 0., 0., 1. };
        std::array<double, 2> atlasOffset = { 0., 0. };
        std::array<double, 2> atlasMax = { 1., 1. };
    };

    enum class BuildingMeshKind
    {
        Wall,
        Roof
    };

    struct BuildingMeshData
    {
        std::string name;
        BuildingMeshKind kind = BuildingMeshKind::Roof;
        std::filesystem::path texturePath;
        bool wrapTextureU = false;
        bool usesAtlasTransform = false;
        std::vector<BuildingVertex> vertices;
        std::vector<uint32_t> indices;
    };

    struct AtlasRect
    {
        double u0 = 0.;
        double v0 = 0.;
        double u1 = 1.;
        double v1 = 1.;
    };

    struct TextureInfo
    {
        std::filesystem::path sourcePath;
        std::filesystem::path outputPath;
        std::string materialName;
        int32_t width = 0;
        int32_t height = 0;

        [[nodiscard]] double AspectRatio() const
        {
            if (height <= 0)
            {
                return 1.;
            }
            return static_cast<double>(width) / static_cast<double>(height);
        }
    };

    struct AtlasSource
    {
        const TextureInfo* texture = nullptr;
        cv::Mat image;
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
    };

    struct AtlasPackingRect
    {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
    };

    struct TextureLibrary
    {
        std::vector<TextureInfo> wallTextures;
        std::vector<TextureInfo> roofTextures;
    };

    struct SatelliteRoofTextureInfo
    {
        TextureInfo texture;
        Bounds2 planeBounds;
    };

    struct MeshBucket
    {
        BuildingMeshData mesh;
    };

    struct BuildingGeometry
    {
        std::vector<BuildingPolygon> polygons;
        Bounds2 bounds;
        Point2 centroid;
        double wallLengthCentimeters = 0.;
    };

    struct WallMaterialBucketState
    {
        std::vector<size_t> textureIndices;
    };

    std::string PathToUtf8String(const std::filesystem::path& path)
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

    std::string ToPortableRelativePath(
        const std::filesystem::path& path,
        const std::filesystem::path& basePath)
    {
        std::error_code relativeError;
        std::filesystem::path relativePath =
            std::filesystem::relative(path, basePath, relativeError);
        if (relativeError)
        {
            relativePath = path;
        }

        std::string text = PathToUtf8String(relativePath);
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    }

    bool WriteBinaryFile(
        const std::filesystem::path& path,
        const void* data,
        size_t dataSize)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            return false;
        }

        std::ofstream fileStream(path, std::ios::binary | std::ios::trunc);
        if (!fileStream.is_open())
        {
            return false;
        }

        fileStream.write(static_cast<const char*>(data), static_cast<std::streamsize>(dataSize));
        return fileStream.good();
    }

    bool WriteEncodedImage(
        const std::filesystem::path& path,
        const std::string& extension,
        const cv::Mat& image)
    {
        std::vector<unsigned char> encodedImage;
        std::vector<int> parameters;
        if (extension == ".jpg" || extension == ".jpeg")
        {
            parameters = { cv::IMWRITE_JPEG_QUALITY, 95 };
        }
        if (!cv::imencode(extension, image, encodedImage, parameters) ||
            encodedImage.empty())
        {
            return false;
        }
        return WriteBinaryFile(path, encodedImage.data(), encodedImage.size());
    }

    double LongitudeToWebMercatorX(double longitude)
    {
        constexpr double earthRadius = 6378137.;
        return earthRadius * longitude * std::numbers::pi / 180.;
    }

    double LatitudeToWebMercatorY(double latitude)
    {
        constexpr double earthRadius = 6378137.;
        constexpr double maxMercatorLatitude = 85.05112877980659;
        const double clampedLatitude =
            std::clamp(latitude, -maxMercatorLatitude, maxMercatorLatitude);
        const double latitudeRadians = clampedLatitude * std::numbers::pi / 180.;
        return earthRadius * std::log(
            std::tan(std::numbers::pi / 4. + latitudeRadians / 2.));
    }

    Point2 ProjectGcj02ToFbxPlane(
        const DownTool::AmapVectorCoordinate& coordinate,
        double centerMercatorX,
        double centerMercatorY)
    {
        const DownTool::LonLat wgs84 = DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(
            { coordinate.longitude, coordinate.latitude });
        return {
            (LongitudeToWebMercatorX(wgs84.lon) - centerMercatorX) * 100.,
            (LatitudeToWebMercatorY(wgs84.lat) - centerMercatorY) * 100.
        };
    }

    Point2 ProjectWgs84ToFbxPlane(
        const DownTool::LonLat& lonLat,
        double centerMercatorX,
        double centerMercatorY)
    {
        return {
            (LongitudeToWebMercatorX(lonLat.lon) - centerMercatorX) * 100.,
            (LatitudeToWebMercatorY(lonLat.lat) - centerMercatorY) * 100.
        };
    }

    DownTool::LonLat Gcj02CoordinateToWgs84(
        const DownTool::AmapVectorCoordinate& coordinate)
    {
        return DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(
            { coordinate.longitude, coordinate.latitude });
    }

    double Distance(const Point2& first, const Point2& second)
    {
        const double dx = second.x - first.x;
        const double dy = second.y - first.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    double SignedArea(const Ring& ring)
    {
        if (ring.size() < 3)
        {
            return 0.;
        }

        double area = 0.;
        for (size_t index = 0; index < ring.size(); index++)
        {
            const Point2& current = ring[index];
            const Point2& next = ring[(index + 1) % ring.size()];
            area += current.x * next.y - next.x * current.y;
        }
        return area * 0.5;
    }

    bool ContainsPoint(const Ring& ring, const Point2& point)
    {
        bool inside = false;
        for (size_t index = 0, previous = ring.size() - 1; index < ring.size();
             previous = index++)
        {
            const Point2& current = ring[index];
            const Point2& last = ring[previous];
            const bool intersects = ((current.y > point.y) != (last.y > point.y)) &&
                point.x < (last.x - current.x) * (point.y - current.y) /
                        (last.y - current.y) + current.x;
            if (intersects)
            {
                inside = !inside;
            }
        }
        return inside;
    }

    Ring BuildRing(
        const std::vector<DownTool::AmapVectorCoordinate>& sourceRing,
        double centerMercatorX,
        double centerMercatorY)
    {
        Ring ring;
        ring.reserve(sourceRing.size());
        for (const DownTool::AmapVectorCoordinate& coordinate : sourceRing)
        {
            const Point2 point = ProjectGcj02ToFbxPlane(
                coordinate,
                centerMercatorX,
                centerMercatorY);
            if (std::isfinite(point.x) && std::isfinite(point.y))
            {
                ring.push_back(point);
            }
        }

        while (ring.size() > 1 && ring.front().x == ring.back().x &&
               ring.front().y == ring.back().y)
        {
            ring.pop_back();
        }
        ring.erase(
            std::unique(
                ring.begin(),
                ring.end(),
                [](const Point2& left, const Point2& right)
                {
                    return left.x == right.x && left.y == right.y;
                }),
            ring.end());
        return ring;
    }

    std::vector<BuildingPolygon> ClassifyRings(const std::vector<Ring>& rings)
    {
        struct RingInfo
        {
            Ring ring;
            double area = 0.;
            size_t parent = std::numeric_limits<size_t>::max();
            int32_t depth = 0;
        };

        std::vector<RingInfo> sorted;
        sorted.reserve(rings.size());
        for (const Ring& ring : rings)
        {
            const double area = SignedArea(ring);
            if (ring.size() >= 3 && std::abs(area) > 1.e-6)
            {
                sorted.push_back({ ring, area });
            }
        }
        std::sort(
            sorted.begin(),
            sorted.end(),
            [](const RingInfo& left, const RingInfo& right)
            {
                return std::abs(left.area) > std::abs(right.area);
            });

        std::vector<BuildingPolygon> polygons;
        std::vector<size_t> outerIndices;
        for (size_t index = 0; index < sorted.size(); index++)
        {
            RingInfo& current = sorted[index];
            for (size_t candidate = 0; candidate < index; candidate++)
            {
                if (!ContainsPoint(sorted[candidate].ring, current.ring.front()))
                {
                    continue;
                }
                if (current.parent == std::numeric_limits<size_t>::max() ||
                    std::abs(sorted[candidate].area) <
                        std::abs(sorted[current.parent].area))
                {
                    current.parent = candidate;
                }
            }
            current.depth = current.parent == std::numeric_limits<size_t>::max() ?
                0 : sorted[current.parent].depth + 1;
            if ((current.depth & 1) == 0)
            {
                BuildingPolygon polygon;
                polygon.rings.push_back({ current.ring, false });
                polygons.push_back(std::move(polygon));
                outerIndices.push_back(index);
                continue;
            }

            size_t parent = current.parent;
            while (parent != std::numeric_limits<size_t>::max() &&
                   (sorted[parent].depth & 1) != 0)
            {
                parent = sorted[parent].parent;
            }
            const auto outerIterator =
                std::find(outerIndices.begin(), outerIndices.end(), parent);
            if (outerIterator != outerIndices.end())
            {
                const size_t polygonIndex = static_cast<size_t>(
                    std::distance(outerIndices.begin(), outerIterator));
                polygons[polygonIndex].rings.push_back({ current.ring, true });
            }
        }
        return polygons;
    }

    BuildingGeometry BuildBuildingGeometry(
        const DownTool::AmapVectorFeature& feature,
        const DownTool::LonLat& centerWgs84)
    {
        const double centerMercatorX = LongitudeToWebMercatorX(centerWgs84.lon);
        const double centerMercatorY = LatitudeToWebMercatorY(centerWgs84.lat);
        std::vector<Ring> rings;
        rings.reserve(feature.geometryParts.size());
        Bounds2 bounds;
        double totalX = 0.;
        double totalY = 0.;
        uint32_t pointCount = 0;

        for (const std::vector<DownTool::AmapVectorCoordinate>& sourceRing :
             feature.geometryParts)
        {
            Ring ring = BuildRing(sourceRing, centerMercatorX, centerMercatorY);
            if (ring.size() < 3 || std::abs(SignedArea(ring)) <= 1.e-6)
            {
                continue;
            }
            for (const Point2& point : ring)
            {
                bounds.Add(point);
                totalX += point.x;
                totalY += point.y;
                pointCount++;
            }
            rings.push_back(std::move(ring));
        }

        BuildingGeometry geometry;
        geometry.polygons = ClassifyRings(rings);
        geometry.bounds = bounds;
        if (pointCount != 0)
        {
            geometry.centroid = { totalX / pointCount, totalY / pointCount };
        }
        for (const BuildingPolygon& polygon : geometry.polygons)
        {
            for (const PolygonRing& polygonRing : polygon.rings)
            {
                const Ring& ring = polygonRing.ring;
                for (size_t index = 0; index < ring.size(); index++)
                {
                    geometry.wallLengthCentimeters +=
                        Distance(ring[index], ring[(index + 1) % ring.size()]);
                }
            }
        }
        return geometry;
    }

    std::optional<std::pair<double, double>> SampleBuildingTerrainHeightRange(
        const DownTool::AmapVectorFeature& feature,
        const DownTool::AmapBuildingFbxExportOptions& options)
    {
        if (!options.terrainHeightEnabled || !options.elevationSampler)
        {
            return std::nullopt;
        }

        double minimumCentimeters = std::numeric_limits<double>::max();
        double maximumCentimeters = std::numeric_limits<double>::lowest();
        bool hasSample = false;
        for (const std::vector<DownTool::AmapVectorCoordinate>& sourceRing :
             feature.geometryParts)
        {
            for (const DownTool::AmapVectorCoordinate& coordinate : sourceRing)
            {
                const std::optional<double> elevationMeters = options.elevationSampler(
                    Gcj02CoordinateToWgs84(coordinate));
                if (!elevationMeters)
                {
                    continue;
                }

                const double relativeCentimeters =
                    std::max(0., *elevationMeters - options.elevationReferenceMeters) * 100.;
                minimumCentimeters = std::min(minimumCentimeters, relativeCentimeters);
                maximumCentimeters = std::max(maximumCentimeters, relativeCentimeters);
                hasSample = true;
            }
        }
        if (!hasSample)
        {
            return std::nullopt;
        }
        return std::pair<double, double>{ minimumCentimeters, maximumCentimeters };
    }

    bool HasJpegSignature(const std::filesystem::path& path)
    {
        std::ifstream fileStream(path, std::ios::binary);
        if (!fileStream.is_open())
        {
            return false;
        }

        std::array<unsigned char, 3> signature{};
        fileStream.read(
            reinterpret_cast<char*>(signature.data()),
            static_cast<std::streamsize>(signature.size()));
        return fileStream.gcount() == static_cast<std::streamsize>(signature.size()) &&
            signature[0] == 0xff && signature[1] == 0xd8 && signature[2] == 0xff;
    }

    std::string LowercaseExtension(const std::filesystem::path& path)
    {
        std::string extension = PathToUtf8String(path.extension());
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return extension;
    }

    bool IsSupportedTexturePath(const std::filesystem::path& path)
    {
        const std::string extension = LowercaseExtension(path);
        return extension == ".jpg" || extension == ".jpeg" || extension == ".png" ||
            HasJpegSignature(path);
    }

    std::string OutputTextureExtension(const std::filesystem::path& path)
    {
        const std::string extension = LowercaseExtension(path);
        if (extension == ".png")
        {
            return ".png";
        }
        return ".jpg";
    }

    std::string AtlasImageExtension(DownTool::AmapBuildingFbxAtlasImageFormat format)
    {
        switch (format)
        {
        case DownTool::AmapBuildingFbxAtlasImageFormat::Png:
            return ".png";
        case DownTool::AmapBuildingFbxAtlasImageFormat::Jpg:
            return ".jpg";
        }
        return ".png";
    }

    cv::Mat ReadTextureImage(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            return {};
        }

        input.seekg(0, std::ios::end);
        const std::streamoff size = input.tellg();
        if (size <= 0)
        {
            return {};
        }
        input.seekg(0, std::ios::beg);

        std::vector<unsigned char> buffer(static_cast<size_t>(size));
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() != static_cast<std::streamsize>(buffer.size()))
        {
            return {};
        }

        return cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
    }

    std::filesystem::path ResolveTextureRoot(const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute())
        {
            return path;
        }
        std::error_code existsError;
        if (std::filesystem::exists(path, existsError))
        {
            return path;
        }

        std::filesystem::path currentPath = std::filesystem::current_path();
        for (uint32_t depth = 0; depth < 6 && !currentPath.empty(); depth++)
        {
            const std::filesystem::path candidate = currentPath / path;
            if (std::filesystem::exists(candidate, existsError))
            {
                return candidate;
            }
            const std::filesystem::path projectSiblingCandidate =
                currentPath / "DomDownTool" / path;
            if (std::filesystem::exists(projectSiblingCandidate, existsError))
            {
                return projectSiblingCandidate;
            }
            currentPath = currentPath.parent_path();
        }
        return path;
    }

    bool LoadTextureList(
        const std::filesystem::path& rootPath,
        const std::filesystem::path& outputDirectory,
        const std::string& namePrefix,
        std::vector<TextureInfo>& textures,
        std::string& error)
    {
        const std::filesystem::path resolvedRoot = ResolveTextureRoot(rootPath);
        std::error_code directoryError;
        if (!std::filesystem::is_directory(resolvedRoot, directoryError))
        {
            error = "建筑贴图目录不存在：" + PathToUtf8String(resolvedRoot);
            return false;
        }

        std::vector<std::filesystem::path> paths;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(resolvedRoot, directoryError))
        {
            if (directoryError)
            {
                error = "读取建筑贴图目录失败：" + directoryError.message();
                return false;
            }
            if (entry.is_regular_file() && IsSupportedTexturePath(entry.path()))
            {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());

        for (const std::filesystem::path& path : paths)
        {
            const cv::Mat image = ReadTextureImage(path);
            if (image.empty() || image.cols <= 0 || image.rows <= 0)
            {
                continue;
            }

            const size_t textureIndex = textures.size();
            const std::string outputName = fmt::format(
                "{}_{}{}",
                namePrefix,
                textureIndex,
                OutputTextureExtension(path));
            textures.push_back({
                path,
                outputDirectory / outputName,
                fmt::format("{}_{}", namePrefix, textureIndex),
                image.cols,
                image.rows
            });
        }

        if (textures.empty())
        {
            error = "建筑贴图目录没有可读取的 JPG/PNG 贴图：" +
                PathToUtf8String(resolvedRoot);
            return false;
        }
        return true;
    }

    bool LoadTextureLibrary(
        const DownTool::AmapBuildingFbxExportOptions& options,
        TextureLibrary& library,
        std::string& error)
    {
        const std::filesystem::path outputDirectory = options.outputPath.parent_path();
        if (!LoadTextureList(
                options.wallTexturePath,
                outputDirectory,
                "building_wall",
                library.wallTextures,
                error))
        {
            return false;
        }
        if (options.roofTextureMode == DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
        {
            return true;
        }
        return LoadTextureList(
            options.roofTexturePath,
            outputDirectory,
            "building_roof",
            library.roofTextures,
            error);
    }

    bool IsFiniteSatelliteRoofBounds(
        const DownTool::AmapBuildingFbxSatelliteRoofTexture& source)
    {
        return std::isfinite(source.west) && std::isfinite(source.south) &&
            std::isfinite(source.east) && std::isfinite(source.north) &&
            source.west < source.east && source.south < source.north;
    }

    bool BuildSatelliteRoofTextureList(
        const DownTool::AmapBuildingFbxExportOptions& options,
        std::vector<SatelliteRoofTextureInfo>& textures,
        std::string& error)
    {
        textures.clear();
        if (options.roofTextureMode != DownTool::AmapBuildingFbxRoofTextureMode::Satellite)
        {
            return true;
        }
        if (options.satelliteRoofTextures.empty())
        {
            error = "屋顶卫星图模式缺少卫星 FBX 贴图清单。";
            return false;
        }

        const double centerMercatorX = LongitudeToWebMercatorX(options.centerWgs84.lon);
        const double centerMercatorY = LatitudeToWebMercatorY(options.centerWgs84.lat);
        textures.reserve(options.satelliteRoofTextures.size());
        for (const DownTool::AmapBuildingFbxSatelliteRoofTexture& source :
             options.satelliteRoofTextures)
        {
            if (source.texturePath.empty() || !IsFiniteSatelliteRoofBounds(source))
            {
                continue;
            }

            const cv::Mat image = ReadTextureImage(source.texturePath);
            if (image.empty() || image.cols <= 0 || image.rows <= 0)
            {
                continue;
            }

            Bounds2 planeBounds;
            planeBounds.Add(ProjectWgs84ToFbxPlane(
                { source.west, source.south }, centerMercatorX, centerMercatorY));
            planeBounds.Add(ProjectWgs84ToFbxPlane(
                { source.east, source.north }, centerMercatorX, centerMercatorY));
            if (!planeBounds.IsValid())
            {
                continue;
            }

            const size_t textureIndex = textures.size();
            textures.push_back({
                TextureInfo{
                    source.texturePath,
                    source.texturePath,
                    fmt::format("building_roof_satellite_{}", textureIndex),
                    image.cols,
                    image.rows},
                planeBounds});
        }
        if (textures.empty())
        {
            error = "屋顶卫星图模式没有可读取的卫星 FBX 贴图。";
            return false;
        }
        return true;
    }

    int32_t WallTextureUnit(const TextureInfo& texture, uint32_t unitPixelSize)
    {
        const uint32_t safeUnitPixelSize = std::max(1u, unitPixelSize);
        return std::max(1, texture.height / static_cast<int32_t>(safeUnitPixelSize));
    }

    std::vector<size_t> BuildWallTextureCandidates(
        const std::vector<TextureInfo>& textures,
        uint64_t height,
        const DownTool::AmapBuildingFbxExportOptions& options)
    {
        const int32_t currentUnit = std::max(
            1,
            static_cast<int32_t>(
                static_cast<double>(height) / std::max(1., options.textureUnitLayer)));
        std::map<int32_t, std::vector<size_t>> texturesByUnit;
        for (size_t index = 0; index < textures.size(); index++)
        {
            texturesByUnit[WallTextureUnit(textures[index], options.textureUnitPixelSize)]
                .push_back(index);
        }

        std::vector<size_t> candidates;
        const auto upperIterator = texturesByUnit.lower_bound(currentUnit);
        if (upperIterator != texturesByUnit.end())
        {
            candidates.insert(
                candidates.end(),
                upperIterator->second.begin(),
                upperIterator->second.end());
        }
        if (upperIterator != texturesByUnit.begin())
        {
            const auto lowerIterator = std::prev(upperIterator);
            candidates.insert(
                candidates.end(),
                lowerIterator->second.begin(),
                lowerIterator->second.end());
        }
        if (candidates.empty())
        {
            for (size_t index = 0; index < textures.size(); index++)
            {
                candidates.push_back(index);
            }
        }

        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        return candidates;
    }

    std::string MakeWallMaterialBucketKey(
        const BuildingGeometry& geometry,
        uint64_t height,
        const DownTool::AmapBuildingFbxExportOptions& options)
    {
        constexpr double bucketSizeCentimeters = 100000.;
        const int64_t chunkX = static_cast<int64_t>(
            std::floor(geometry.centroid.x / bucketSizeCentimeters));
        const int64_t chunkY = static_cast<int64_t>(
            std::floor(geometry.centroid.y / bucketSizeCentimeters));
        const int32_t currentUnit = std::max(
            1,
            static_cast<int32_t>(
                static_cast<double>(height) / std::max(1., options.textureUnitLayer)));
        return fmt::format("{}:{}:{}", chunkX, chunkY, currentUnit);
    }

    size_t SelectWeightedWallTexture(
        const std::vector<TextureInfo>& textures,
        const std::vector<size_t>& candidates,
        double heightCentimeters,
        double wallLengthCentimeters,
        std::mt19937& randomEngine)
    {
        std::vector<double> weights;
        weights.reserve(candidates.size());
        for (const size_t index : candidates)
        {
            const TextureInfo& texture = textures[index];
            const double textureWallWidth = heightCentimeters /
                std::max(1., static_cast<double>(texture.height)) *
                std::max(1., static_cast<double>(texture.width));
            weights.push_back(std::clamp(
                textureWallWidth / std::max(1., wallLengthCentimeters),
                0.01,
                10.));
        }
        std::discrete_distribution<size_t> distribution(weights.begin(), weights.end());
        return candidates[distribution(randomEngine)];
    }

    size_t SelectWallTexture(
        const std::vector<TextureInfo>& textures,
        const BuildingGeometry& geometry,
        uint64_t height,
        double heightCentimeters,
        const DownTool::AmapBuildingFbxExportOptions& options,
        std::unordered_map<std::string, WallMaterialBucketState>& bucketStates,
        std::mt19937& randomEngine)
    {
        std::vector<size_t> candidates = BuildWallTextureCandidates(textures, height, options);
        const std::string bucketKey = MakeWallMaterialBucketKey(geometry, height, options);
        WallMaterialBucketState& state = bucketStates[bucketKey];
        std::vector<size_t> reusedCandidates;
        for (const size_t index : candidates)
        {
            if (std::find(state.textureIndices.begin(), state.textureIndices.end(), index) !=
                state.textureIndices.end())
            {
                reusedCandidates.push_back(index);
            }
        }

        const uint32_t maxMaterialCount = std::max(1u, options.maxWallMaterialsPerBucket);
        if (!reusedCandidates.empty() && state.textureIndices.size() >= maxMaterialCount)
        {
            candidates = std::move(reusedCandidates);
        }

        const size_t selectedIndex = SelectWeightedWallTexture(
            textures,
            candidates,
            heightCentimeters,
            geometry.wallLengthCentimeters,
            randomEngine);
        if (std::find(state.textureIndices.begin(), state.textureIndices.end(), selectedIndex) ==
            state.textureIndices.end() && state.textureIndices.size() < maxMaterialCount)
        {
            state.textureIndices.push_back(selectedIndex);
        }
        return selectedIndex;
    }

    size_t SelectRoofTexture(
        const std::vector<TextureInfo>& textures,
        const BuildingGeometry& geometry,
        std::mt19937& randomEngine)
    {
        const double footprintWidth = std::max(1., geometry.bounds.maxX - geometry.bounds.minX);
        const double footprintHeight = std::max(1., geometry.bounds.maxY - geometry.bounds.minY);
        const double footprintAspect = footprintWidth / footprintHeight;

        double bestDifference = std::numeric_limits<double>::max();
        std::vector<size_t> candidates;
        for (size_t index = 0; index < textures.size(); index++)
        {
            const double difference = std::abs(
                std::log(std::max(1.e-6, textures[index].AspectRatio()) / footprintAspect));
            if (difference < bestDifference)
            {
                bestDifference = difference;
            }
        }
        const double threshold = std::max(bestDifference * 1.3, bestDifference + 0.05);
        for (size_t index = 0; index < textures.size(); index++)
        {
            const double difference = std::abs(
                std::log(std::max(1.e-6, textures[index].AspectRatio()) / footprintAspect));
            if (difference <= threshold)
            {
                candidates.push_back(index);
            }
        }
        if (candidates.empty())
        {
            return 0;
        }

        std::uniform_int_distribution<size_t> distribution(0, candidates.size() - 1);
        return candidates[distribution(randomEngine)];
    }

    bool ContainsPoint(const Bounds2& bounds, const Point2& point)
    {
        return point.x >= bounds.minX && point.x <= bounds.maxX &&
            point.y >= bounds.minY && point.y <= bounds.maxY;
    }

    bool IntersectsBounds(const Bounds2& first, const Bounds2& second)
    {
        return first.minX <= second.maxX && first.maxX >= second.minX &&
            first.minY <= second.maxY && first.maxY >= second.minY;
    }

    double BoundsArea(const Bounds2& bounds)
    {
        return std::max(0., bounds.maxX - bounds.minX) *
            std::max(0., bounds.maxY - bounds.minY);
    }

    MeshBucket& GetMeshBucket(
        std::map<std::filesystem::path, MeshBucket>& buckets,
        const TextureInfo& texture,
        BuildingMeshKind kind,
        bool wrapTextureU)
    {
        MeshBucket& bucket = buckets[texture.outputPath];
        if (bucket.mesh.name.empty())
        {
            bucket.mesh.name = texture.materialName;
            bucket.mesh.kind = kind;
            bucket.mesh.texturePath = texture.outputPath;
            bucket.mesh.wrapTextureU = wrapTextureU;
        }
        return bucket;
    }

    bool AppendVertex(BuildingMeshData& mesh, const BuildingVertex& vertex, uint32_t& index)
    {
        if (mesh.vertices.size() >= std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(vertex);
        return true;
    }

    void AppendTriangle(BuildingMeshData& mesh, uint32_t first, uint32_t second, uint32_t third)
    {
        mesh.indices.push_back(first);
        mesh.indices.push_back(second);
        mesh.indices.push_back(third);
    }

    void AppendWallQuad(
        const Point2& first,
        const Point2& second,
        double lowerCentimeters,
        double upperCentimeters,
        double firstU,
        double secondU,
        double lowerV,
        double upperV,
        const std::array<double, 3>& normal,
        bool useRightNormal,
        BuildingMeshData& mesh)
    {
        uint32_t bottomFirst = 0;
        uint32_t bottomSecond = 0;
        uint32_t topFirst = 0;
        uint32_t topSecond = 0;
        if (!AppendVertex(
                mesh,
                { first.x, first.y, lowerCentimeters, firstU, lowerV, normal },
                bottomFirst) ||
            !AppendVertex(
                mesh,
                { second.x, second.y, lowerCentimeters, secondU, lowerV, normal },
                bottomSecond) ||
            !AppendVertex(
                mesh,
                { first.x, first.y, upperCentimeters, firstU, upperV, normal },
                topFirst) ||
            !AppendVertex(
                mesh,
                { second.x, second.y, upperCentimeters, secondU, upperV, normal },
                topSecond))
        {
            return;
        }

        if (useRightNormal)
        {
            AppendTriangle(mesh, bottomFirst, bottomSecond, topFirst);
            AppendTriangle(mesh, topFirst, bottomSecond, topSecond);
        }
        else
        {
            AppendTriangle(mesh, bottomFirst, topFirst, bottomSecond);
            AppendTriangle(mesh, topFirst, topSecond, bottomSecond);
        }
    }

    void AppendWallRingGeometry(
        const Ring& ring,
        bool hole,
        double heightCentimeters,
        double bottomCentimeters,
        double topCentimeters,
        const TextureInfo& texture,
        BuildingMeshData& mesh)
    {
        const double signedArea = SignedArea(ring);
        const bool useRightNormal = (signedArea > 0.) != hole;
        const double textureWallWidth = heightCentimeters /
            std::max(1., static_cast<double>(texture.height)) *
            std::max(1., static_cast<double>(texture.width));
        double currentU = 0.;
        for (size_t index = 0; index < ring.size(); index++)
        {
            const Point2& first = ring[index];
            const Point2& second = ring[(index + 1) % ring.size()];
            const double segmentLength = Distance(first, second);
            if (segmentLength <= 1.e-6)
            {
                continue;
            }

            const double dx = (second.x - first.x) / segmentLength;
            const double dy = (second.y - first.y) / segmentLength;
            const std::array<double, 3> normal = useRightNormal ?
                std::array<double, 3>{ dy, -dx, 0. } :
                std::array<double, 3>{ -dy, dx, 0. };
            const double nextU = currentU + segmentLength / std::max(1., textureWallWidth);

            AppendWallQuad(
                first,
                second,
                bottomCentimeters,
                topCentimeters,
                currentU,
                nextU,
                1.,
                0.,
                normal,
                useRightNormal,
                mesh);
            currentU = nextU;
        }
    }

    void AppendWallGeometry(
        const BuildingGeometry& geometry,
        double heightCentimeters,
        double bottomCentimeters,
        double topCentimeters,
        const TextureInfo& texture,
        BuildingMeshData& mesh)
    {
        for (const BuildingPolygon& polygon : geometry.polygons)
        {
            for (const PolygonRing& polygonRing : polygon.rings)
            {
                AppendWallRingGeometry(
                    polygonRing.ring,
                    polygonRing.hole,
                    heightCentimeters,
                    bottomCentimeters,
                    topCentimeters,
                    texture,
                    mesh);
            }
        }
    }

    double TriangleCrossZ(const std::vector<BuildingVertex>& vertices,
                          uint32_t first,
                          uint32_t second,
                          uint32_t third)
    {
        const BuildingVertex& a = vertices[first];
        const BuildingVertex& b = vertices[second];
        const BuildingVertex& c = vertices[third];
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    void AppendRoofGeometry(
        const BuildingGeometry& geometry,
        double topCentimeters,
        const Bounds2& uvBounds,
        BuildingMeshData& mesh)
    {
        const double footprintWidth = std::max(1., uvBounds.maxX - uvBounds.minX);
        const double footprintHeight = std::max(1., uvBounds.maxY - uvBounds.minY);
        for (const BuildingPolygon& polygon : geometry.polygons)
        {
            const size_t vertexOffset = mesh.vertices.size();
            std::vector<std::vector<std::array<double, 2>>> earcutPolygon;
            earcutPolygon.reserve(polygon.rings.size());
            for (const PolygonRing& polygonRing : polygon.rings)
            {
                std::vector<std::array<double, 2>> earcutRing;
                earcutRing.reserve(polygonRing.ring.size());
                for (const Point2& point : polygonRing.ring)
                {
                    earcutRing.push_back({ point.x, point.y });
                    mesh.vertices.push_back({
                        point.x,
                        point.y,
                        topCentimeters,
                        (point.x - uvBounds.minX) / footprintWidth,
                        1. - (point.y - uvBounds.minY) / footprintHeight,
                        { 0., 0., 1. }
                    });
                }
                earcutPolygon.push_back(std::move(earcutRing));
            }

            const std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(earcutPolygon);
            for (size_t index = 0; index + 2 < indices.size(); index += 3)
            {
                uint32_t first = static_cast<uint32_t>(vertexOffset) + indices[index];
                uint32_t second = static_cast<uint32_t>(vertexOffset) + indices[index + 1];
                uint32_t third = static_cast<uint32_t>(vertexOffset) + indices[index + 2];
                if (TriangleCrossZ(mesh.vertices, first, second, third) < 0.)
                {
                    std::swap(second, third);
                }
                AppendTriangle(mesh, first, second, third);
            }
        }
    }

    void AppendRoofGeometry(
        const BuildingGeometry& geometry,
        double topCentimeters,
        BuildingMeshData& mesh)
    {
        AppendRoofGeometry(geometry, topCentimeters, geometry.bounds, mesh);
    }

    bool CopyTexture(
        const TextureInfo& texture,
        std::set<std::filesystem::path>& copiedTextures,
        std::string& error)
    {
        if (copiedTextures.contains(texture.outputPath))
        {
            return true;
        }
        if (std::filesystem::absolute(texture.sourcePath).lexically_normal() ==
            std::filesystem::absolute(texture.outputPath).lexically_normal())
        {
            copiedTextures.insert(texture.outputPath);
            return true;
        }

        std::error_code createError;
        std::filesystem::create_directories(texture.outputPath.parent_path(), createError);
        if (createError)
        {
            error = "无法创建建筑 FBX 贴图目录：" + createError.message();
            return false;
        }

        std::error_code copyError;
        std::filesystem::copy_file(
            texture.sourcePath,
            texture.outputPath,
            std::filesystem::copy_options::overwrite_existing,
            copyError);
        if (copyError)
        {
            error = "无法复制建筑贴图：" + PathToUtf8String(texture.sourcePath) +
                " -> " + PathToUtf8String(texture.outputPath) + "：" + copyError.message();
            return false;
        }
        copiedTextures.insert(texture.outputPath);
        return true;
    }

    enum class RectClipEdge
    {
        Left,
        Right,
        Bottom,
        Top
    };

    bool PointsNearlyEqual(const Point2& first, const Point2& second)
    {
        constexpr double epsilon = 1.e-4;
        return std::abs(first.x - second.x) <= epsilon &&
            std::abs(first.y - second.y) <= epsilon;
    }

    Ring CleanClippedRing(Ring ring)
    {
        Ring cleaned;
        cleaned.reserve(ring.size());
        for (const Point2& point : ring)
        {
            if (cleaned.empty() || !PointsNearlyEqual(cleaned.back(), point))
            {
                cleaned.push_back(point);
            }
        }
        while (cleaned.size() > 1 && PointsNearlyEqual(cleaned.front(), cleaned.back()))
        {
            cleaned.pop_back();
        }
        if (cleaned.size() < 3 || std::abs(SignedArea(cleaned)) <= 1.e-4)
        {
            cleaned.clear();
        }
        return cleaned;
    }

    bool IsInsideClipEdge(const Point2& point, const Bounds2& bounds, RectClipEdge edge)
    {
        constexpr double epsilon = 1.e-4;
        switch (edge)
        {
        case RectClipEdge::Left:
            return point.x >= bounds.minX - epsilon;
        case RectClipEdge::Right:
            return point.x <= bounds.maxX + epsilon;
        case RectClipEdge::Bottom:
            return point.y >= bounds.minY - epsilon;
        case RectClipEdge::Top:
            return point.y <= bounds.maxY + epsilon;
        }
        return false;
    }

    Point2 ClipSegmentIntersection(
        const Point2& start,
        const Point2& end,
        const Bounds2& bounds,
        RectClipEdge edge)
    {
        constexpr double epsilon = 1.e-9;
        if (edge == RectClipEdge::Left || edge == RectClipEdge::Right)
        {
            const double clipX = edge == RectClipEdge::Left ? bounds.minX : bounds.maxX;
            const double denominator = end.x - start.x;
            if (std::abs(denominator) <= epsilon)
            {
                return { clipX, start.y };
            }
            const double t = std::clamp((clipX - start.x) / denominator, 0., 1.);
            return { clipX, start.y + (end.y - start.y) * t };
        }

        const double clipY = edge == RectClipEdge::Bottom ? bounds.minY : bounds.maxY;
        const double denominator = end.y - start.y;
        if (std::abs(denominator) <= epsilon)
        {
            return { start.x, clipY };
        }
        const double t = std::clamp((clipY - start.y) / denominator, 0., 1.);
        return { start.x + (end.x - start.x) * t, clipY };
    }

    Ring ClipRingByEdge(const Ring& input, const Bounds2& bounds, RectClipEdge edge)
    {
        if (input.empty())
        {
            return {};
        }

        Ring output;
        output.reserve(input.size() + 1);
        Point2 previous = input.back();
        bool previousInside = IsInsideClipEdge(previous, bounds, edge);
        for (const Point2& current : input)
        {
            const bool currentInside = IsInsideClipEdge(current, bounds, edge);
            if (currentInside)
            {
                if (!previousInside)
                {
                    output.push_back(ClipSegmentIntersection(previous, current, bounds, edge));
                }
                output.push_back(current);
            }
            else if (previousInside)
            {
                output.push_back(ClipSegmentIntersection(previous, current, bounds, edge));
            }

            previous = current;
            previousInside = currentInside;
        }
        return CleanClippedRing(std::move(output));
    }

    Ring ClipRingToBounds(Ring ring, const Bounds2& bounds)
    {
        for (const RectClipEdge edge : {
                 RectClipEdge::Left,
                 RectClipEdge::Right,
                 RectClipEdge::Bottom,
                 RectClipEdge::Top})
        {
            ring = ClipRingByEdge(ring, bounds, edge);
            if (ring.empty())
            {
                return {};
            }
        }
        return ring;
    }

    Bounds2 BuildTriangleBounds(const std::array<Point2, 3>& triangle)
    {
        Bounds2 bounds;
        for (const Point2& point : triangle)
        {
            bounds.Add(point);
        }
        return bounds;
    }

    std::optional<size_t> SelectTextureContainingTriangle(
        const std::vector<SatelliteRoofTextureInfo>& textures,
        const std::array<Point2, 3>& triangle)
    {
        std::optional<size_t> selectedIndex;
        double selectedArea = std::numeric_limits<double>::max();
        for (size_t index = 0; index < textures.size(); index++)
        {
            const Bounds2& bounds = textures[index].planeBounds;
            if (!ContainsPoint(bounds, triangle[0]) || !ContainsPoint(bounds, triangle[1]) ||
                !ContainsPoint(bounds, triangle[2]))
            {
                continue;
            }
            const double area = BoundsArea(bounds);
            if (area < selectedArea)
            {
                selectedArea = area;
                selectedIndex = index;
            }
        }
        return selectedIndex;
    }

    bool AppendSatelliteRoofPolygon(
        const Ring& polygon,
        double topCentimeters,
        const SatelliteRoofTextureInfo& roofTexture,
        std::map<std::filesystem::path, MeshBucket>& roofMeshBuckets,
        std::set<std::filesystem::path>& copiedTextures,
        bool& appended,
        std::string& error)
    {
        const Ring cleanPolygon = CleanClippedRing(polygon);
        if (cleanPolygon.empty())
        {
            return true;
        }
        if (!CopyTexture(roofTexture.texture, copiedTextures, error))
        {
            return false;
        }

        BuildingMeshData& mesh = GetMeshBucket(
            roofMeshBuckets,
            roofTexture.texture,
            BuildingMeshKind::Roof,
            false).mesh;
        const double width = std::max(1., roofTexture.planeBounds.maxX -
                                          roofTexture.planeBounds.minX);
        const double height = std::max(1., roofTexture.planeBounds.maxY -
                                           roofTexture.planeBounds.minY);
        std::vector<uint32_t> vertexIndices;
        vertexIndices.reserve(cleanPolygon.size());
        for (const Point2& point : cleanPolygon)
        {
            uint32_t vertexIndex = 0;
            if (!AppendVertex(
                    mesh,
                    { point.x,
                      point.y,
                      topCentimeters,
                      (point.x - roofTexture.planeBounds.minX) / width,
                      (point.y - roofTexture.planeBounds.minY) / height,
                      { 0., 0., 1. } },
                    vertexIndex))
            {
                error = "卫星屋顶 FBX 顶点数量超过索引上限。";
                return false;
            }
            vertexIndices.push_back(vertexIndex);
        }

        for (size_t index = 1; index + 1 < vertexIndices.size(); index++)
        {
            uint32_t first = vertexIndices[0];
            uint32_t second = vertexIndices[index];
            uint32_t third = vertexIndices[index + 1];
            const double cross = TriangleCrossZ(mesh.vertices, first, second, third);
            if (std::abs(cross) <= 1.e-4)
            {
                continue;
            }
            if (cross < 0.)
            {
                std::swap(second, third);
            }
            AppendTriangle(mesh, first, second, third);
            appended = true;
        }
        return true;
    }

    bool AppendSatelliteRoofTriangle(
        const std::array<Point2, 3>& triangle,
        double topCentimeters,
        const std::vector<SatelliteRoofTextureInfo>& roofTextures,
        std::map<std::filesystem::path, MeshBucket>& roofMeshBuckets,
        std::set<std::filesystem::path>& copiedTextures,
        std::string& error)
    {
        const Ring triangleRing = { triangle[0], triangle[1], triangle[2] };
        bool appended = false;
        const std::optional<size_t> containingTextureIndex = SelectTextureContainingTriangle(
            roofTextures,
            triangle);
        if (containingTextureIndex)
        {
            return AppendSatelliteRoofPolygon(
                triangleRing,
                topCentimeters,
                roofTextures[*containingTextureIndex],
                roofMeshBuckets,
                copiedTextures,
                appended,
                error);
        }

        const Bounds2 triangleBounds = BuildTriangleBounds(triangle);
        for (const SatelliteRoofTextureInfo& roofTexture : roofTextures)
        {
            if (!IntersectsBounds(roofTexture.planeBounds, triangleBounds))
            {
                continue;
            }
            const Ring clippedPolygon = ClipRingToBounds(triangleRing, roofTexture.planeBounds);
            if (!AppendSatelliteRoofPolygon(
                    clippedPolygon,
                    topCentimeters,
                    roofTexture,
                    roofMeshBuckets,
                    copiedTextures,
                    appended,
                    error))
            {
                return false;
            }
        }
        if (!appended)
        {
            error = "建筑屋顶没有匹配的卫星 FBX 贴图范围。";
            return false;
        }
        return true;
    }

    bool AppendSatelliteRoofGeometry(
        const BuildingGeometry& geometry,
        double topCentimeters,
        const std::vector<SatelliteRoofTextureInfo>& roofTextures,
        std::map<std::filesystem::path, MeshBucket>& roofMeshBuckets,
        std::set<std::filesystem::path>& copiedTextures,
        std::string& error)
    {
        for (const BuildingPolygon& polygon : geometry.polygons)
        {
            std::vector<Point2> sourcePoints;
            std::vector<std::vector<std::array<double, 2>>> earcutPolygon;
            earcutPolygon.reserve(polygon.rings.size());
            for (const PolygonRing& polygonRing : polygon.rings)
            {
                std::vector<std::array<double, 2>> earcutRing;
                earcutRing.reserve(polygonRing.ring.size());
                for (const Point2& point : polygonRing.ring)
                {
                    sourcePoints.push_back(point);
                    earcutRing.push_back({ point.x, point.y });
                }
                earcutPolygon.push_back(std::move(earcutRing));
            }

            const std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(earcutPolygon);
            for (size_t index = 0; index + 2 < indices.size(); index += 3)
            {
                if (indices[index] >= sourcePoints.size() ||
                    indices[index + 1] >= sourcePoints.size() ||
                    indices[index + 2] >= sourcePoints.size())
                {
                    error = "卫星屋顶三角化索引越界。";
                    return false;
                }
                const std::array<Point2, 3> triangle{
                    sourcePoints[indices[index]],
                    sourcePoints[indices[index + 1]],
                    sourcePoints[indices[index + 2]]
                };
                if (!AppendSatelliteRoofTriangle(
                        triangle,
                        topCentimeters,
                        roofTextures,
                        roofMeshBuckets,
                        copiedTextures,
                        error))
                {
                    return false;
                }
            }
        }
        return true;
    }

    cv::Mat ConvertAtlasImage(const cv::Mat& image)
    {
        if (image.empty())
        {
            return {};
        }
        cv::Mat converted;
        if (image.channels() == 4)
        {
            cv::cvtColor(image, converted, cv::COLOR_BGRA2BGR);
        }
        else if (image.channels() == 1)
        {
            cv::cvtColor(image, converted, cv::COLOR_GRAY2BGR);
        }
        else if (image.channels() == 3)
        {
            converted = image;
        }
        else
        {
            return {};
        }

        if (converted.depth() != CV_8U)
        {
            cv::Mat normalized;
            converted.convertTo(normalized, CV_8U);
            return normalized;
        }
        return converted.clone();
    }

    int32_t NextPowerOfTwo(int32_t value)
    {
        value = std::max(1, value);
        int32_t result = 1;
        while (result < value && result <= (std::numeric_limits<int32_t>::max() / 2))
        {
            result *= 2;
        }
        return result;
    }

    std::unordered_map<std::string, const TextureInfo*> BuildTextureLookup(
        const std::vector<TextureInfo>& textures)
    {
        std::unordered_map<std::string, const TextureInfo*> lookup;
        lookup.reserve(textures.size());
        for (const TextureInfo& texture : textures)
        {
            lookup.emplace(PathToUtf8String(texture.outputPath), &texture);
        }
        return lookup;
    }

    std::vector<const TextureInfo*> CollectUsedTextures(
        const std::map<std::filesystem::path, MeshBucket>& meshBuckets,
        const std::vector<TextureInfo>& textures)
    {
        const std::unordered_map<std::string, const TextureInfo*> lookup =
            BuildTextureLookup(textures);
        std::vector<const TextureInfo*> usedTextures;
        for (const auto& [texturePath, bucket] : meshBuckets)
        {
            if (bucket.mesh.vertices.empty() || bucket.mesh.indices.empty())
            {
                continue;
            }
            const auto found = lookup.find(PathToUtf8String(texturePath));
            if (found != lookup.end())
            {
                usedTextures.push_back(found->second);
            }
        }
        return usedTextures;
    }

    bool BuildAtlasSources(
        const std::vector<const TextureInfo*>& textures,
        std::vector<AtlasSource>& sources,
        std::string& error)
    {
        sources.clear();
        sources.reserve(textures.size());
        for (const TextureInfo* texture : textures)
        {
            cv::Mat image = ConvertAtlasImage(ReadTextureImage(texture->sourcePath));
            if (image.empty() || image.cols <= 0 || image.rows <= 0)
            {
                error = "无法读取待合并建筑贴图：" + PathToUtf8String(texture->sourcePath);
                return false;
            }
            const int32_t width = image.cols;
            const int32_t height = image.rows;
            sources.push_back({ texture, std::move(image), 0, 0, width, height });
        }
        std::sort(
            sources.begin(),
            sources.end(),
            [](const AtlasSource& left, const AtlasSource& right)
            {
                const int64_t leftArea = static_cast<int64_t>(left.width) * left.height;
                const int64_t rightArea = static_cast<int64_t>(right.width) * right.height;
                if (leftArea != rightArea)
                {
                    return leftArea > rightArea;
                }
                const int32_t leftMaxSide = std::max(left.width, left.height);
                const int32_t rightMaxSide = std::max(right.width, right.height);
                if (leftMaxSide != rightMaxSide)
                {
                    return leftMaxSide > rightMaxSide;
                }
                if (left.height != right.height)
                {
                    return left.height > right.height;
                }
                return PathToUtf8String(left.texture->sourcePath) <
                    PathToUtf8String(right.texture->sourcePath);
            });
        return true;
    }

    bool ContainsRect(const AtlasPackingRect& outer, const AtlasPackingRect& inner)
    {
        return inner.x >= outer.x && inner.y >= outer.y &&
            inner.x + inner.width <= outer.x + outer.width &&
            inner.y + inner.height <= outer.y + outer.height;
    }

    bool IntersectsRect(const AtlasPackingRect& first, const AtlasPackingRect& second)
    {
        return first.x < second.x + second.width && first.x + first.width > second.x &&
            first.y < second.y + second.height && first.y + first.height > second.y;
    }

    void PruneFreeAtlasRects(std::vector<AtlasPackingRect>& freeRects)
    {
        for (size_t index = 0; index < freeRects.size();)
        {
            bool removed = freeRects[index].width <= 0 || freeRects[index].height <= 0;
            for (size_t otherIndex = 0; !removed && otherIndex < freeRects.size();
                 otherIndex++)
            {
                if (index != otherIndex && ContainsRect(freeRects[otherIndex], freeRects[index]))
                {
                    removed = true;
                }
            }
            if (removed)
            {
                freeRects.erase(freeRects.begin() + static_cast<std::ptrdiff_t>(index));
            }
            else
            {
                index++;
            }
        }
    }

    void SplitFreeAtlasRects(
        std::vector<AtlasPackingRect>& freeRects,
        const AtlasPackingRect& placedRect)
    {
        std::vector<AtlasPackingRect> nextFreeRects;
        nextFreeRects.reserve(freeRects.size() + 4);
        for (const AtlasPackingRect& freeRect : freeRects)
        {
            if (!IntersectsRect(freeRect, placedRect))
            {
                nextFreeRects.push_back(freeRect);
                continue;
            }

            const int32_t freeRight = freeRect.x + freeRect.width;
            const int32_t freeBottom = freeRect.y + freeRect.height;
            const int32_t placedRight = placedRect.x + placedRect.width;
            const int32_t placedBottom = placedRect.y + placedRect.height;
            if (placedRect.x > freeRect.x)
            {
                nextFreeRects.push_back({
                    freeRect.x,
                    freeRect.y,
                    placedRect.x - freeRect.x,
                    freeRect.height});
            }
            if (placedRight < freeRight)
            {
                nextFreeRects.push_back({
                    placedRight,
                    freeRect.y,
                    freeRight - placedRight,
                    freeRect.height});
            }
            if (placedRect.y > freeRect.y)
            {
                nextFreeRects.push_back({
                    freeRect.x,
                    freeRect.y,
                    freeRect.width,
                    placedRect.y - freeRect.y});
            }
            if (placedBottom < freeBottom)
            {
                nextFreeRects.push_back({
                    freeRect.x,
                    placedBottom,
                    freeRect.width,
                    freeBottom - placedBottom});
            }
        }
        freeRects = std::move(nextFreeRects);
        PruneFreeAtlasRects(freeRects);
    }

    bool TryPackAtlasSources(
        const std::vector<AtlasSource>& sources,
        int32_t padding,
        int32_t atlasWidth,
        int32_t atlasHeight,
        std::vector<AtlasSource>& packedSources)
    {
        std::vector<AtlasPackingRect> freeRects = {{0, 0, atlasWidth, atlasHeight}};
        packedSources = sources;
        for (AtlasSource& source : packedSources)
        {
            const int32_t paddedWidth = source.width + padding * 2;
            const int32_t paddedHeight = source.height + padding * 2;
            std::optional<size_t> bestFreeRectIndex;
            int32_t bestShortSide = std::numeric_limits<int32_t>::max();
            int32_t bestLongSide = std::numeric_limits<int32_t>::max();
            for (size_t index = 0; index < freeRects.size(); index++)
            {
                const AtlasPackingRect& freeRect = freeRects[index];
                if (paddedWidth > freeRect.width || paddedHeight > freeRect.height)
                {
                    continue;
                }
                const int32_t leftoverWidth = freeRect.width - paddedWidth;
                const int32_t leftoverHeight = freeRect.height - paddedHeight;
                const int32_t shortSide = std::min(leftoverWidth, leftoverHeight);
                const int32_t longSide = std::max(leftoverWidth, leftoverHeight);
                if (!bestFreeRectIndex || shortSide < bestShortSide ||
                    (shortSide == bestShortSide && longSide < bestLongSide))
                {
                    bestFreeRectIndex = index;
                    bestShortSide = shortSide;
                    bestLongSide = longSide;
                }
            }
            if (!bestFreeRectIndex)
            {
                return false;
            }

            const AtlasPackingRect freeRect = freeRects[*bestFreeRectIndex];
            const AtlasPackingRect placedRect{
                freeRect.x,
                freeRect.y,
                paddedWidth,
                paddedHeight};
            source.x = placedRect.x + padding;
            source.y = placedRect.y + padding;
            SplitFreeAtlasRects(freeRects, placedRect);
        }
        return true;
    }

    bool PackAtlasSources(std::vector<AtlasSource>& sources, int32_t padding,
                          int32_t& atlasWidth, int32_t& atlasHeight)
    {
        if (sources.empty())
        {
            atlasWidth = 0;
            atlasHeight = 0;
            return true;
        }

        int64_t paddedArea = 0;
        int32_t maximumPaddedWidth = 1;
        int32_t maximumPaddedHeight = 1;
        for (const AtlasSource& source : sources)
        {
            const int32_t paddedWidth = source.width + padding * 2;
            const int32_t paddedHeight = source.height + padding * 2;
            paddedArea += static_cast<int64_t>(paddedWidth) * paddedHeight;
            maximumPaddedWidth = std::max(maximumPaddedWidth, paddedWidth);
            maximumPaddedHeight = std::max(maximumPaddedHeight, paddedHeight);
        }

        constexpr int32_t maximumAtlasSize = 16384;
        const int32_t minimumWidth = NextPowerOfTwo(maximumPaddedWidth);
        const int32_t minimumHeight = NextPowerOfTwo(maximumPaddedHeight);
        if (minimumWidth > maximumAtlasSize || minimumHeight > maximumAtlasSize)
        {
            return false;
        }

        int64_t bestArea = std::numeric_limits<int64_t>::max();
        int32_t bestMaximumSide = std::numeric_limits<int32_t>::max();
        int32_t bestAspectDelta = std::numeric_limits<int32_t>::max();
        std::vector<AtlasSource> bestPackedSources;
        for (int32_t candidateWidth = minimumWidth; candidateWidth <= maximumAtlasSize;
             candidateWidth *= 2)
        {
            for (int32_t candidateHeight = minimumHeight; candidateHeight <= maximumAtlasSize;
                 candidateHeight *= 2)
            {
                const int64_t candidateArea =
                    static_cast<int64_t>(candidateWidth) * candidateHeight;
                if (candidateArea < paddedArea || candidateArea > bestArea)
                {
                    continue;
                }

                std::vector<AtlasSource> packedSources;
                if (!TryPackAtlasSources(
                        sources,
                        padding,
                        candidateWidth,
                        candidateHeight,
                        packedSources))
                {
                    continue;
                }

                const int32_t candidateMaximumSide =
                    std::max(candidateWidth, candidateHeight);
                const int32_t candidateAspectDelta = std::abs(candidateWidth - candidateHeight);
                if (candidateArea < bestArea ||
                    (candidateArea == bestArea && candidateMaximumSide < bestMaximumSide) ||
                    (candidateArea == bestArea && candidateMaximumSide == bestMaximumSide &&
                     candidateAspectDelta < bestAspectDelta))
                {
                    bestArea = candidateArea;
                    bestMaximumSide = candidateMaximumSide;
                    bestAspectDelta = candidateAspectDelta;
                    atlasWidth = candidateWidth;
                    atlasHeight = candidateHeight;
                    bestPackedSources = std::move(packedSources);
                }
            }
        }
        if (bestPackedSources.empty())
        {
            return false;
        }
        sources = std::move(bestPackedSources);
        return true;
    }

    bool WriteTextureAtlas(
        std::vector<AtlasSource>& sources,
        const std::filesystem::path& outputPath,
        std::unordered_map<std::string, AtlasRect>& atlasRects,
        std::string& error)
    {
        constexpr int32_t padding = 2;
        int32_t atlasWidth = 0;
        int32_t atlasHeight = 0;
        if (!PackAtlasSources(sources, padding, atlasWidth, atlasHeight))
        {
            error = "建筑贴图合并失败：atlas 尺寸无效。";
            return false;
        }

        cv::Mat atlas(atlasHeight, atlasWidth, CV_8UC3, cv::Scalar(0, 0, 0));
        atlasRects.clear();
        atlasRects.reserve(sources.size());
        for (const AtlasSource& source : sources)
        {
            cv::Mat paddedImage;
            cv::copyMakeBorder(
                source.image,
                paddedImage,
                padding,
                padding,
                padding,
                padding,
                cv::BORDER_REPLICATE);
            paddedImage.copyTo(atlas(cv::Rect(
                source.x - padding,
                source.y - padding,
                paddedImage.cols,
                paddedImage.rows)));
            atlasRects.emplace(
                PathToUtf8String(source.texture->outputPath),
                AtlasRect{
                    static_cast<double>(source.x) / static_cast<double>(atlasWidth),
                    1. - static_cast<double>(source.y + source.height) /
                        static_cast<double>(atlasHeight),
                    static_cast<double>(source.x + source.width) /
                        static_cast<double>(atlasWidth),
                    1. - static_cast<double>(source.y) / static_cast<double>(atlasHeight)
                });
        }

        if (!WriteEncodedImage(outputPath, LowercaseExtension(outputPath), atlas))
        {
            error = "建筑合并贴图写入失败：" + PathToUtf8String(outputPath);
            return false;
        }
        return true;
    }

    BuildingVertex AddAtlasTransformToVertex(const BuildingVertex& vertex, const AtlasRect& rect)
    {
        BuildingVertex result = vertex;
        result.atlasOffset = { rect.u0, rect.v0 };
        result.atlasMax = { rect.u1, rect.v1 };
        return result;
    }

    bool AppendAtlasBucketMesh(
        const MeshBucket& bucket,
        const AtlasRect& rect,
        BuildingMeshData& atlasMesh)
    {
        if (bucket.mesh.vertices.empty() || bucket.mesh.indices.empty())
        {
            return true;
        }
        if (atlasMesh.vertices.size() + bucket.mesh.vertices.size() >=
            std::numeric_limits<uint32_t>::max())
        {
            return false;
        }
        const uint32_t vertexOffset = static_cast<uint32_t>(atlasMesh.vertices.size());
        atlasMesh.vertices.reserve(atlasMesh.vertices.size() + bucket.mesh.vertices.size());
        for (const BuildingVertex& vertex : bucket.mesh.vertices)
        {
            atlasMesh.vertices.push_back(AddAtlasTransformToVertex(vertex, rect));
        }
        atlasMesh.indices.reserve(atlasMesh.indices.size() + bucket.mesh.indices.size());
        for (const uint32_t index : bucket.mesh.indices)
        {
            atlasMesh.indices.push_back(vertexOffset + index);
        }
        return true;
    }

    bool BuildAtlasMesh(
        const std::map<std::filesystem::path, MeshBucket>& meshBuckets,
        const std::vector<TextureInfo>& textures,
        const std::filesystem::path& atlasPath,
        const std::string& materialName,
        BuildingMeshKind meshKind,
        BuildingMeshData& atlasMesh,
        std::string& error)
    {
        std::vector<AtlasSource> sources;
        if (!BuildAtlasSources(CollectUsedTextures(meshBuckets, textures), sources, error))
        {
            return false;
        }
        if (sources.empty())
        {
            return true;
        }

        std::unordered_map<std::string, AtlasRect> atlasRects;
        if (!WriteTextureAtlas(sources, atlasPath, atlasRects, error))
        {
            return false;
        }

        atlasMesh.name = materialName;
        atlasMesh.kind = meshKind;
        atlasMesh.texturePath = atlasPath;
        atlasMesh.wrapTextureU = false;
        atlasMesh.usesAtlasTransform = true;
        for (const auto& [texturePath, bucket] : meshBuckets)
        {
            const auto rect = atlasRects.find(PathToUtf8String(texturePath));
            if (rect == atlasRects.end())
            {
                continue;
            }
            if (!AppendAtlasBucketMesh(bucket, rect->second, atlasMesh))
            {
                error = "建筑合并贴图网格顶点数量超过 FBX 索引上限。";
                return false;
            }
        }
        return true;
    }

    aiMesh* CreateAssimpMesh(const BuildingMeshData& meshData, unsigned int materialIndex)
    {
        aiMesh* mesh = new aiMesh();
        mesh->mName = aiString(meshData.name);
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        mesh->mMaterialIndex = materialIndex;
        mesh->mNumVertices = static_cast<unsigned int>(meshData.vertices.size());
        mesh->mVertices = new aiVector3D[mesh->mNumVertices];
        mesh->mNormals = new aiVector3D[mesh->mNumVertices];
        mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
        mesh->mNumUVComponents[0] = 2;
        if (meshData.usesAtlasTransform)
        {
            mesh->mTextureCoords[1] = new aiVector3D[mesh->mNumVertices];
            mesh->mTextureCoords[2] = new aiVector3D[mesh->mNumVertices];
            mesh->mNumUVComponents[1] = 2;
            mesh->mNumUVComponents[2] = 2;
        }
        for (unsigned int index = 0; index < mesh->mNumVertices; index++)
        {
            const BuildingVertex& vertex = meshData.vertices[index];
            mesh->mVertices[index] = aiVector3D(
                static_cast<float>(vertex.x),
                static_cast<float>(vertex.y),
                static_cast<float>(vertex.z));
            mesh->mNormals[index] = aiVector3D(
                static_cast<float>(vertex.normal[0]),
                static_cast<float>(vertex.normal[1]),
                static_cast<float>(vertex.normal[2]));
            mesh->mTextureCoords[0][index] = aiVector3D(
                static_cast<float>(vertex.u),
                static_cast<float>(vertex.v),
                0.f);
            if (meshData.usesAtlasTransform)
            {
                mesh->mTextureCoords[1][index] = aiVector3D(
                    static_cast<float>(vertex.atlasOffset[0]),
                    static_cast<float>(vertex.atlasOffset[1]),
                    0.f);
                mesh->mTextureCoords[2][index] = aiVector3D(
                    static_cast<float>(vertex.atlasMax[0]),
                    static_cast<float>(vertex.atlasMax[1]),
                    0.f);
            }
        }

        mesh->mNumFaces = static_cast<unsigned int>(meshData.indices.size() / 3);
        mesh->mFaces = new aiFace[mesh->mNumFaces];
        for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; faceIndex++)
        {
            aiFace& face = mesh->mFaces[faceIndex];
            face.mNumIndices = 3;
            face.mIndices = new unsigned int[3];
            face.mIndices[0] = meshData.indices[faceIndex * 3];
            face.mIndices[1] = meshData.indices[faceIndex * 3 + 1];
            face.mIndices[2] = meshData.indices[faceIndex * 3 + 2];
        }
        return mesh;
    }

    aiMaterial* CreateAssimpMaterial(
        const BuildingMeshData& meshData,
        const std::filesystem::path& fbxPath)
    {
        aiMaterial* material = new aiMaterial();
        const aiString materialName(meshData.name);
        material->AddProperty(&materialName, AI_MATKEY_NAME);

        const std::string texturePath = ToPortableRelativePath(
            meshData.texturePath,
            fbxPath.parent_path());
        const aiString diffuseTexture(texturePath);
        material->AddProperty(&diffuseTexture, AI_MATKEY_TEXTURE_DIFFUSE(0));

        const aiTextureMapMode mapModeU = meshData.wrapTextureU ?
            aiTextureMapMode_Wrap : aiTextureMapMode_Clamp;
        const aiTextureMapMode mapModeV = aiTextureMapMode_Clamp;
        material->AddProperty(&mapModeU, 1, AI_MATKEY_MAPPINGMODE_U_DIFFUSE(0));
        material->AddProperty(&mapModeV, 1, AI_MATKEY_MAPPINGMODE_V_DIFFUSE(0));

        aiColor3D diffuseColor(1.f, 1.f, 1.f);
        material->AddProperty(&diffuseColor, 1, AI_MATKEY_COLOR_DIFFUSE);
        return material;
    }

    void SetAssimpFbxGlobalSettings(aiScene& scene)
    {
        scene.mMetaData = aiMetadata::Alloc(10);
        scene.mMetaData->Set(0, "UpAxis", int32_t{ 2 });
        scene.mMetaData->Set(1, "UpAxisSign", int32_t{ 1 });
        scene.mMetaData->Set(2, "FrontAxis", int32_t{ 1 });
        scene.mMetaData->Set(3, "FrontAxisSign", int32_t{ -1 });
        scene.mMetaData->Set(4, "CoordAxis", int32_t{ 0 });
        scene.mMetaData->Set(5, "CoordAxisSign", int32_t{ 1 });
        scene.mMetaData->Set(6, "OriginalUpAxis", int32_t{ 2 });
        scene.mMetaData->Set(7, "OriginalUpAxisSign", int32_t{ 1 });
        scene.mMetaData->Set(8, "UnitScaleFactor", 1.);
        scene.mMetaData->Set(9, "OriginalUnitScaleFactor", 1.);
    }

    bool ExportAssimpFbx(
        const std::vector<BuildingMeshData>& meshDatas,
        const std::filesystem::path& fbxPath,
        std::string& error)
    {
        if (meshDatas.empty())
        {
            error = "建筑 FBX 没有可导出的网格。";
            return false;
        }

        aiScene scene;
        SetAssimpFbxGlobalSettings(scene);
        scene.mRootNode = new aiNode("RootNode");
        scene.mNumMeshes = static_cast<unsigned int>(meshDatas.size());
        scene.mMeshes = new aiMesh * [scene.mNumMeshes];
        scene.mNumMaterials = scene.mNumMeshes;
        scene.mMaterials = new aiMaterial * [scene.mNumMaterials];
        scene.mRootNode->mNumChildren = scene.mNumMeshes;
        scene.mRootNode->mChildren = new aiNode * [scene.mRootNode->mNumChildren];

        for (unsigned int index = 0; index < scene.mNumMeshes; index++)
        {
            const BuildingMeshData& meshData = meshDatas[index];
            scene.mMeshes[index] = CreateAssimpMesh(meshData, index);
            scene.mMaterials[index] = CreateAssimpMaterial(meshData, fbxPath);

            aiNode* childNode = new aiNode(meshData.name);
            childNode->mParent = scene.mRootNode;
            childNode->mNumMeshes = 1;
            childNode->mMeshes = new unsigned int[1];
            childNode->mMeshes[0] = index;
            scene.mRootNode->mChildren[index] = childNode;
        }

        Assimp::Exporter exporter;
        const aiExportDataBlob* blob = exporter.ExportToBlob(&scene, "fbx");
        if (!blob || !blob->data || blob->size == 0)
        {
            error = "建筑 FBX 导出失败：" + std::string(exporter.GetErrorString());
            return false;
        }
        if (!WriteBinaryFile(fbxPath, blob->data, blob->size))
        {
            error = "建筑 FBX 文件写入失败：" + PathToUtf8String(fbxPath);
            return false;
        }
        return true;
    }
}

DownTool::AmapBuildingFbxExportResult DownTool::ExportAmapBuildingsToFbx(
    const AmapVectorDecodedData& decodedData,
    const AmapBuildingFbxExportOptions& options)
{
    AmapBuildingFbxExportResult result;
    result.outputPath = options.outputPath;
    if (options.outputPath.empty())
    {
        result.error = "建筑 FBX 输出路径为空。";
        return result;
    }
    if (!std::isfinite(options.centerWgs84.lon) || !std::isfinite(options.centerWgs84.lat))
    {
        result.error = "建筑 FBX 中心经纬度无效。";
        return result;
    }
    if (options.layerHeightCentimeters <= 0.)
    {
        result.error = "建筑楼层高度必须大于 0。";
        return result;
    }

    TextureLibrary textureLibrary;
    if (!LoadTextureLibrary(options, textureLibrary, result.error))
    {
        return result;
    }

    std::vector<SatelliteRoofTextureInfo> satelliteRoofTextures;
    if (!BuildSatelliteRoofTextureList(options, satelliteRoofTextures, result.error))
    {
        return result;
    }

    const bool useWallAtlas =
        options.wallTextureMode == AmapBuildingFbxTextureMode::MergedAtlas;
    const bool useRoofAtlas =
        options.roofTextureMode == AmapBuildingFbxRoofTextureMode::MergedAtlas;
    const bool useSatelliteRoof =
        options.roofTextureMode == AmapBuildingFbxRoofTextureMode::Satellite;
    std::map<std::filesystem::path, MeshBucket> wallMeshBuckets;
    std::map<std::filesystem::path, MeshBucket> roofMeshBuckets;
    std::unordered_map<std::string, WallMaterialBucketState> wallBucketStates;
    std::set<std::filesystem::path> copiedTextures;
    std::mt19937 randomEngine(options.randomSeed);

    for (const AmapVectorFeature& feature : decodedData.features)
    {
        if (options.cancellationRequested && options.cancellationRequested())
        {
            result.error = "建筑 FBX 导出已取消。";
            return result;
        }
        if (feature.geometryType != AmapVectorGeometryType::Polygon || !feature.hasHeight ||
            feature.height == 0)
        {
            continue;
        }

        const BuildingGeometry geometry = BuildBuildingGeometry(feature, options.centerWgs84);
        if (geometry.polygons.empty() || !geometry.bounds.IsValid() ||
            geometry.wallLengthCentimeters <= 1.e-6)
        {
            continue;
        }

        const double heightCentimeters =
            static_cast<double>(feature.height) * options.layerHeightCentimeters;
        const std::optional<std::pair<double, double>> terrainHeightRange =
            SampleBuildingTerrainHeightRange(feature, options);
        const double bottomCentimeters = terrainHeightRange ? terrainHeightRange->first : 0.;
        const double topCentimeters =
            (terrainHeightRange ? terrainHeightRange->second : 0.) + heightCentimeters;
        const size_t wallTextureIndex = SelectWallTexture(
            textureLibrary.wallTextures,
            geometry,
            feature.height,
            heightCentimeters,
            options,
            wallBucketStates,
            randomEngine);
        const TextureInfo& wallTexture = textureLibrary.wallTextures[wallTextureIndex];

        if (!useWallAtlas && !CopyTexture(wallTexture, copiedTextures, result.error))
        {
            return result;
        }

        AppendWallGeometry(
            geometry,
            heightCentimeters,
            bottomCentimeters,
            topCentimeters,
            wallTexture,
            GetMeshBucket(
                wallMeshBuckets, wallTexture, BuildingMeshKind::Wall, !useWallAtlas).mesh);
        if (useSatelliteRoof)
        {
            if (!AppendSatelliteRoofGeometry(
                geometry,
                topCentimeters,
                satelliteRoofTextures,
                roofMeshBuckets,
                copiedTextures,
                result.error))
            {
                return result;
            }
        }
        else
        {
            const size_t roofTextureIndex = SelectRoofTexture(
                textureLibrary.roofTextures,
                geometry,
                randomEngine);
            const TextureInfo& roofTexture = textureLibrary.roofTextures[roofTextureIndex];
            if (!useRoofAtlas && !CopyTexture(roofTexture, copiedTextures, result.error))
            {
                return result;
            }
            AppendRoofGeometry(
                geometry,
                topCentimeters,
                GetMeshBucket(
                    roofMeshBuckets,
                    roofTexture,
                    BuildingMeshKind::Roof,
                    false).mesh);
        }
        result.buildingCount++;
    }

    std::vector<BuildingMeshData> meshDatas;
    if (useWallAtlas || useRoofAtlas)
    {
        meshDatas.reserve(2);
        const std::filesystem::path outputDirectory = options.outputPath.parent_path();
        const std::string atlasExtension = AtlasImageExtension(options.atlasImageFormat);
        if (useWallAtlas)
        {
            BuildingMeshData wallAtlasMesh;
            if (!BuildAtlasMesh(
                    wallMeshBuckets,
                    textureLibrary.wallTextures,
                    outputDirectory / ("building_wall_atlas" + atlasExtension),
                    "building_wall_atlas",
                    BuildingMeshKind::Wall,
                    wallAtlasMesh,
                    result.error))
            {
                return result;
            }
            if (!wallAtlasMesh.vertices.empty() && !wallAtlasMesh.indices.empty())
            {
                meshDatas.push_back(std::move(wallAtlasMesh));
            }
        }
        else
        {
            meshDatas.reserve(meshDatas.size() + wallMeshBuckets.size());
            for (auto& [texturePath, bucket] : wallMeshBuckets)
            {
                if (!bucket.mesh.vertices.empty() && !bucket.mesh.indices.empty())
                {
                    meshDatas.push_back(std::move(bucket.mesh));
                }
            }
        }
        if (useRoofAtlas)
        {
            BuildingMeshData roofAtlasMesh;
            if (!BuildAtlasMesh(
                    roofMeshBuckets,
                    textureLibrary.roofTextures,
                    outputDirectory / ("building_roof_atlas" + atlasExtension),
                    "building_roof_atlas",
                    BuildingMeshKind::Roof,
                    roofAtlasMesh,
                    result.error))
            {
                return result;
            }
            if (!roofAtlasMesh.vertices.empty() && !roofAtlasMesh.indices.empty())
            {
                meshDatas.push_back(std::move(roofAtlasMesh));
            }
        }
        else
        {
            meshDatas.reserve(meshDatas.size() + roofMeshBuckets.size());
            for (auto& [texturePath, bucket] : roofMeshBuckets)
            {
                if (!bucket.mesh.vertices.empty() && !bucket.mesh.indices.empty())
                {
                    meshDatas.push_back(std::move(bucket.mesh));
                }
            }
        }
    }
    else
    {
        meshDatas.reserve(wallMeshBuckets.size() + roofMeshBuckets.size());
        const auto appendMeshData = [&meshDatas](MeshBucket& bucket)
        {
            if (!bucket.mesh.vertices.empty() && !bucket.mesh.indices.empty())
            {
                meshDatas.push_back(std::move(bucket.mesh));
            }
        };

        // 墙体优先输出，低角度检查和渐进加载时先看到建筑立面。
        for (auto& [texturePath, bucket] : wallMeshBuckets)
        {
            appendMeshData(bucket);
        }
        for (auto& [texturePath, bucket] : roofMeshBuckets)
        {
            appendMeshData(bucket);
        }
    }

    if (meshDatas.empty())
    {
        result.success = true;
        return result;
    }
    if (!ExportAssimpFbx(meshDatas, options.outputPath, result.error))
    {
        return result;
    }

    result.meshCount = static_cast<uint32_t>(meshDatas.size());
    result.materialCount = result.meshCount;
    result.success = true;
    return result;
}
