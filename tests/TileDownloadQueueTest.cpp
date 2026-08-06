#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "DownToolBase.h"
#include "preview/TileDownloadQueue.h"

namespace
{
using namespace std::chrono_literals;

std::string fileUrl(const std::filesystem::path& path)
{
    const std::u8string genericPath = std::filesystem::absolute(path).generic_u8string();
    return "file:///" +
        std::string(reinterpret_cast<const char*>(genericPath.data()), genericPath.size());
}

void writeFile(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error("无法写入测试源文件");
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file.good())
    {
        throw std::runtime_error("测试源文件写入不完整");
    }
}

bool waitUntil(const std::function<bool()>& predicate, std::condition_variable& condition,
               std::mutex& mutex)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::unique_lock lock(mutex);
        condition.wait_for(lock, 100ms);
    }
    return predicate();
}

std::filesystem::path partialPathFor(const std::filesystem::path& destination)
{
    std::filesystem::path path = destination;
    path += L".part";
    return path;
}

std::vector<unsigned char> makePngBytes()
{
    static constexpr unsigned char kPngBytes[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x04, 0x00, 0x00, 0x00, 0xB5, 0x1C, 0x0C, 0x02, 0x00, 0x00, 0x00, 0x0B,
        0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x64, 0xF8, 0x0F, 0x00, 0x01,
        0x05, 0x01, 0x01, 0x27, 0x18, 0xE3, 0x66, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    return {std::begin(kPngBytes), std::end(kPngBytes)};
}
} // namespace

int main()
{
    const std::filesystem::path testRoot =
        std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) / "tmp" / "tile-download-queue-test";
    std::error_code error;
    std::filesystem::remove_all(testRoot, error);

    try
    {
        const std::vector<unsigned char> pngBytes = makePngBytes();
        if (pngBytes.empty())
        {
            throw std::runtime_error("无法创建 PNG 测试数据");
        }

        const std::filesystem::path validSourcePath = testRoot / "source.png";
        const std::filesystem::path invalidSourcePath = testRoot / "source.txt";
        writeFile(validSourcePath, pngBytes);
        writeFile(invalidSourcePath, {'n', 'o', 't', '-', 'a', 'n', '-', 'i', 'm', 'a', 'g', 'e'});

        std::condition_variable completionCondition;
        std::mutex completionMutex;
        DownTool::DownToolBase source;
        DomDownToolPreview::TileDownloadQueue queue(
            [&completionCondition] { completionCondition.notify_all(); }, 1);
        const DownTool::Tile tile(1, 2, 3);

        const std::filesystem::path validPath = testRoot / "cache" / "valid.png";
        source.SetUrlTemplate(fileUrl(validSourcePath));
        queue.updateVisibleRequests(
            {{validPath, tile, &source, source.BuildTileHttpRequest(tile)}});
        if (!waitUntil([&validPath] { return std::filesystem::exists(validPath); },
                       completionCondition, completionMutex))
        {
            throw std::runtime_error("有效瓦块未在超时时间内落盘");
        }
        if (std::filesystem::exists(partialPathFor(validPath)) ||
            std::filesystem::file_size(validPath) != pngBytes.size())
        {
            throw std::runtime_error("有效瓦块没有通过原子落盘或图片校验");
        }

        const std::filesystem::path invalidPath = testRoot / "cache" / "invalid.png";
        source.SetUrlTemplate(fileUrl(invalidSourcePath));
        queue.updateVisibleRequests(
            {{invalidPath, tile, &source, source.BuildTileHttpRequest(tile)}});
        if (!waitUntil([&queue] { return queue.stats().failedTiles > 0; }, completionCondition,
                       completionMutex))
        {
            throw std::runtime_error("无效响应没有进入失败状态");
        }
        queue.setEnabled(false);
        if (std::filesystem::exists(invalidPath) ||
            std::filesystem::exists(partialPathFor(invalidPath)))
        {
            throw std::runtime_error("无效响应污染了正式缓存或临时文件");
        }

        std::filesystem::remove_all(testRoot, error);
        std::cout << "TileDownloadQueueTest passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::filesystem::remove_all(testRoot, error);
        std::cerr << "TileDownloadQueueTest failed: " << exception.what() << '\n';
        return 1;
    }
}
