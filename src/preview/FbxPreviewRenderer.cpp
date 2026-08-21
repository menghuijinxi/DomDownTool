#include "preview/FbxPreviewRenderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkVertices.h"

#include "preview/SkiaImageCodecs.h"

namespace
{
constexpr std::size_t kMaximumPreviewVertices = 250000;
constexpr std::size_t kMaximumPreviewTriangles = 500000;
// DX12 是主路径；Skia 仅作为不支持 D3D12 的回退，仍覆盖常见多贴图 FBX。
constexpr std::size_t kMaximumPreviewTextures = 16;
constexpr int kMaximumPreviewTextureDimension = 4096;
constexpr uint32_t kNoTextureIndex = std::numeric_limits<uint32_t>::max();

struct Bounds
{
    std::array<float, 3> minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    std::array<float, 3> maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void IncludePoint(Bounds& bounds, const std::array<float, 3>& point)
{
    for (std::size_t index = 0; index < point.size(); ++index)
    {
        bounds.minimum[index] = std::min(bounds.minimum[index], point[index]);
        bounds.maximum[index] = std::max(bounds.maximum[index], point[index]);
    }
}

aiMatrix4x4 NodeTransform(const aiNode* node, const aiMatrix4x4& parent)
{
    return parent * node->mTransformation;
}

sk_sp<SkImage> LoadPreviewTexture(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return nullptr;
    }
    std::vector<uint8_t> encoded;
    encoded.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (encoded.empty())
    {
        return nullptr;
    }

    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(
        SkData::MakeWithCopy(encoded.data(), encoded.size()));
    if (!codec)
    {
        return nullptr;
    }
    const SkISize sourceDimensions = codec->dimensions();
    if (sourceDimensions.width() <= 0 || sourceDimensions.height() <= 0)
    {
        return nullptr;
    }
    const int maximumSourceDimension =
        std::max(sourceDimensions.width(), sourceDimensions.height());
    const float decodeScale = maximumSourceDimension > kMaximumPreviewTextureDimension ?
        static_cast<float>(kMaximumPreviewTextureDimension) /
            static_cast<float>(maximumSourceDimension) :
        1.F;
    // 预览不需要保留导出贴图的完整分辨率，避免高分辨率源图耗尽内存。
    const SkISize dimensions = codec->getScaledDimensions(decodeScale);
    if (dimensions.width() > kMaximumPreviewTextureDimension ||
        dimensions.height() > kMaximumPreviewTextureDimension)
    {
        return nullptr;
    }
    const SkImageInfo imageInfo = SkImageInfo::MakeN32Premul(
        dimensions.width(), dimensions.height(), codec->getInfo().refColorSpace());
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(imageInfo) ||
        codec->getPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes()) != SkCodec::kSuccess)
    {
        return nullptr;
    }
    bitmap.setImmutable();
    return SkImages::RasterFromBitmap(bitmap);
}

} // namespace

bool DomDownToolPreview::FbxPreviewRenderer::load(
    const std::vector<std::filesystem::path>& paths, std::string& error)
{
    registerSkiaImageCodecs();
    clear();
    Bounds bounds;
    bool loadedAny = false;
    std::unordered_map<std::string, uint32_t> textureIndices;

    const auto loadMaterialTexture = [&](const aiScene* scene, unsigned int materialIndex,
                                         const std::filesystem::path& fbxPath) -> uint32_t
    {
        if (scene == nullptr || materialIndex >= scene->mNumMaterials)
        {
            return kNoTextureIndex;
        }
        aiString textureName;
        if (scene->mMaterials[materialIndex]->GetTexture(
                aiTextureType_DIFFUSE, 0, &textureName) != AI_SUCCESS ||
            textureName.length == 0 || textureName.C_Str()[0] == '*')
        {
            return kNoTextureIndex;
        }
        const std::filesystem::path texturePath =
            (fbxPath.parent_path() / std::filesystem::u8path(textureName.C_Str())).lexically_normal();
        const std::string textureKey = PathToUtf8(texturePath);
        const auto existing = textureIndices.find(textureKey);
        if (existing != textureIndices.end())
        {
            return existing->second;
        }
        if (textures_.size() >= kMaximumPreviewTextures)
        {
            return kNoTextureIndex;
        }
        sk_sp<SkImage> image = LoadPreviewTexture(texturePath);
        if (!image)
        {
            return kNoTextureIndex;
        }
        const uint32_t index = static_cast<uint32_t>(textures_.size());
        textures_.push_back({std::move(image), texturePath});
        textureIndices.emplace(textureKey, index);
        return index;
    };

    for (const std::filesystem::path& path : paths)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(
            PathToUtf8(path), aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                aiProcess_ImproveCacheLocality);
        if (scene == nullptr || scene->mRootNode == nullptr)
        {
            error = "无法读取 FBX：" + PathToUtf8(path) + "；" + importer.GetErrorString();
            continue;
        }

        const auto appendNode = [&](const auto& self, const aiNode* node,
                                     const aiMatrix4x4& parent) -> void
        {
            const aiMatrix4x4 transform = NodeTransform(node, parent);
            for (unsigned int meshIndex = 0; meshIndex < node->mNumMeshes; ++meshIndex)
            {
                const aiMesh* mesh = scene->mMeshes[node->mMeshes[meshIndex]];
                if (mesh == nullptr || mesh->mNumVertices == 0 ||
                    vertices_.size() + mesh->mNumVertices > kMaximumPreviewVertices)
                {
                    continue;
                }
                const uint32_t textureIndex = loadMaterialTexture(
                    scene, mesh->mMaterialIndex, path);
                const bool hasTextureCoordinates = mesh->HasTextureCoords(0);
                const std::size_t vertexOffset = vertices_.size();
                for (unsigned int vertexIndex = 0; vertexIndex < mesh->mNumVertices;
                     ++vertexIndex)
                {
                    const aiVector3D point = transform * mesh->mVertices[vertexIndex];
                    const std::array<float, 3> position{point.x, point.y, point.z};
                    const aiVector3D textureCoordinate = hasTextureCoordinates ?
                        mesh->mTextureCoords[0][vertexIndex] : aiVector3D{};
                    vertices_.push_back(Vertex{
                        position, {textureCoordinate.x, textureCoordinate.y}});
                    IncludePoint(bounds, position);
                }
                for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex)
                {
                    const aiFace& face = mesh->mFaces[faceIndex];
                    if (face.mNumIndices != 3 ||
                        triangles_.size() >= kMaximumPreviewTriangles)
                    {
                        continue;
                    }
                    triangles_.push_back(Triangle{
                        {static_cast<uint32_t>(vertexOffset + face.mIndices[0]),
                         static_cast<uint32_t>(vertexOffset + face.mIndices[1]),
                         static_cast<uint32_t>(vertexOffset + face.mIndices[2])},
                        textureIndex});
                }
                loadedAny = true;
            }
            for (unsigned int childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
            {
                self(self, node->mChildren[childIndex], transform);
            }
        };
        appendNode(appendNode, scene->mRootNode, aiMatrix4x4());
    }

    if (!loadedAny || vertices_.empty() || triangles_.empty())
    {
        clear();
        if (error.empty())
        {
            error = "没有找到可预览的 FBX 三角网格。";
        }
        return false;
    }

    for (std::size_t index = 0; index < center_.size(); ++index)
    {
        center_[index] = (bounds.minimum[index] + bounds.maximum[index]) * 0.5F;
    }
    const float spanX = bounds.maximum[0] - bounds.minimum[0];
    const float spanY = bounds.maximum[1] - bounds.minimum[1];
    const float spanZ = bounds.maximum[2] - bounds.minimum[2];
    modelScale_ = std::max({spanX, spanY, spanZ, 1e-3F});
    error.clear();
    return true;
}

void DomDownToolPreview::FbxPreviewRenderer::clear()
{
    vertices_.clear();
    triangles_.clear();
    textures_.clear();
    center_ = {};
    modelScale_ = 1.F;
    yaw_ = -0.65;
    pitch_ = 0.55;
    zoom_ = 1.0;
    mode_ = FbxPreviewMode::TexturedWireframe;
}

void DomDownToolPreview::FbxPreviewRenderer::rotateBy(double deltaX, double deltaY)
{
    yaw_ += deltaX * 0.008;
    pitch_ = std::clamp(pitch_ + deltaY * 0.008, -1.35, 1.35);
}

void DomDownToolPreview::FbxPreviewRenderer::zoomBy(double delta)
{
    zoom_ = std::clamp(zoom_ * std::pow(1.12, delta), 0.15, 12.0);
}

void DomDownToolPreview::FbxPreviewRenderer::draw(
    SkCanvas& canvas, int width, int height, float dpiScale) const
{
    if (empty() || width <= 0 || height <= 0)
    {
        return;
    }

    struct ProjectedVertex
    {
        SkPoint point{};
        float depth = 0.F;
    };
    const double cosYaw = std::cos(yaw_);
    const double sinYaw = std::sin(yaw_);
    const double cosPitch = std::cos(pitch_);
    const double sinPitch = std::sin(pitch_);
    const float scale = std::min(width, height) * 0.76F * static_cast<float>(zoom_) /
        modelScale_;
    const float centerX = static_cast<float>(width) * 0.5F;
    const float centerY = static_cast<float>(height) * 0.54F;
    std::vector<ProjectedVertex> projected;
    projected.reserve(vertices_.size());
    for (const Vertex& vertex : vertices_)
    {
        const double x = static_cast<double>(vertex.position[0] - center_[0]);
        const double y = static_cast<double>(vertex.position[1] - center_[1]);
        const double z = static_cast<double>(vertex.position[2] - center_[2]);
        const float rotatedX = static_cast<float>(cosYaw * x - sinYaw * y);
        const float rotatedY = static_cast<float>(sinYaw * x + cosYaw * y);
        const float elevatedY = static_cast<float>(cosPitch * rotatedY - sinPitch * z);
        const float depth = static_cast<float>(sinPitch * rotatedY + cosPitch * z);
        projected.push_back({SkPoint::Make(centerX + rotatedX * scale,
                                            centerY - elevatedY * scale),
                             depth});
    }

    struct DrawTriangle
    {
        uint32_t index = 0;
        float depth = 0.F;
    };
    std::vector<DrawTriangle> order;
    order.reserve(triangles_.size());
    for (uint32_t index = 0; index < triangles_.size(); ++index)
    {
        const Triangle& triangle = triangles_[index];
        order.push_back({index, (projected[triangle.indices[0]].depth +
                                 projected[triangle.indices[1]].depth +
                                 projected[triangle.indices[2]].depth) /
                                3.F});
    }
    std::sort(order.begin(), order.end(),
              [](const DrawTriangle& left, const DrawTriangle& right)
              { return left.depth < right.depth; });

    const bool drawSurface = mode_ != FbxPreviewMode::Wireframe;
    const bool drawWireframe = mode_ != FbxPreviewMode::Textured;
    const SkSamplingOptions textureSampling(SkFilterMode::kLinear, SkMipmapMode::kNone);
    SkPaint fillPaint;
    fillPaint.setAntiAlias(true);
    fillPaint.setStyle(SkPaint::kFill_Style);
    SkPaint edgePaint;
    edgePaint.setAntiAlias(true);
    edgePaint.setStyle(SkPaint::kStroke_Style);
    edgePaint.setStrokeWidth(std::max(0.45F, dpiScale * 0.65F));
    edgePaint.setColor(SkColorSetARGB(54, 220, 236, 229));

    for (const DrawTriangle& item : order)
    {
        const Triangle& triangle = triangles_[item.index];
        const ProjectedVertex& first = projected[triangle.indices[0]];
        const ProjectedVertex& second = projected[triangle.indices[1]];
        const ProjectedVertex& third = projected[triangle.indices[2]];
        const float cross = (second.point.fX - first.point.fX) *
                (third.point.fY - first.point.fY) -
            (second.point.fY - first.point.fY) * (third.point.fX - first.point.fX);
        if (std::abs(cross) < 0.01F)
        {
            continue;
        }
        SkPathBuilder pathBuilder;
        pathBuilder.moveTo(first.point);
        pathBuilder.lineTo(second.point);
        pathBuilder.lineTo(third.point);
        pathBuilder.close();
        const SkPath path = pathBuilder.detach();
        const bool hasTexture = drawSurface &&
            triangle.textureIndex < textures_.size() && textures_[triangle.textureIndex].image;
        if (hasTexture)
        {
            const sk_sp<SkImage>& image = textures_[triangle.textureIndex].image;
            const SkPoint positions[] = {first.point, second.point, third.point};
            const auto texturePoint = [&](uint32_t vertexIndex) {
                const Vertex& source = vertices_[vertexIndex];
                return SkPoint::Make(
                    source.texCoord[0] * static_cast<float>(image->width()),
                    (1.F - source.texCoord[1]) * static_cast<float>(image->height()));
            };
            const SkPoint textureCoordinates[] = {
                texturePoint(triangle.indices[0]), texturePoint(triangle.indices[1]),
                texturePoint(triangle.indices[2])};
            const sk_sp<SkVertices> mesh = SkVertices::MakeCopy(
                SkVertices::kTriangles_VertexMode, 3, positions, textureCoordinates, nullptr);
            SkPaint texturePaint;
            texturePaint.setAntiAlias(true);
            texturePaint.setShader(image->makeShader(textureSampling));
            canvas.drawVertices(mesh, SkBlendMode::kSrcOver, texturePaint);
        }
        else if (drawSurface)
        {
            const float light = std::clamp(0.55F + (cross > 0.F ? 0.22F : 0.F) -
                                               item.depth / modelScale_ * 0.08F,
                                           0.32F, 0.9F);
            fillPaint.setColor(SkColorSetARGB(235,
                                              static_cast<U8CPU>(50.F + 90.F * light),
                                              static_cast<U8CPU>(115.F + 100.F * light),
                                              static_cast<U8CPU>(105.F + 85.F * light)));
            canvas.drawPath(path, fillPaint);
        }
        if (drawWireframe)
        {
            canvas.drawPath(path, edgePaint);
        }
    }
}
