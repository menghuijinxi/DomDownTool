#include "CesiumGlbWriter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <vector>

#include "assimp/Exporter.hpp"
#include "nlohmann/json.hpp"

#include "CesiumExportSupport.h"

namespace
{
uint32_t readWord(const unsigned char* address)
{
    uint32_t value = 0;
    std::memcpy(&value, address, sizeof(value));
    return value;
}

void writeWord(std::ofstream& stream, uint32_t value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void configureBasisTextures(nlohmann::json& document, bool mipmaps)
{
    std::set<size_t> basisImages;
    const auto images = document.find("images");
    if (images == document.end())
    {
        return;
    }
    for (size_t index = 0; index < images->size(); ++index)
    {
        auto& image = (*images)[index];
        if (image.value("uri", "").ends_with(".ktx2"))
        {
            image["mimeType"] = "image/ktx2";
            basisImages.insert(index);
        }
    }
    if (basisImages.empty())
    {
        return;
    }
    for (const char* key : {"extensionsUsed", "extensionsRequired"})
    {
        auto& extensions = document[key];
        if (extensions.is_null())
        {
            extensions = nlohmann::json::array();
        }
        if (std::find(extensions.begin(), extensions.end(), "KHR_texture_basisu") ==
            extensions.end())
        {
            extensions.push_back("KHR_texture_basisu");
        }
    }
    auto& samplers = document["samplers"];
    if (samplers.is_null() || samplers.empty())
    {
        samplers = nlohmann::json::array({nlohmann::json::object()});
    }
    for (auto& sampler : samplers)
    {
        sampler["magFilter"] = 9729; // LINEAR。
        sampler["minFilter"] = mipmaps ? 9987 : 9729;
    }
    // Assimp 只输出普通 source；KTX2 必须通过扩展指定，不能伪装为 JPEG/PNG。
    for (auto& texture : document["textures"])
    {
        if (texture.contains("source") &&
            basisImages.contains(texture["source"].get<size_t>()))
        {
            texture["extensions"]["KHR_texture_basisu"] = {{"source", texture["source"]}};
            texture.erase("source");
            if (!texture.contains("sampler"))
            {
                texture["sampler"] = 0;
            }
        }
    }
}
} // namespace

bool DownTool::cesium::writeGlb(
    const aiScene& scene, const std::filesystem::path& path,
    std::string& error, bool textureMipmaps)
{
    Assimp::Exporter exporter;
    const auto* blob = exporter.ExportToBlob(&scene, "glb2");
    if (!blob || !blob->data || blob->size < 28)
    {
        error = "建筑 GLB 生成失败：" + std::string(exporter.GetErrorString());
        return false;
    }
    const auto* bytes = static_cast<const unsigned char*>(blob->data);
    const uint32_t jsonLength = readWord(bytes + 12);
    const size_t binaryHeader = size_t(jsonLength) + 20;
    if (binaryHeader + 8 > blob->size)
    {
        error = "Assimp GLB 缺少二进制块。";
        return false;
    }
    const uint32_t binaryLength = readWord(bytes + binaryHeader);
    const auto* binary = bytes + binaryHeader + 8;
    if (binaryHeader + 8 + binaryLength > blob->size)
    {
        error = "Assimp GLB 二进制块长度无效。";
        return false;
    }
    auto document = nlohmann::json::parse(bytes + 20, bytes + 20 + jsonLength);
    // Assimp 6.0.4 会把正数 accessor 的 min 初始化为 0；
    // 仅修正其已声明的 float 属性边界，网格与索引内容保持不变。
    for (auto& accessor : document["accessors"])
    {
        if (accessor.value("componentType", 0) != 5126 ||
            !accessor.contains("bufferView") ||
            (!accessor.contains("min") && !accessor.contains("max")))
        {
            continue;
        }
        const auto type = accessor.value("type", "");
        const size_t components = type == "VEC3" ? 3 : type == "VEC2" ? 2 :
            type == "VEC4" ? 4 : type == "SCALAR" ? 1 : 0;
        if (components == 0)
        {
            continue;
        }
        const auto& view = document["bufferViews"][accessor["bufferView"].get<size_t>()];
        const size_t stride = view.value("byteStride", components * sizeof(float));
        const size_t offset = view.value("byteOffset", size_t(0)) +
            accessor.value("byteOffset", size_t(0));
        const size_t count = accessor["count"].get<size_t>();
        std::vector<float> minimum(components, std::numeric_limits<float>::max());
        std::vector<float> maximum(components, std::numeric_limits<float>::lowest());
        for (size_t index = 0; index < count; ++index)
        {
            const size_t address = offset + index * stride;
            if (address + components * sizeof(float) > binaryLength)
            {
                error = "Assimp GLB accessor 超出二进制块。";
                return false;
            }
            for (size_t axis = 0; axis < components; ++axis)
            {
                float value = 0.f;
                std::memcpy(&value, binary + address + axis * sizeof(float), sizeof(float));
                minimum[axis] = std::min(minimum[axis], value);
                maximum[axis] = std::max(maximum[axis], value);
            }
        }
        accessor["min"] = minimum;
        accessor["max"] = maximum;
    }
    configureBasisTextures(document, textureMipmaps);
    std::string json = document.dump();
    while (json.size() % 4 != 0)
    {
        json.push_back(' ');
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    writeWord(stream, 0x46546c67);
    writeWord(stream, 2);
    writeWord(stream, uint32_t(28 + json.size() + binaryLength));
    writeWord(stream, uint32_t(json.size()));
    writeWord(stream, 0x4e4f534a);
    stream.write(json.data(), std::streamsize(json.size()));
    writeWord(stream, binaryLength);
    writeWord(stream, 0x004e4942);
    stream.write(reinterpret_cast<const char*>(binary), binaryLength);
    stream.flush();
    if (!stream.good())
    {
        error = "建筑 GLB 写入失败：" + pathUtf8(path);
        return false;
    }
    return true;
}
