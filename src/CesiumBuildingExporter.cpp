#include "CesiumExport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "GeographicLib/LocalCartesian.hpp"
#include "assimp/scene.h"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

#include "CesiumExportSupport.h"
#include "CesiumGlbWriter.h"
#include "CesiumTextureEncoder.h"
#include "FileDigest.h"
#include "GCJ02GoogleDownBase.h"

namespace
{
using namespace DownTool;
using Json = nlohmann::json;

struct BuildingTile
{
    uint32_t level = 0;
    int64_t x = 0;
    int64_t y = 0;
    auto operator<=>(const BuildingTile&) const = default;
};

struct BuildingTileSource
{
    AmapVectorDecodedData buildings;
    std::vector<size_t> sourceIndices;
};

cv::Mat readImage(const std::filesystem::path& path, int flags = cv::IMREAD_COLOR)
{
    std::ifstream stream(path, std::ios::binary);
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return cv::imdecode(bytes, flags);
}

void mergeBakedMeshes(aiScene& scene)
{
    uint64_t vertices = 0;
    uint64_t faces = 0;
    for (uint32_t index = 0; index < scene.mNumMeshes; ++index)
    {
        vertices += scene.mMeshes[index]->mNumVertices;
        faces += scene.mMeshes[index]->mNumFaces;
    }
    if (vertices > std::numeric_limits<uint32_t>::max() ||
        faces > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("粗层建筑网格超过 GLB 索引容量。");
    }
    auto merged = std::make_unique<aiMesh>();
    merged->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    merged->mNumVertices = uint32_t(vertices);
    merged->mNumFaces = uint32_t(faces);
    merged->mVertices = std::make_unique<aiVector3D[]>(vertices).release();
    merged->mNormals = std::make_unique<aiVector3D[]>(vertices).release();
    merged->mTextureCoords[0] = std::make_unique<aiVector3D[]>(vertices).release();
    merged->mNumUVComponents[0] = 2;
    merged->mFaces = std::make_unique<aiFace[]>(faces).release();
    uint32_t vertexOffset = 0;
    uint32_t faceOffset = 0;
    for (uint32_t index = 0; index < scene.mNumMeshes; ++index)
    {
        const auto& mesh = *scene.mMeshes[index];
        std::copy_n(mesh.mVertices, mesh.mNumVertices, merged->mVertices + vertexOffset);
        std::copy_n(mesh.mNormals, mesh.mNumVertices, merged->mNormals + vertexOffset);
        std::copy_n(mesh.mTextureCoords[0], mesh.mNumVertices,
            merged->mTextureCoords[0] + vertexOffset);
        for (uint32_t face = 0; face < mesh.mNumFaces; ++face)
        {
            auto& destination = merged->mFaces[faceOffset++];
            destination.mNumIndices = 3;
            destination.mIndices = std::make_unique<uint32_t[]>(3).release();
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                destination.mIndices[corner] = mesh.mFaces[face].mIndices[corner] + vertexOffset;
            }
        }
        vertexOffset += mesh.mNumVertices;
    }
    std::unique_ptr<aiMesh*[]> previousMeshes(std::exchange(scene.mMeshes, nullptr));
    std::vector<std::unique_ptr<aiMesh>> previousOwners;
    for (uint32_t index = 0; index < scene.mNumMeshes; ++index)
    {
        previousOwners.emplace_back(previousMeshes[index]);
    }
    auto meshes = std::make_unique<aiMesh*[]>(1);
    meshes[0] = merged.release();
    scene.mMeshes = meshes.release();
    scene.mNumMeshes = 1;
    std::unique_ptr<aiNode> previousRoot(std::exchange(scene.mRootNode, nullptr));
    auto root = std::make_unique<aiNode>("BakedBuildings");
    root->mNumMeshes = 1;
    root->mMeshes = std::make_unique<uint32_t[]>(1).release();
    root->mMeshes[0] = 0;
    scene.mRootNode = root.release();
}

void shareScatterTextures(aiScene& scene, const std::filesystem::path& directory,
    const std::filesystem::path& output, const cesium::TextureEncoder& encoder,
    std::set<std::filesystem::path>& encoded)
{
    std::filesystem::create_directories(output / "textures");
    std::set<std::filesystem::path> temporary;
    for (uint32_t index = 0; index < scene.mNumMaterials; ++index)
    {
        auto& material = *scene.mMaterials[index];
        aiString texture;
        if (material.GetTexture(aiTextureType_DIFFUSE, 0, &texture) != aiReturn_SUCCESS)
        {
            continue;
        }
        const auto source = directory / std::filesystem::u8path(texture.C_Str());
        const auto digest = CalculateFileMd5(source);
        if (!digest)
        {
            throw std::runtime_error("无法校验建筑散图。");
        }
        constexpr char hex[] = "0123456789abcdef";
        std::string name;
        for (const auto value : *digest)
        {
            name.push_back(hex[value >> 4]);
            name.push_back(hex[value & 15]);
        }
        const auto target = output / "textures" / (name + ".ktx2");
        if (!encoded.contains(target))
        {
            std::string error;
            if (!encoder.write(readImage(source, cv::IMREAD_UNCHANGED), target, error))
            {
                throw std::runtime_error(error);
            }
            encoded.insert(target);
        }
        const aiString relative(cesium::pathUtf8(std::filesystem::relative(target, directory)));
        material.AddProperty(&relative, AI_MATKEY_TEXTURE_DIFFUSE(0));
        temporary.insert(source);
    }
    for (const auto& path : temporary)
    {
        std::filesystem::remove(path);
    }
}

// 将重复寻址展开成基础色，再重写 UV；不能直接缩放原散图的 UV 到 atlas。
bool bakeMaterials(aiScene& scene, const std::filesystem::path& directory, uint32_t size,
    const cesium::TextureEncoder& encoder, std::string& error)
{
    const uint32_t count = scene.mNumMeshes;
    const uint32_t columns = uint32_t(std::ceil(std::sqrt(double(count))));
    const uint32_t cell = size / std::max(1u, columns);
    constexpr uint32_t padding = 4;
    if (cell <= padding * 2 + 2)
    {
        error = "粗层材质数量超过图集容量，请增加图集尺寸或减小建筑分块。";
        return false;
    }
    cv::Mat atlas(int(size), int(size), CV_8UC3, cv::Scalar(180, 180, 180));
    for (uint32_t meshIndex = 0; meshIndex < count; ++meshIndex)
    {
        aiMesh& mesh = *scene.mMeshes[meshIndex];
        aiString texture;
        scene.mMaterials[mesh.mMaterialIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &texture);
        const auto image = readImage(directory / std::filesystem::u8path(texture.C_Str()));
        if (image.empty() || !mesh.HasTextureCoords(0))
        {
            error = "建筑粗层烘焙缺少有效贴图或 UV。";
            return false;
        }
        aiTextureMapMode modeU = aiTextureMapMode_Clamp;
        scene.mMaterials[mesh.mMaterialIndex]->Get(
            AI_MATKEY_MAPPINGMODE_U_DIFFUSE(0), modeU);
        float minU = std::numeric_limits<float>::max();
        float minV = minU;
        float maxU = std::numeric_limits<float>::lowest();
        float maxV = maxU;
        for (uint32_t vertex = 0; vertex < mesh.mNumVertices; ++vertex)
        {
            const auto& uv = mesh.mTextureCoords[0][vertex];
            minU = std::min(minU, uv.x);
            maxU = std::max(maxU, uv.x);
            minV = std::min(minV, uv.y);
            maxV = std::max(maxV, uv.y);
        }
        const float width = std::max(1.e-6f, maxU - minU);
        const float height = std::max(1.e-6f, maxV - minV);
        const int inner = int(cell - padding * 2);
        cv::Mat mapX(inner, inner, CV_32F);
        cv::Mat mapY(inner, inner, CV_32F);
        for (int y = 0; y < inner; ++y)
        {
            for (int x = 0; x < inner; ++x)
            {
                float u = minU + width * float(x) / float(inner - 1);
                const float v = minV + height * (1.f - float(y) / float(inner - 1));
                u = modeU == aiTextureMapMode_Wrap ? u - std::floor(u) :
                    std::clamp(u, 0.f, 1.f);
                mapX.at<float>(y, x) = u * (image.cols - 1);
                mapY.at<float>(y, x) = (1.f - std::clamp(v, 0.f, 1.f)) * (image.rows - 1);
            }
        }
        cv::Mat baked;
        cv::remap(image, baked, mapX, mapY, cv::INTER_LINEAR, cv::BORDER_REPLICATE);
        cv::Mat padded;
        cv::copyMakeBorder(baked, padded, int(padding), int(padding),
            int(padding), int(padding), cv::BORDER_REPLICATE);
        const uint32_t originX = (meshIndex % columns) * cell;
        const uint32_t originY = (meshIndex / columns) * cell;
        padded.copyTo(atlas(cv::Rect(int(originX), int(originY), int(cell), int(cell))));
        for (uint32_t vertex = 0; vertex < mesh.mNumVertices; ++vertex)
        {
            auto& uv = mesh.mTextureCoords[0][vertex];
            uv.x = (originX + padding + 0.5f +
                (uv.x - minU) / width * (inner - 1)) / size;
            uv.y = 1.f - (originY + padding + 0.5f +
                (1.f - (uv.y - minV) / height) * (inner - 1)) / size;
        }
        mesh.mMaterialIndex = 0;
    }
    if (!encoder.write(atlas, directory / "atlas.ktx2", error))
    {
        return false;
    }
    auto material = std::make_unique<aiMaterial>();
    const aiString texture("atlas.ktx2");
    material->AddProperty(&texture, AI_MATKEY_TEXTURE_DIFFUSE(0));
    const aiColor4D color(1.f, 1.f, 1.f, 1.f);
    material->AddProperty(&color, 1, AI_MATKEY_BASE_COLOR);
    const float metallic = 0.f;
    const float roughness = 1.f;
    material->AddProperty(&metallic, 1, AI_MATKEY_METALLIC_FACTOR);
    material->AddProperty(&roughness, 1, AI_MATKEY_ROUGHNESS_FACTOR);
    std::unique_ptr<aiMaterial*[]> previousMaterials(std::exchange(scene.mMaterials, nullptr));
    std::vector<std::unique_ptr<aiMaterial>> previousOwners;
    for (uint32_t index = 0; index < scene.mNumMaterials; ++index)
    {
        previousOwners.emplace_back(previousMaterials[index]);
    }
    scene.mNumMaterials = 1;
    auto materials = std::make_unique<aiMaterial*[]>(1);
    materials[0] = material.release();
    scene.mMaterials = materials.release();
    mergeBakedMeshes(scene);
    return true;
}

double mercatorX(double longitude)
{
    return longitude * std::numbers::pi / 180. * 6378137.;
}

double mercatorY(double latitude)
{
    return std::log(std::tan(std::numbers::pi / 4. +
        std::clamp(latitude, -85.0511287798, 85.0511287798) *
            std::numbers::pi / 360.)) * 6378137.;
}

double signedArea(const std::vector<cv::Point2d>& ring)
{
    double area = 0.;
    for (size_t index = 0; index < ring.size(); ++index)
    {
        const auto& a = ring[index];
        const auto& b = ring[(index + 1) % ring.size()];
        area += a.x * b.y - a.y * b.x;
    }
    return area * 0.5;
}

bool segmentsCross(const cv::Point2d& a, const cv::Point2d& b,
    const cv::Point2d& c, const cv::Point2d& d)
{
    const auto cross = [](const auto& first, const auto& second, const auto& third)
    {
        return (second.x - first.x) * (third.y - first.y) -
            (second.y - first.y) * (third.x - first.x);
    };
    return cross(a, b, c) * cross(a, b, d) < 0. &&
        cross(c, d, a) * cross(c, d, b) < 0.;
}

AmapVectorDecodedData simplified(
    const AmapVectorDecodedData& source, double tolerance)
{
    auto result = source;
    for (auto& feature : result.features)
    {
        // 多环建筑保留原轮廓，避免粗层把孔洞或独立楼体错误连接。
        if (feature.geometryParts.size() != 1 || tolerance <= 0.)
        {
            continue;
        }
        auto& part = feature.geometryParts.front();
        std::vector<cv::Point2d> ring;
        for (const auto& point : part)
        {
            ring.emplace_back(point.longitude * 111320., point.latitude * 111320.);
        }
        if (ring.size() > 3 && ring.front() == ring.back())
        {
            ring.pop_back();
        }
        if (ring.size() <= 4)
        {
            continue;
        }
        std::vector<cv::Point2f> input;
        const auto origin = ring.front();
        for (const auto& point : ring)
        {
            input.emplace_back(float(point.x - origin.x), float(point.y - origin.y));
        }
        std::vector<cv::Point2f> approximation;
        cv::approxPolyDP(input, approximation, tolerance, true);
        std::vector<cv::Point2d> candidate;
        for (const auto& point : approximation)
        {
            candidate.emplace_back(point.x + origin.x, point.y + origin.y);
        }
        if (candidate.size() < 4 || candidate.size() >= ring.size() ||
            signedArea(candidate) * signedArea(ring) <= 0.)
        {
            continue;
        }
        bool intersects = false;
        for (size_t first = 0; first < candidate.size(); ++first)
        {
            for (size_t second = first + 2; second < candidate.size(); ++second)
            {
                if (first == 0 && second + 1 == candidate.size())
                {
                    continue;
                }
                intersects |= segmentsCross(candidate[first],
                    candidate[(first + 1) % candidate.size()], candidate[second],
                    candidate[(second + 1) % candidate.size()]);
            }
        }
        if (!intersects)
        {
            part.clear();
            for (const auto& point : candidate)
            {
                part.push_back({point.x / 111320., point.y / 111320.});
            }
        }
    }
    return result;
}

std::array<double, 16> enuTransform(const LonLat& center)
{
    const double lon = center.lon * std::numbers::pi / 180.;
    const double lat = center.lat * std::numbers::pi / 180.;
    const auto position = cesium::ecef(center.lon, center.lat, 0.);
    return {-std::sin(lon), std::cos(lon), 0., 0.,
        -std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat), 0.,
        std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat), 0.,
        position[0], position[1], position[2], 1.};
}
} // namespace

bool DownTool::ExportCesiumBuildings(
    const AmapVectorDecodedData& buildings, const CesiumBuildingOptions& options,
    std::string& error)
{
    try
    {
        if (!cesium::validateOptions(options, error))
        {
            return false;
        }
        const auto& base = options.GetBuildingOptions();
        if (options.GetLodLevels() < 2 || options.GetLodLevels() > 8 ||
            options.GetAtlasSize() < 256 || options.GetAtlasSize() > 4096 ||
            !std::isfinite(base.GetChunkSizeMeters()) || base.GetChunkSizeMeters() <= 0.)
        {
            error = "建筑 LOD 层数、图集尺寸或分块边长无效。";
            return false;
        }
        const cesium::HeightConversion conversion(options);
        const double originX = mercatorX(base.centerWgs84.lon);
        const double originY = mercatorY(base.centerWgs84.lat);
        const uint32_t leaf = options.GetLodLevels() - 1;
        const double rootSize = base.GetChunkSizeMeters() * std::pow(2., leaf);
        std::map<BuildingTile, BuildingTileSource> nodes;
        AmapVectorDecodedData selectedBuildings;
        GeographicLib::LocalCartesian grouping(
            base.centerWgs84.lat, base.centerWgs84.lon, 0.);
        for (const auto& feature : buildings.features)
        {
            if (!feature.hasHeight || feature.height == 0 ||
                feature.geometryType != AmapVectorGeometryType::Polygon ||
                feature.geometryParts.empty() || feature.geometryParts.front().empty())
            {
                continue;
            }
            double x = 0.;
            double y = 0.;
            GeographicBounds footprint{180., -180., 90., -90.};
            for (const auto& point : feature.geometryParts.front())
            {
                const auto wgs = GCJ02GoogleDownBase::GCJ02ToWGS84(
                    LonLat(point.longitude, point.latitude));
                footprint.west = std::min(footprint.west, wgs.lon);
                footprint.east = std::max(footprint.east, wgs.lon);
                footprint.south = std::min(footprint.south, wgs.lat);
                footprint.north = std::max(footprint.north, wgs.lat);
                double east = 0.;
                double north = 0.;
                double up = 0.;
                grouping.Forward(wgs.lat, wgs.lon, 0., east, north, up);
                x += east;
                y += north;
            }
            const auto& bounds = options.GetBounds();
            if (footprint.east < bounds.west || footprint.west > bounds.east ||
                footprint.north < bounds.south || footprint.south > bounds.north ||
                (!options.GetPolygons().empty() &&
                 !GeographicPolygonsIntersectBounds(options.GetPolygons(), footprint)))
            {
                continue;
            }
            x /= feature.geometryParts.front().size();
            y /= feature.geometryParts.front().size();
            const size_t sourceIndex = selectedBuildings.features.size();
            selectedBuildings.features.push_back(feature);
            for (uint32_t level = 0; level <= leaf; ++level)
            {
                const double side = rootSize / std::pow(2., level);
                const BuildingTile tile{level, int64_t(std::floor(x / side)),
                    int64_t(std::floor(y / side))};
                auto& source = nodes[tile];
                source.buildings.features.push_back(feature);
                source.sourceIndices.push_back(sourceIndex);
            }
        }
        if (nodes.empty())
        {
            error = "所选范围没有可导出的带高度建筑。";
            return false;
        }
        auto selectionOptions = base;
        selectionOptions.wallTextureMode = AmapBuildingFbxTextureMode::Scattered;
        selectionOptions.roofTextureMode = AmapBuildingFbxRoofTextureMode::Scattered;
        selectionOptions.cancellationRequested = options.GetCancellation();
        AmapBuildingTextureSelections selections;
        // 在原轮廓上按原 FBX 规则只选一次，源索引在分块和各级简化中保持不变。
        if (!PrepareAmapBuildingTextureSelections(
            selectedBuildings, selectionOptions, selections, error))
        {
            return false;
        }
        std::map<BuildingTile, Json> tileJson;
        const cesium::TextureEncoder encoder(
            options.GetTextureMipmaps(), options.GetCancellation());
        std::set<std::filesystem::path> encodedTextures;
        Json statistics = Json::array();
        uint64_t completed = 0;
        for (auto iterator = nodes.rbegin(); iterator != nodes.rend(); ++iterator)
        {
            if (cesium::canceled(options))
            {
                error = "建筑 3D Tiles 导出已取消。";
                return false;
            }
            const auto& tile = iterator->first;
            const auto& source = iterator->second;
            const bool finest = tile.level == leaf;
            const double tolerance = finest ? 0. :
                options.GetErrorMeters() * std::pow(2., leaf - tile.level - 1);
            const auto data = simplified(source.buildings, tolerance);
            const auto directory = options.GetOutputPath() / "tiles" /
                std::to_string(tile.level) /
                (std::to_string(tile.x) + "_" + std::to_string(tile.y));
            std::filesystem::create_directories(directory);
            auto settings = base;
            settings.outputPath = directory / "model.glb";
            settings.shape = FbxPlanetShape::Plane;
            settings.SetChunkSizeMeters(rootSize * 1000000.);
            settings.wallTextureMode = AmapBuildingFbxTextureMode::Scattered;
            settings.roofTextureMode = AmapBuildingFbxRoofTextureMode::Scattered;
            AmapBuildingTextureSelections tileSelections;
            tileSelections.reserve(source.sourceIndices.size());
            for (const size_t index : source.sourceIndices)
            {
                tileSelections.push_back(selections[index]);
            }
            settings.SetTextureSelections(tileSelections);
            settings.terrainHeightEnabled = true;
            settings.elevationReferenceMeters = 0.;
            settings.elevationSampler = [&](const LonLat& point) -> std::optional<double>
            {
                const auto sample = options.GetSampler() ?
                    options.GetSampler()(point) : std::optional<double>(0.);
                return sample ? std::optional<double>(conversion.convert(point, *sample)) :
                    std::nullopt;
            };
            settings.cancellationRequested = options.GetCancellation();
            GeographicLib::LocalCartesian local(
                base.centerWgs84.lat, base.centerWgs84.lon, 0.);
            std::array<double, 3> minimum = {1.e100, 1.e100, 1.e100};
            std::array<double, 3> maximum = {-1.e100, -1.e100, -1.e100};
            uint64_t triangleCount = 0;
            uint64_t vertexCount = 0;
            uint32_t primitiveCount = 0;
            const uint32_t atlasSize = finest ? 0 : std::max(256u,
                options.GetAtlasSize() >> (leaf - tile.level - 1));
            const auto result = ExportAmapBuildingScene(data, settings,
                [&](aiScene& scene, std::string& sceneError)
                {
                    if (!finest &&
                        !bakeMaterials(scene, directory, atlasSize, encoder, sceneError))
                    {
                        return false;
                    }
                    if (finest)
                    {
                        shareScatterTextures(scene, directory,
                            options.GetOutputPath(), encoder, encodedTextures);
                    }
                    for (uint32_t meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex)
                    {
                        auto& mesh = *scene.mMeshes[meshIndex];
                        triangleCount += mesh.mNumFaces;
                        vertexCount += mesh.mNumVertices;
                        for (uint32_t index = 0; index < mesh.mNumVertices; ++index)
                        {
                            auto& point = mesh.mVertices[index];
                            const double longitude = (originX + point.x * 0.01) /
                                6378137. * 180. / std::numbers::pi;
                            const double latitude = (2. * std::atan(std::exp(
                                (originY + point.y * 0.01) / 6378137.)) -
                                    std::numbers::pi / 2.) * 180. / std::numbers::pi;
                            double east = 0.;
                            double north = 0.;
                            double up = 0.;
                            local.Forward(latitude, longitude, point.z * 0.01, east, north, up);
                            const std::array<double, 3> position{east, north, up};
                            for (size_t axis = 0; axis < 3; ++axis)
                            {
                                minimum[axis] = std::min(minimum[axis], position[axis]);
                                maximum[axis] = std::max(maximum[axis], position[axis]);
                            }
                            // glTF Y-up；Cesium 的 glTF -> Z-up 转换后回到 ENU。
                            point = aiVector3D(float(east), float(up), float(-north));
                            const auto normal = mesh.mNormals[index];
                            mesh.mNormals[index] = aiVector3D(normal.x, normal.z, -normal.y);
                        }
                    }
                    primitiveCount = finest ? scene.mNumMeshes : 1;
                    return cesium::writeGlb(scene, settings.outputPath,
                        sceneError, options.GetTextureMipmaps());
                });
            if (!result.success)
            {
                error = result.error;
                return false;
            }
            if (result.meshCount == 0)
            {
                if (options.GetProgress())
                {
                    options.GetProgress()(++completed, nodes.size());
                }
                continue;
            }
            Json children = Json::array();
            for (int64_t dy = 0; dy < 2; ++dy)
            {
                for (int64_t dx = 0; dx < 2; ++dx)
                {
                    const BuildingTile child{tile.level + 1, tile.x * 2 + dx, tile.y * 2 + dy};
                    const auto found = tileJson.find(child);
                    if (found == tileJson.end())
                    {
                        continue;
                    }
                    const auto box = found->second["boundingVolume"]["box"];
                    for (size_t axis = 0; axis < 3; ++axis)
                    {
                        const double center = box[axis];
                        const double extent = box[3 + axis * 4];
                        minimum[axis] = std::min(minimum[axis], center - extent);
                        maximum[axis] = std::max(maximum[axis], center + extent);
                    }
                    children.push_back(found->second);
                }
            }
            std::array<double, 12> box{};
            for (size_t axis = 0; axis < 3; ++axis)
            {
                box[axis] = (minimum[axis] + maximum[axis]) * 0.5;
                box[3 + axis * 4] = (maximum[axis] - minimum[axis]) * 0.5 + 0.1;
            }
            const double side = rootSize / std::pow(2., tile.level);
            Json entry = {{"boundingVolume", {{"box", box}}},
                {"geometricError", finest ? 0. : std::max(tolerance, side / 128.)},
                {"refine", "REPLACE"},
                {"content", {{"uri", cesium::pathUtf8(
                    std::filesystem::relative(settings.outputPath, options.GetOutputPath()))}}}};
            if (!children.empty())
            {
                entry["children"] = std::move(children);
            }
            tileJson.emplace(tile, std::move(entry));
            statistics.push_back({{"level", tile.level}, {"x", tile.x}, {"y", tile.y},
                {"buildings", result.buildingCount}, {"triangles", triangleCount},
                {"inputRenderVertices", vertexCount}, {"primitiveCountEstimate", primitiveCount},
                {"atlasSize", finest ? 0 : atlasSize}, {"footprintToleranceMeters", tolerance}});
            if (!finest)
            {
                // 粗层 GLB 只引用 atlas，删去本层烘焙产生的临时散图。
                for (const auto& entry : std::filesystem::directory_iterator(directory))
                {
                    const auto name = cesium::pathUtf8(entry.path().filename());
                    if (name.starts_with("building_wall_") || name.starts_with("building_roof_"))
                    {
                        std::filesystem::remove(entry.path());
                    }
                }
            }
            if (options.GetProgress())
            {
                options.GetProgress()(++completed, nodes.size());
            }
        }
        Json roots = Json::array();
        std::array<double, 3> minimum{1.e100, 1.e100, 1.e100};
        std::array<double, 3> maximum{-1.e100, -1.e100, -1.e100};
        for (const auto& [tile, entry] : tileJson)
        {
            if (tile.level != 0)
            {
                continue;
            }
            roots.push_back(entry);
            const auto& box = entry["boundingVolume"]["box"];
            for (size_t axis = 0; axis < 3; ++axis)
            {
                const double center = box[axis];
                const double extent = box[3 + axis * 4];
                minimum[axis] = std::min(minimum[axis], center - extent);
                maximum[axis] = std::max(maximum[axis], center + extent);
            }
        }
        std::array<double, 12> box{};
        if (roots.empty())
        {
            error = "所选范围没有有效的建筑多边形网格。";
            return false;
        }
        for (size_t axis = 0; axis < 3; ++axis)
        {
            box[axis] = (minimum[axis] + maximum[axis]) * 0.5;
            box[3 + axis * 4] = (maximum[axis] - minimum[axis]) * 0.5 + 0.1;
        }
        double rootError = rootSize;
        for (const auto& root : roots)
        {
            rootError = std::max(rootError, root["geometricError"].get<double>() * 2.);
        }
        const Json tileset = {{"asset", {{"version", "1.1"}}},
            {"geometricError", rootError},
            {"root", {{"boundingVolume", {{"box", box}}}, {"transform", enuTransform(base.centerWgs84)},
                {"geometricError", rootError}, {"refine", "REPLACE"}, {"children", roots}}}};
        return cesium::writeJson(options.GetOutputPath() / "tileset.json", tileset, error) &&
            cesium::writeJson(options.GetOutputPath() / "export-report.json",
                {{"heightConversion", conversion.description()},
                 {"textureEncoding", {{"container", "KTX2"}, {"codec", "ETC1S"},
                     {"quality", 128}, {"mipmaps", options.GetTextureMipmaps()},
                     {"minFilter", options.GetTextureMipmaps() ? 9987 : 9729},
                     {"magFilter", 9729}, {"maximumSideLimit", nullptr}}},
                 {"selectionErrorPolicy", "max(footprint tolerance, nominal tile side / 128)"},
                 {"tiles", statistics}}, error);
    }
    catch (const std::exception& exception)
    {
        error = "建筑 3D Tiles 导出失败：" + std::string(exception.what());
        return false;
    }
}
