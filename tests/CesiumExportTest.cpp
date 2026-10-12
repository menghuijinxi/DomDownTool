#include "CesiumExport.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <set>
#include <stdexcept>
#include <vector>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "ktx.h"
#include "nlohmann/json.hpp"
#include "opencv2/imgcodecs.hpp"
#include "zlib.h"

#include "AmapVectorDownloader.h"
#include "AmapVectorPngDownloader.h"
#include "CopernicusDem.h"
#include "CesiumGlbWriter.h"
#include "CesiumExportSupport.h"
#include "CesiumTextureEncoder.h"
#include "FileDigest.h"
#include "GCJ02GoogleDownBase.h"
#include "GaoDeDown.h"
#include "PostProcessTestSupport.h"

namespace
{
using namespace DownTool;
using Json = nlohmann::json;

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

Json readJson(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    return Json::parse(stream);
}

Json readGlbJson(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    stream.seekg(12);
    uint32_t length = 0;
    stream.read(reinterpret_cast<char*>(&length), sizeof(length));
    stream.seekg(20);
    std::string content(length, '\0');
    stream.read(content.data(), length);
    require(stream.good(), "cannot read GLB JSON");
    return Json::parse(content);
}

struct TextureDeleter
{
    void operator()(ktxTexture2* texture) const
    {
        ktxTexture_Destroy(ktxTexture(texture));
    }
};
using Texture = std::unique_ptr<ktxTexture2, TextureDeleter>;

Texture readTexture(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    const std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    ktxTexture2* texture = nullptr;
    require(ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(),
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &texture) == KTX_SUCCESS,
        "cannot read KTX2 " + path.string());
    return Texture(texture);
}

void validateTexture(const std::filesystem::path& path, bool mipmaps)
{
    const auto texture = readTexture(path);
    require(ktxTexture2_GetColorModel_e(texture.get()) == KHR_DF_MODEL_ETC1S &&
        texture->supercompressionScheme == KTX_SS_BASIS_LZ,
        "texture is not ETC1S/BasisLZ");
    require(ktxTexture2_GetOETF_e(texture.get()) == KHR_DF_TRANSFER_SRGB,
        "base color is not sRGB");
    const uint32_t levels = mipmaps ? uint32_t(std::floor(std::log2(
        std::max(texture->baseWidth, texture->baseHeight)))) + 1 : 1;
    require(texture->numLevels == levels, "incorrect encoded mipmap count");
    require(ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0) == KTX_SUCCESS,
        "ETC1S texture cannot be transcoded");
    for (uint32_t level = 0; level < levels; ++level)
    {
        ktx_size_t offset = 0;
        require(ktxTexture_GetImageOffset(ktxTexture(texture.get()), level, 0, 0, &offset) ==
            KTX_SUCCESS, "mipmap pixels missing after transcode");
    }
}

void validateTextureEncoder(const std::filesystem::path& root)
{
    const cv::Mat image(256, 768, CV_8UC4, cv::Scalar(32, 80, 220, 128));
    std::string error;
    for (const bool mipmaps : {false, true})
    {
        const auto path = root / (mipmaps ? "rgba-mips.ktx2" : "rgba.ktx2");
        const cesium::TextureEncoder encoder(mipmaps);
        require(encoder.write(image, path, error), error);
        validateTexture(path, mipmaps);
        const auto texture = readTexture(path);
        require(texture->baseWidth == 768 && texture->baseHeight == 256,
            "texture was capped at 512 or stretched to a power of two");
        require(ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0) == KTX_SUCCESS,
            "cannot decode color check");
        ktx_size_t offset = 0;
        require(ktxTexture_GetImageOffset(ktxTexture(texture.get()), 0, 0, 0, &offset) ==
            KTX_SUCCESS, "missing base level");
        const auto* pixel = ktxTexture_GetData(ktxTexture(texture.get())) + offset;
        require(std::abs(int(pixel[0]) - 220) < 12 &&
            std::abs(int(pixel[1]) - 80) < 12 &&
            std::abs(int(pixel[2]) - 32) < 12 &&
            std::abs(int(pixel[3]) - 128) < 12,
            "RGB channel order or alpha was changed");
    }
    cv::Mat checker(4, 8, CV_8UC3, cv::Scalar(0, 0, 0));
    checker(cv::Rect(4, 0, 4, 4)).setTo(cv::Scalar(255, 255, 255));
    const cesium::TextureEncoder encoder(true);
    const auto path = root / "linear-mips.ktx2";
    require(encoder.write(checker, path, error), error);
    const auto texture = readTexture(path);
    require(ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0) == KTX_SUCCESS,
        "cannot decode mipmap gamma test");
    ktx_size_t offset = 0;
    require(ktxTexture_GetImageOffset(ktxTexture(texture.get()), 3, 0, 0, &offset) ==
        KTX_SUCCESS, "last mip missing");
    const auto* pixel = ktxTexture_GetData(ktxTexture(texture.get())) + offset;
    require(pixel[0] > 175 && pixel[0] < 200, "mipmap downsampling ignored sRGB transfer");
    require(!encoder.write({}, root / "invalid.ktx2", error),
        "empty texture accepted");
    const cesium::TextureEncoder canceled(false, [] { return true; });
    require(!canceled.write(image, root / "canceled.ktx2", error) &&
        !std::filesystem::exists(root / "canceled.ktx2"), "texture cancellation ignored");
    int cancellationChecks = 0;
    const cesium::TextureEncoder lateCanceled(false,
        [&] { return ++cancellationChecks == 3; });
    require(!lateCanceled.write(image, root / "late-canceled.ktx2", error) &&
        !std::filesystem::exists(root / "late-canceled.ktx2"),
        "texture canceled after compression was still written");
    require(!encoder.write(image, root / "missing" / "image.ktx2", error) &&
        !error.empty(), "texture output failure was ignored");
    const cv::Mat unsupported(8, 8, CV_32FC3, cv::Scalar(0., 0., 0.));
    require(!encoder.write(unsupported, root / "float.ktx2", error),
        "unsupported texture depth accepted");
    const cv::Mat odd(65, 127, CV_8UC3, cv::Scalar(20, 90, 170));
    require(encoder.write(odd, root / "aligned.ktx2", error), error);
    const auto aligned = readTexture(root / "aligned.ktx2");
    require(aligned->baseWidth == 128 && aligned->baseHeight == 68,
        "block alignment changed texture dimensions unexpectedly");
}

class TerrainReader
{
public:
    explicit TerrainReader(const std::filesystem::path& path)
    {
        const auto level = std::stoul(path.parent_path().parent_path().filename().string());
        const auto x = std::stoul(path.parent_path().filename().string());
        const auto y = std::stoul(path.stem().string());
        const double side = 180. / std::pow(2., level);
        bounds_ = {-180. + x * side, -180. + (x + 1.) * side,
            -90. + y * side, -90. + (y + 1.) * side};
#if defined(_WIN32)
        gzFile stream = gzopen_w(path.wstring().c_str(), "rb");
#else
        gzFile stream = gzopen(postprocess_test::PathToUtf8String(path).c_str(), "rb");
#endif
        require(stream != nullptr, "cannot open terrain");
        std::array<unsigned char, 16384> buffer{};
        int count = 0;
        while ((count = gzread(stream, buffer.data(), unsigned(buffer.size()))) > 0)
        {
            bytes_.insert(bytes_.end(), buffer.begin(), buffer.begin() + count);
        }
        gzclose(stream);
        require(count == 0, "invalid gzip terrain");
    }

    template <typename Value>
    Value read()
    {
        require(offset_ + sizeof(Value) <= bytes_.size(), "truncated terrain");
        Value value{};
        std::memcpy(&value, bytes_.data() + offset_, sizeof(Value));
        offset_ += sizeof(Value);
        return value;
    }

    void validate()
    {
        for (int index = 0; index < 3; ++index)
        {
            require(std::isfinite(read<double>()), "invalid center");
        }
        const float minimum = read<float>();
        const float maximum = read<float>();
        require(minimum <= maximum, "invalid height range");
        std::array<double, 3> center{};
        for (auto& value : center)
        {
            value = read<double>();
            require(std::isfinite(value), "invalid sphere");
        }
        const auto radius = read<double>();
        require(std::isfinite(radius) && radius > 0., "invalid sphere radius");
        double horizonMagnitude = 0.;
        for (int index = 0; index < 3; ++index)
        {
            const double value = read<double>();
            require(std::isfinite(value), "invalid horizon");
            horizonMagnitude += value * value;
        }
        require(horizonMagnitude >= 1., "zero horizon point culls the whole terrain");
        const uint32_t vertices = read<uint32_t>();
        require(vertices >= 4 && vertices <= 65536, "invalid vertices");
        std::array<std::vector<int32_t>, 3> decoded;
        for (auto& values : decoded)
        {
            int32_t previous = 0;
            for (uint32_t index = 0; index < vertices; ++index)
            {
                const uint16_t code = read<uint16_t>();
                previous += (code >> 1) ^ -int32_t(code & 1);
                require(previous >= 0 && previous <= 32767, "invalid quantized vertex");
                values.push_back(previous);
            }
        }
        for (uint32_t index = 0; index < vertices; ++index)
        {
            const auto position = cesium::ecef(
                bounds_.west + (bounds_.east - bounds_.west) * decoded[0][index] / 32767.,
                bounds_.south + (bounds_.north - bounds_.south) * decoded[1][index] / 32767.,
                minimum + (double(maximum) - minimum) * decoded[2][index] / 32767.);
            double squared = 0.;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                squared += std::pow(position[axis] - center[axis], 2.);
            }
            require(std::sqrt(squared) <= radius + 1.e-5,
                "decoded vertex outside terrain bounding sphere");
        }
        const auto triangles = read<uint32_t>();
        require(triangles > 0, "empty mesh");
        uint32_t highest = 0;
        std::array<uint32_t, 3> triangle{};
        for (uint32_t index = 0; index < triangles * 3; ++index)
        {
            const auto code = read<uint16_t>();
            require(code <= highest, "invalid high-water-mark");
            const auto vertex = highest - code;
            require(vertex < vertices, "invalid vertex reference");
            if (code == 0)
            {
                ++highest;
            }
            triangle[index % 3] = vertex;
            if (index % 3 == 2)
            {
                const auto a = triangle[0];
                const auto b = triangle[1];
                const auto c = triangle[2];
                const int64_t cross =
                    int64_t(decoded[0][b] - decoded[0][a]) *
                        (decoded[1][c] - decoded[1][a]) -
                    int64_t(decoded[1][b] - decoded[1][a]) *
                        (decoded[0][c] - decoded[0][a]);
                require(cross > 0, "degenerate or inverted triangle");
            }
        }
        require(highest == vertices, "unreferenced vertices");
        for (uint32_t edge = 0; edge < 4; ++edge)
        {
            const auto count = read<uint32_t>();
            require(count >= 2, "missing edge");
            for (uint32_t index = 0; index < count; ++index)
            {
                const auto vertex = read<uint16_t>();
                require(vertex < vertices, "invalid edge index");
                const auto channel = edge % 2;
                const auto expected = edge < 2 ? 0 : 32767;
                require(decoded[channel][vertex] == expected, "edge not on tile boundary");
            }
        }
        require(offset_ == bytes_.size(), "unparsed terrain bytes");
    }

protected:
    GeographicBounds bounds_;
    std::vector<unsigned char> bytes_;
    size_t offset_ = 0;
};

AmapVectorFeature makeBuilding(double x, double y, uint64_t uid)
{
    AmapVectorFeature feature;
    feature.uid = uid;
    feature.layerName = "building";
    feature.geometryType = AmapVectorGeometryType::Polygon;
    feature.hasHeight = true;
    feature.height = 6;
    std::vector<AmapVectorCoordinate> ring;
    for (uint32_t index = 0; index < 24; ++index)
    {
        const double angle = index * std::numbers::pi * 2. / 24.;
        const LonLat point(116.4 + (x + std::cos(angle) * 20.) / 85000.,
            39.9 + (y + std::sin(angle) * 15.) / 111320.);
        const auto gcj = GCJ02GoogleDownBase::WGS84ToGCJ02(point);
        ring.push_back({gcj.lon, gcj.lat});
    }
    feature.geometryParts.push_back(ring);
    return feature;
}

void validateBuildings(const std::filesystem::path& output, bool mipmaps = false)
{
    const auto tileset = readJson(output / "tileset.json");
    require(tileset["asset"]["version"] == "1.1", "wrong tileset version");
    uint64_t models = 0;
    uint64_t coarse = 0;
    std::set<std::filesystem::path> textures;
    const std::function<void(const Json&)> visit = [&](const Json& tile)
    {
        require(tile["refine"] == "REPLACE", "wrong refinement");
        require(tile["boundingVolume"]["box"].size() == 12, "invalid bounding box");
        if (tile.contains("content"))
        {
            const auto path = output /
                std::filesystem::u8path(tile["content"]["uri"].get<std::string>());
            const auto document = readGlbJson(path);
            require(std::find(document["extensionsRequired"].begin(),
                document["extensionsRequired"].end(), "KHR_texture_basisu") !=
                document["extensionsRequired"].end(), "BasisU extension not required");
            for (const auto& texture : document["textures"])
            {
                require(!texture.contains("source") &&
                    texture.contains("extensions") &&
                    texture["extensions"].contains("KHR_texture_basisu"),
                    "KTX2 referenced as an ordinary JPEG/PNG");
                const auto source = texture["extensions"]["KHR_texture_basisu"]["source"]
                    .get<size_t>();
                const auto& image = document["images"][source];
                require(image["mimeType"] == "image/ktx2", "incorrect KTX2 MIME type");
                const auto imagePath = (path.parent_path() /
                    std::filesystem::u8path(image["uri"].get<std::string>())).lexically_normal();
                if (textures.insert(imagePath).second)
                {
                    validateTexture(imagePath, mipmaps);
                }
                const auto& sampler = document["samplers"][texture["sampler"].get<size_t>()];
                require(sampler["magFilter"] == 9729 &&
                    sampler["minFilter"] == (mipmaps ? 9987 : 9729),
                    "sampler does not match mipmap option");
            }
            Assimp::Importer importer;
            const auto* scene = importer.ReadFile(postprocess_test::PathToUtf8String(path), 0);
            require(scene && scene->mNumMeshes > 0, "cannot read GLB " + path.string());
            ++models;
            const bool parent = tile.contains("children");
            if (parent)
            {
                ++coarse;
                require(scene->mNumMeshes == 1, "coarse tile was not merged into one primitive");
                require(scene->mNumMaterials <= 2, "coarse tile retains scatter materials");
                require(std::filesystem::exists(path.parent_path() / "atlas.ktx2"),
                    "coarse atlas missing");
            }
            for (uint32_t material = 0; material < scene->mNumMaterials; ++material)
            {
                aiString texture;
                if (scene->mMaterials[material]->GetTexture(
                        aiTextureType_DIFFUSE, 0, &texture) == aiReturn_SUCCESS)
                {
                    require(std::filesystem::exists(path.parent_path() /
                        std::filesystem::u8path(texture.C_Str())), "GLB texture missing");
                }
            }
            for (uint32_t meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
            {
                const auto& mesh = *scene->mMeshes[meshIndex];
                require(mesh.HasNormals() && mesh.HasTextureCoords(0), "missing normals/UV");
                for (uint32_t index = 0; index < mesh.mNumVertices; ++index)
                {
                    const auto& point = mesh.mVertices[index];
                    require(std::isfinite(point.x) && std::isfinite(point.y) &&
                        std::isfinite(point.z), "nonfinite vertex");
                    if (parent)
                    {
                        const auto& uv = mesh.mTextureCoords[0][index];
                        require(uv.x >= 0. && uv.x <= 1. && uv.y >= 0. && uv.y <= 1.,
                            "coarse UV outside atlas");
                    }
                }
            }
        }
        if (tile.contains("children"))
        {
            for (const auto& child : tile["children"])
            {
                require(child["geometricError"].get<double>() <=
                    tile["geometricError"].get<double>(), "nonmonotonic geometric error");
                visit(child);
            }
        }
    };
    visit(tileset["root"]);
    require(models >= 3 && coarse > 0, "missing building pyramid");
    require(textures.size() < models * 2, "scatter textures were not shared across tiles");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(output))
    {
        const auto extension = entry.path().extension();
        require(extension != ".jpg" && extension != ".png",
            "uncompressed or staging textures left in building output");
    }
}

void validatePositiveGlbBounds(const std::filesystem::path& root)
{
    aiScene scene;
    auto mesh = std::make_unique<aiMesh>();
    mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
    mesh->mNumVertices = 3;
    auto vertices = std::make_unique<aiVector3D[]>(3);
    vertices[0] = {1.f, 2.f, 3.f};
    vertices[1] = {2.f, 2.f, 3.f};
    vertices[2] = {1.f, 3.f, 3.f};
    mesh->mVertices = vertices.release();
    mesh->mNumFaces = 1;
    mesh->mFaces = std::make_unique<aiFace[]>(1).release();
    mesh->mFaces[0].mNumIndices = 3;
    auto indices = std::make_unique<uint32_t[]>(3);
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    mesh->mFaces[0].mIndices = indices.release();
    scene.mNumMeshes = 1;
    auto meshes = std::make_unique<aiMesh*[]>(1);
    meshes[0] = mesh.release();
    scene.mMeshes = meshes.release();
    scene.mNumMaterials = 1;
    auto materials = std::make_unique<aiMaterial*[]>(1);
    materials[0] = std::make_unique<aiMaterial>().release();
    scene.mMaterials = materials.release();
    auto node = std::make_unique<aiNode>("PositiveBounds");
    node->mNumMeshes = 1;
    node->mMeshes = std::make_unique<uint32_t[]>(1).release();
    node->mMeshes[0] = 0;
    scene.mRootNode = node.release();
    const auto path = root / "positive.glb";
    std::string error;
    require(cesium::writeGlb(scene, path, error), error);
    const auto document = readGlbJson(path);
    const auto position = document["meshes"][0]["primitives"][0]["attributes"]["POSITION"];
    const auto& accessor = document["accessors"][position.get<size_t>()];
    require(accessor["min"] == Json::array({1.f, 2.f, 3.f}) &&
        accessor["max"] == Json::array({2.f, 3.f, 3.f}),
        "Assimp positive accessor bounds were not corrected");
}

std::set<std::string> scatterTextureDigests(const std::filesystem::path& directory)
{
    std::set<std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory))
    {
        const auto extension = entry.path().extension();
        if (extension != ".png" && extension != ".jpg")
        {
            continue;
        }
        const auto digest = CalculateFileMd5(entry.path());
        require(digest.has_value(), "cannot digest scatter texture");
        constexpr char hex[] = "0123456789abcdef";
        std::string name;
        for (const auto value : *digest)
        {
            name.push_back(hex[value >> 4]);
            name.push_back(hex[value & 15]);
        }
        result.insert(name);
    }
    return result;
}

void validateTextureSelectionReplay(const std::filesystem::path& directory,
    const AmapVectorDecodedData& buildings, AmapBuildingFbxExportOptions settings)
{
    require(cv::imwrite((settings.wallTexturePath / "c-tall.png").string(),
        cv::Mat(512, 128, CV_8UC3, cv::Scalar(40, 220, 40))), "cannot create second tall wall");
    settings.maxWallMaterialsPerBucket = 8;
    AmapBuildingTextureSelections selections;
    std::string error;
    require(PrepareAmapBuildingTextureSelections(buildings, settings, selections, error), error);
    require(selections.size() == buildings.features.size(), "selection lost feature alignment");
    for (size_t index = selections.size(); index-- > 0;)
    {
        require(selections[index].has_value() &&
            selections[index]->GetWallTextureIndex() > 0 &&
            selections[index]->GetRoofTextureIndex() == 1,
            "selection ignored building height or roof aspect");
        AmapVectorDecodedData subset;
        subset.features.push_back(buildings.features[index]);
        auto& ring = subset.features.front().geometryParts.front();
        // 模拟粗层轮廓简化和分块重排；原始选图不能被再次计算。
        for (size_t vertex = ring.size(); vertex-- > 0;)
        {
            if (vertex % 2 != 0)
            {
                ring.erase(ring.begin() + vertex);
            }
        }
        settings.outputPath = directory / "replay" / std::to_string(index) / "model.glb";
        settings.SetTextureSelections({selections[index]});
        const auto replay = ExportAmapBuildingScene(subset, settings,
            [&](aiScene& scene, std::string&)
            {
                std::set<std::string> names;
                for (uint32_t material = 0; material < scene.mNumMaterials; ++material)
                {
                    aiString texture;
                    require(scene.mMaterials[material]->GetTexture(
                        aiTextureType_DIFFUSE, 0, &texture) == aiReturn_SUCCESS,
                        "replayed material lost its texture");
                    names.insert(std::filesystem::u8path(texture.C_Str()).stem().string());
                }
                const auto& selection = *selections[index];
                require(names == std::set<std::string>{
                    "building_wall_" + std::to_string(selection.GetWallTextureIndex()),
                    "building_roof_" + std::to_string(*selection.GetRoofTextureIndex())},
                    "subset or simplified outline changed the source texture");
                return true;
            });
        require(replay.success && replay.buildingCount == 1, replay.error);
    }
    settings.SetTextureSelections({selections.front()});
    require(!ExportAmapBuildingScene(buildings, settings,
        [](aiScene&, std::string&) { return true; }).success,
        "selection count mismatch was accepted");
    auto invalid = selections;
    invalid.front() = AmapBuildingTextureSelection(
        std::numeric_limits<size_t>::max(), size_t(1));
    settings.SetTextureSelections(invalid);
    require(!ExportAmapBuildingScene(buildings, settings,
        [](aiScene&, std::string&) { return true; }).success,
        "out-of-range texture selection was accepted");
    settings.SetTextureSelections({});
    auto sourceWithInvalid = buildings;
    sourceWithInvalid.features.insert(sourceWithInvalid.features.begin(), AmapVectorFeature{});
    require(PrepareAmapBuildingTextureSelections(
        sourceWithInvalid, settings, selections, error), error);
    require(selections.size() == sourceWithInvalid.features.size() && !selections.front(),
        "invalid feature shifted source selection indices");
    settings.cancellationRequested = [] { return true; };
    const size_t retained = selections.size();
    require(!PrepareAmapBuildingTextureSelections(
        buildings, settings, selections, error) && selections.size() == retained,
        "canceled preparation replaced the completed selection plan");
}

void validateMissingUidTextureReuse(const std::filesystem::path& root)
{
    const auto directory = root / "missing-uid";
    const auto walls = directory / "walls";
    const auto roofs = directory / "roofs";
    std::filesystem::create_directories(walls);
    std::filesystem::create_directories(roofs);
    require(cv::imwrite((walls / "a-short.png").string(),
        cv::Mat(64, 64, CV_8UC3, cv::Scalar(80, 80, 80))), "cannot create short wall");
    require(cv::imwrite((walls / "b-tall.png").string(),
        cv::Mat(512, 128, CV_8UC3, cv::Scalar(20, 180, 240))), "cannot create tall wall");
    require(cv::imwrite((roofs / "a-wide.png").string(),
        cv::Mat(64, 256, CV_8UC3, cv::Scalar(80, 80, 80))), "cannot create wide roof");
    require(cv::imwrite((roofs / "b-matched.png").string(),
        cv::Mat(256, 256, CV_8UC3, cv::Scalar(220, 100, 30))), "cannot create matched roof");
    AmapVectorDecodedData buildings;
    buildings.features = {makeBuilding(50., 50., 0), makeBuilding(180., 40., 0),
        makeBuilding(-40., 140., 0)};
    for (auto& feature : buildings.features)
    {
        feature.height = 80;
    }
    AmapBuildingFbxExportOptions settings;
    settings.centerWgs84 = LonLat(116.4, 39.9);
    settings.wallTexturePath = walls;
    settings.roofTexturePath = roofs;
    settings.SetChunkSizeMeters(100.);
    settings.outputPath = directory / "fbx" / "buildings.fbx";
    const auto fbx = ExportAmapBuildingsToFbx(buildings, settings);
    require(fbx.success, fbx.error);
    const auto expected = scatterTextureDigests(settings.outputPath.parent_path());
    require(expected.size() == 2, "FBX did not select matched wall and roof");

    CesiumBuildingOptions options;
    options.SetBounds({116.396, 116.404, 39.896, 39.904});
    options.SetBuildingOptions(settings);
    options.SetOutputPath(directory / "tileset");
    options.SetLodLevels(3);
    options.SetAtlasSize(512);
    std::string error;
    require(ExportCesiumBuildings(buildings, options, error), error);
    std::set<std::string> actual;
    for (const auto& entry : std::filesystem::directory_iterator(
             options.GetOutputPath() / "textures"))
    {
        actual.insert(entry.path().stem().string());
    }
    require(actual == expected,
        "UID=0 buildings do not reuse FBX height/aspect-matched scatter textures");
    validateTextureSelectionReplay(directory, buildings, settings);
}

bool runTest()
{
    const auto root = postprocess_test::MakeTestRoot("cesium-export");
    require(postprocess_test::ResetDirectory(root), "cannot reset test directory");
    validatePositiveGlbBounds(root);
    validateTextureEncoder(root);
    validateMissingUidTextureReuse(root);
    CesiumExportOptions terrain;
    terrain.SetOutputPath(root / "terrain");
    terrain.SetBounds({116.396, 116.404, 39.896, 39.904});
    terrain.SetSampleMeters(100.);
    terrain.SetSampler([](const LonLat& point) -> std::optional<double>
        {
            return 100. + 30. * std::sin(point.lon * 40.) * std::cos(point.lat * 40.);
        });
    std::string error;
    require(ExportCesiumTerrain(terrain, error), error);
    const auto layer = readJson(terrain.GetOutputPath() / "layer.json");
    require(layer["scheme"] == "tms" && layer["projection"] == "EPSG:4326",
        "wrong terrain scheme");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(terrain.GetOutputPath()))
    {
        if (entry.path().extension() == ".terrain")
        {
            TerrainReader(entry.path()).validate();
        }
    }
    CesiumExportOptions invalid = terrain;
    invalid.SetErrorMeters(0.);
    require(!ExportCesiumTerrain(invalid, error), "invalid error accepted");
    invalid = terrain;
    invalid.SetCancellation([] { return true; });
    require(!ExportCesiumTerrain(invalid, error), "cancel ignored");
    const auto textures = root / "source-textures";
    std::filesystem::create_directories(textures);
    require(cv::imwrite((textures / "wall.png").string(),
        cv::Mat(256, 256, CV_8UC3, cv::Scalar(40, 170, 240))), "cannot create texture");
    require(cv::imwrite((textures / "wall-red.png").string(),
        cv::Mat(256, 256, CV_8UC3, cv::Scalar(40, 40, 240))), "cannot create red texture");
    require(cv::imwrite((textures / "wall-green.png").string(),
        cv::Mat(256, 256, CV_8UC3, cv::Scalar(40, 240, 40))), "cannot create green texture");
    AmapVectorDecodedData buildings;
    buildings.features = {makeBuilding(50., 50., 1), makeBuilding(180., 40., 2),
        makeBuilding(-40., 140., 3)};
    CesiumBuildingOptions options;
    require(!options.GetTextureMipmaps(), "mipmap must default to off");
    options.SetBounds(terrain.GetBounds());
    options.SetOutputPath(root / "buildings");
    options.SetErrorMeters(2.);
    options.SetLodLevels(3);
    options.SetAtlasSize(512);
    AmapBuildingFbxExportOptions settings;
    settings.centerWgs84 = LonLat(116.4, 39.9);
    settings.wallTexturePath = textures;
    settings.roofTexturePath = textures;
    settings.SetChunkSizeMeters(100.);
    options.SetBuildingOptions(settings);
    auto sceneOptions = settings;
    sceneOptions.outputPath = root / "scene-count" / "model.glb";
    sceneOptions.wallTextureMode = AmapBuildingFbxTextureMode::Scattered;
    sceneOptions.roofTextureMode = AmapBuildingFbxRoofTextureMode::Scattered;
    uint32_t materialCount = 0;
    const auto sceneResult = ExportAmapBuildingScene(buildings, sceneOptions,
        [&](aiScene& scene, std::string&)
        {
            materialCount = scene.mNumMaterials;
            return true;
        });
    require(sceneResult.success && materialCount > 0 &&
        sceneResult.materialCount == materialCount, "scene writer lost material statistics");
    require(ExportCesiumBuildings(buildings, options, error), error);
    validateBuildings(options.GetOutputPath());
    const auto report = readJson(options.GetOutputPath() / "export-report.json");
    std::array<uint64_t, 3> triangles{};
    std::array<uint64_t, 3> vertices{};
    for (const auto& tile : report["tiles"])
    {
        const auto level = tile["level"].get<size_t>();
        triangles[level] += tile["triangles"].get<uint64_t>();
        vertices[level] += tile["inputRenderVertices"].get<uint64_t>();
    }
    require(triangles[0] < triangles[2] && vertices[0] < vertices[2],
        "coarse building pyramid did not simplify geometry and vertices");
    require(report["textureEncoding"]["codec"] == "ETC1S" &&
        report["textureEncoding"]["mipmaps"] == false, "missing encoding report");
    options.SetTextureMipmaps(true);
    options.SetOutputPath(root / "buildings-mips");
    require(ExportCesiumBuildings(buildings, options, error), error);
    validateBuildings(options.GetOutputPath(), true);
    options.SetCancellation([] { return true; });
    require(!ExportCesiumBuildings(buildings, options, error), "building cancel ignored");
    options.SetCancellation({});
    require(!ExportCesiumBuildings({}, options, error), "empty buildings accepted");
    std::cout << "CesiumExportTest passed\n";
    return true;
}

bool realData()
{
    const auto root = postprocess_test::MakeTestRoot("cesium-real-data");
    require(postprocess_test::ResetDirectory(root), "cannot reset real-data directory");
    const LonLat center(116.4, 39.9);
    CopernicusDemOptions dem;
    dem.centerWgs84 = center;
    dem.radiusMeters = 400.;
    std::string error;
    const auto elevation = CopernicusDemData::Prepare(dem, error);
    require(elevation != nullptr, error);
    CesiumBuildingOptions options;
    options.SetBounds(*CalculateGeographicBounds(center, 400.));
    options.SetOutputPath(root / "terrain");
    options.SetSampler([elevation](const LonLat& point) { return elevation->Sample(point); });
    require(ExportCesiumTerrain(options, error), error);
    AmapVectorDownloadOptions download;
    download.centerWgs84 = center;
    download.radiusMeters = 400.;
    download.zoom = 17;
    AmapVectorDownloader downloader;
    const auto result = downloader.Download(download);
    require(result.success, result.error);
    auto data = result.decodedData;
    std::erase_if(data.features, [](const auto& feature)
        {
            return ClassifyAmapVectorFeature(feature) != AmapVectorRenderCategory::Building;
        });
    AmapBuildingFbxExportOptions building;
    building.centerWgs84 = center;
    building.wallTexturePath = std::filesystem::u8path("贴图/墙体");
    building.roofTexturePath = std::filesystem::u8path("贴图/房顶");
    building.SetChunkSizeMeters(200.);
    options.SetBuildingOptions(building);
    options.SetOutputPath(root / "buildings");
    options.SetLodLevels(3);
    require(ExportCesiumBuildings(data, options, error), error);
    validateBuildings(options.GetOutputPath());
    options.SetTextureMipmaps(true);
    options.SetOutputPath(root / "buildings-mips");
    require(ExportCesiumBuildings(data, options, error), error);
    validateBuildings(options.GetOutputPath(), true);
    GaoDeDown imagery;
    imagery.SetCenterLonLat(center);
    imagery.SetRadius(400.);
    imagery.SetZoom(17);
    imagery.SetMaxDownQueue(4);
    imagery.SetPostProcessOutputPath(
        postprocess_test::PathToUtf8String(root / "xyz"));
    imagery.SetPostProcessEnabled(true);
    imagery.SetPostProcessMaxZoomOnlyEnabled(false);
    imagery.StartDown();
    require(std::filesystem::exists(root / "xyz" / "meta.json"), "XYZ metadata missing");
    std::cout << "Real Cesium exports: " << postprocess_test::PathToUtf8String(root) << '\n';
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    return postprocess_test::RunGuarded(
        argc > 1 && std::string(argv[1]) == "--real-data" ? realData : runTest);
}
