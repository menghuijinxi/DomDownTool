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
    };

    struct BuildingMeshData
    {
        std::string name;
        std::filesystem::path texturePath;
        std::vector<BuildingVertex> vertices;
        std::vector<uint32_t> indices;
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

    struct TextureLibrary
    {
        std::vector<TextureInfo> wallTextures;
        std::vector<TextureInfo> roofTextures;
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
        return LoadTextureList(
            options.wallTexturePath,
            outputDirectory,
            "building_wall",
            library.wallTextures,
            error) &&
            LoadTextureList(
                options.roofTexturePath,
                outputDirectory,
                "building_roof",
                library.roofTextures,
                error);
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

    MeshBucket& GetMeshBucket(
        std::map<std::filesystem::path, MeshBucket>& buckets,
        const TextureInfo& texture)
    {
        MeshBucket& bucket = buckets[texture.outputPath];
        if (bucket.mesh.name.empty())
        {
            bucket.mesh.name = texture.materialName;
            bucket.mesh.texturePath = texture.outputPath;
        }
        return bucket;
    }

    bool IsWallMeshTexturePath(const std::filesystem::path& texturePath)
    {
        return PathToUtf8String(texturePath.filename()).starts_with("building_wall_");
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

            uint32_t bottomFirst = 0;
            uint32_t bottomSecond = 0;
            uint32_t topFirst = 0;
            uint32_t topSecond = 0;
            const double bottomV =
                std::max(1., (topCentimeters - bottomCentimeters) /
                                std::max(1., heightCentimeters));
            if (!AppendVertex(
                    mesh,
                    { first.x, first.y, bottomCentimeters, currentU, bottomV, normal },
                    bottomFirst) ||
                !AppendVertex(
                    mesh,
                    { second.x, second.y, bottomCentimeters, nextU, bottomV, normal },
                    bottomSecond) ||
                !AppendVertex(
                    mesh,
                    { first.x, first.y, topCentimeters, currentU, 0., normal },
                    topFirst) ||
                !AppendVertex(
                    mesh,
                    { second.x, second.y, topCentimeters, nextU, 0., normal },
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
        BuildingMeshData& mesh)
    {
        const double footprintWidth = std::max(1., geometry.bounds.maxX - geometry.bounds.minX);
        const double footprintHeight = std::max(1., geometry.bounds.maxY - geometry.bounds.minY);
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
                        (point.x - geometry.bounds.minX) / footprintWidth,
                        1. - (point.y - geometry.bounds.minY) / footprintHeight,
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

    bool CopyTexture(
        const TextureInfo& texture,
        std::set<std::filesystem::path>& copiedTextures,
        std::string& error)
    {
        if (copiedTextures.contains(texture.outputPath))
        {
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

        const bool isWallTexture = IsWallMeshTexturePath(meshData.texturePath);
        const aiTextureMapMode mapModeU = isWallTexture ?
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

    std::map<std::filesystem::path, MeshBucket> meshBuckets;
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
        const size_t roofTextureIndex = SelectRoofTexture(
            textureLibrary.roofTextures,
            geometry,
            randomEngine);
        const TextureInfo& wallTexture = textureLibrary.wallTextures[wallTextureIndex];
        const TextureInfo& roofTexture = textureLibrary.roofTextures[roofTextureIndex];

        if (!CopyTexture(wallTexture, copiedTextures, result.error) ||
            !CopyTexture(roofTexture, copiedTextures, result.error))
        {
            return result;
        }

        AppendWallGeometry(
            geometry,
            heightCentimeters,
            bottomCentimeters,
            topCentimeters,
            wallTexture,
            GetMeshBucket(meshBuckets, wallTexture).mesh);
        AppendRoofGeometry(
            geometry,
            topCentimeters,
            GetMeshBucket(meshBuckets, roofTexture).mesh);
        result.buildingCount++;
    }

    std::vector<BuildingMeshData> meshDatas;
    meshDatas.reserve(meshBuckets.size());
    const auto appendMeshData = [&meshDatas](MeshBucket& bucket)
    {
        if (!bucket.mesh.vertices.empty() && !bucket.mesh.indices.empty())
        {
            meshDatas.push_back(std::move(bucket.mesh));
        }
    };

    // 墙体优先输出，低角度检查和渐进加载时先看到建筑立面。
    for (auto& [texturePath, bucket] : meshBuckets)
    {
        if (IsWallMeshTexturePath(texturePath))
        {
            appendMeshData(bucket);
        }
    }
    for (auto& [texturePath, bucket] : meshBuckets)
    {
        if (!IsWallMeshTexturePath(texturePath))
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
