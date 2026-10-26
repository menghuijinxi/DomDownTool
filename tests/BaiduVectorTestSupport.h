#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "openssl/evp.h"

#include "BaiduVectorDownloader.h"

namespace baidu_vector_test
{
inline void Append32(std::string& bytes, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<char>((value >> shift) & 255));
    }
}

inline std::string Block(const std::vector<uint32_t>& header,
                         const std::vector<std::string>& children)
{
    std::string bytes;
    for (const auto value : header)
    {
        Append32(bytes, value);
    }
    Append32(bytes, static_cast<uint32_t>(children.size()));
    for (const auto& child : children)
    {
        Append32(bytes, static_cast<uint32_t>(child.size()));
    }
    for (const auto& child : children)
    {
        bytes += child;
        bytes.resize((bytes.size() + 3) / 4 * 4, '\0');
    }
    return bytes;
}

inline std::string Fixture(bool empty = false, bool badIndex = false)
{
    const std::vector<std::pair<int32_t, int32_t>> points = {
        {0, 0}, {100, 0}, {100, 100}, {0, 100}, {25, 25}, {75, 25}, {75, 75}, {25, 75}};
    std::string vertices;
    int32_t x = 0;
    int32_t y = 0;
    for (const auto& point : points)
    {
        Append32(vertices, static_cast<uint32_t>((point.first - x) * 100));
        Append32(vertices, static_cast<uint32_t>((point.second - y) * 100));
        x = point.first;
        y = point.second;
    }
    std::string indices;
    for (const uint16_t value :
         {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7})
    {
        indices.push_back(static_cast<char>(value));
        indices.push_back('\0');
    }
    if (badIndex)
    {
        indices[0] = 99;
    }
    const auto polygon = Block({2, 0xffffffff, 12}, {vertices, "", indices});
    const auto building = Block({2, 0xffffffff, 12}, {vertices, indices});
    const auto point = Block({5, 0xffffffff, 2500, 2500, 3, 0}, {"fixture-guid", "测试地点"});
    const auto base = Block(
        {}, empty ? std::vector<std::string>{}
                  : std::vector<std::string>{Block({2, 15, 9000}, {Block({1, 504812}, {polygon})}),
                                             Block({2, 8, 9500}, {Block({1, 55}, {building})}),
                                             Block({2, 3, 9999}, {Block({1, 42}, {point})})});
    return Block({2, 2, 0, 0}, {base, Block({}, {}), Block({}, {}), Block({}, {}), Block({}, {})});
}

inline void Write(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output)
    {
        throw std::runtime_error("测试缓存写入失败。");
    }
}

inline void SeedCache(const DownTool::AmapVectorDownloadOptions& options,
                      const DownTool::BaiduVectorTile& tile)
{
    const auto root = options.GetBaiduCachePath() / "v088-20261021";
    for (const auto& selected : DownTool::BaiduVectorDownloader::SelectTiles(options))
    {
        Write(root / "vector" / std::to_string(selected.GetZoom()) /
                  std::to_string(selected.GetX()) / (std::to_string(selected.GetY()) + ".bin"),
              Fixture(selected.GetX() != tile.GetX() || selected.GetY() != tile.GetY()));
    }
    const std::string key = "types/42,55,504812";
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned length = 0;
    if (EVP_Digest(key.data(), key.size(), hash.data(), &length, EVP_sha256(), nullptr) != 1)
    {
        throw std::runtime_error("测试分类摘要失败。");
    }
    constexpr const char* digits = "0123456789abcdef";
    std::string name;
    for (unsigned index = 0; index < length; ++index)
    {
        name.push_back(digits[hash[index] >> 4]);
        name.push_back(digits[hash[index] & 15]);
    }
    Write(root / "feature-types" / (name + ".txt"),
          "baiduFeatureTypes([\"EDUCATION\",\"BUILDING\",\"\"]);");
}
} // namespace baidu_vector_test
