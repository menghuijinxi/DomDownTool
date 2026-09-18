#include <algorithm>
#include <atomic>
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
#include <thread>
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

void writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        throw std::runtime_error("无法写入测试标记文件");
    }
    file << text;
    if (!file.good())
    {
        throw std::runtime_error("测试标记文件写入不完整");
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

std::filesystem::path emptyMarkerPathFor(const std::filesystem::path& destination)
{
    std::filesystem::path path = destination;
    path += L".empty";
    return path;
}

std::filesystem::path failedMarkerPathFor(const std::filesystem::path& destination)
{
    std::filesystem::path path = destination;
    path += L".failed";
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
        const std::filesystem::path emptySourcePath = testRoot / "empty.bin";
        writeFile(validSourcePath, pngBytes);
        writeFile(invalidSourcePath, {'n', 'o', 't', '-', 'a', 'n', '-', 'i', 'm', 'a', 'g', 'e'});
        writeFile(emptySourcePath, {});

        std::condition_variable completionCondition;
        std::mutex completionMutex;
        DownTool::DownToolBase source;
        DomDownToolPreview::TileDownloadQueue queue(
            [&completionCondition] { completionCondition.notify_all(); }, 1);
        DomDownToolPreview::TileDownloadQueue automaticQueue(
            [&completionCondition] { completionCondition.notify_all(); });
        const size_t hardwareThreadCount = std::max<size_t>(
            1, static_cast<size_t>(std::thread::hardware_concurrency()));
        if (automaticQueue.GetWorkerCount() != hardwareThreadCount)
        {
            throw std::runtime_error("自动下载并发数没有使用 CPU 硬件线程数");
        }
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
        (void)queue.takeCompletedPaths();

        const std::filesystem::path customPath = testRoot / "cache" / "custom.png";
        std::atomic_bool customDownloaderCalled = false;
        source.SetUrlTemplate("https://example.invalid/custom.png");
        DomDownToolPreview::TileDownloadJob customJob{
            customPath, tile, &source, source.BuildTileHttpRequest(tile)};
        customJob.customDownloader =
            [&pngBytes, &customDownloaderCalled](
                const DownTool::TileHttpRequest& request,
                const std::filesystem::path& partialPath)
            {
                if (request.url.empty())
                {
                    return false;
                }
                customDownloaderCalled = true;
                try
                {
                    writeFile(partialPath, pngBytes);
                }
                catch (...)
                {
                    return false;
                }
                return true;
            };
        queue.updateVisibleRequests({customJob});
        if (!waitUntil([&customPath] { return std::filesystem::exists(customPath); },
                       completionCondition, completionMutex))
        {
            throw std::runtime_error("自定义下载器没有写入有效瓦块");
        }
        if (!customDownloaderCalled || std::filesystem::exists(partialPathFor(customPath)) ||
            std::filesystem::file_size(customPath) != pngBytes.size())
        {
            throw std::runtime_error("自定义下载器没有复用原子落盘和图片校验路径");
        }
        (void)queue.takeCompletedPaths();

        const std::filesystem::path highPriorityPath = testRoot / "cache" / "high.png";
        const std::filesystem::path lowPriorityPath = testRoot / "cache" / "low.png";
        source.SetUrlTemplate(fileUrl(validSourcePath));
        queue.updateVisibleRequests({
            {lowPriorityPath, tile, &source, source.BuildTileHttpRequest(tile), 100.0},
            {highPriorityPath, tile, &source, source.BuildTileHttpRequest(tile), 1.0}});
        if (!waitUntil(
                [&highPriorityPath, &lowPriorityPath]
                {
                    return std::filesystem::exists(highPriorityPath) &&
                        std::filesystem::exists(lowPriorityPath);
                },
                completionCondition, completionMutex))
        {
            throw std::runtime_error("中心优先测试瓦块未在超时时间内完成");
        }
        const std::vector<std::filesystem::path> priorityCompletions =
            queue.takeCompletedPaths();
        if (priorityCompletions.size() < 2 || priorityCompletions[0] != highPriorityPath)
        {
            throw std::runtime_error("瓦块下载没有按中心优先级执行");
        }

        const std::filesystem::path transparentPath = testRoot / "cache" / "transparent.png";
        source.SetUrlTemplate(fileUrl(emptySourcePath));
        DomDownToolPreview::TileDownloadJob transparentJob{
            transparentPath, tile, &source, source.BuildTileHttpRequest(tile)};
        transparentJob.useTransparentPlaceholderOnEmptyResponse = true;
        queue.updateVisibleRequests({transparentJob});
        if (!waitUntil([&transparentPath] { return std::filesystem::exists(transparentPath); },
                       completionCondition, completionMutex))
        {
            throw std::runtime_error("空响应没有写入透明瓦块");
        }
        std::ifstream transparentFile(transparentPath, std::ios::binary);
        std::vector<unsigned char> transparentBytes(
            (std::istreambuf_iterator<char>(transparentFile)), std::istreambuf_iterator<char>());
        transparentFile.close();
        if (transparentBytes.size() < 8 || transparentBytes[0] != 0x89 ||
            transparentBytes[1] != 0x50 || transparentBytes[2] != 0x4E ||
            transparentBytes[3] != 0x47 ||
            !std::filesystem::exists(emptyMarkerPathFor(transparentPath)))
        {
            throw std::runtime_error("透明瓦块不是 PNG 编码，或缺少空响应标记");
        }
        const DomDownToolPreview::TilePersistentFailureState emptyFailureState =
            DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(transparentPath);
        if (emptyFailureState.kind !=
                DomDownToolPreview::TilePersistentFailureKind::EmptyResponse ||
            emptyFailureState.attempts != 1 || emptyFailureState.exhausted)
        {
            throw std::runtime_error("空响应标记没有记录失败次数");
        }
        std::vector<std::filesystem::path> transparentCompletions;
        if (!waitUntil(
                [&queue, &transparentCompletions]
                {
                    transparentCompletions = queue.takeCompletedPaths();
                    return !transparentCompletions.empty();
                },
                completionCondition, completionMutex))
        {
            throw std::runtime_error("透明瓦块任务没有完成出队");
        }

        source.SetUrlTemplate(fileUrl(validSourcePath));
        queue.updateVisibleRequests(
            {{transparentPath, tile, &source, source.BuildTileHttpRequest(tile)}});
        std::vector<std::filesystem::path> refreshCompletions;
        if (!waitUntil(
                [&queue, &refreshCompletions, &transparentPath]
                {
                    refreshCompletions = queue.takeCompletedPaths();
                    return !refreshCompletions.empty() &&
                        !std::filesystem::exists(emptyMarkerPathFor(transparentPath));
                },
                completionCondition, completionMutex))
        {
            throw std::runtime_error("真实瓦块覆盖后没有清除空响应标记");
        }

        const std::filesystem::path exhaustedPath = testRoot / "cache" / "exhausted.png";
        const std::filesystem::path retryablePath = testRoot / "cache" / "retryable.png";
        writeTextFile(failedMarkerPathFor(retryablePath),
                      "request-failed\nattempts=" +
                          std::to_string(
                              DomDownToolPreview::kPreviewTileFailureAttemptLimit - 1) +
                          "\n");
        queue.updateVisibleRequests(
            {{retryablePath, tile, &source, source.BuildTileHttpRequest(tile)}});
        if (!waitUntil(
                [&retryablePath]
                {
                    return std::filesystem::exists(retryablePath) &&
                        !std::filesystem::exists(failedMarkerPathFor(retryablePath));
                },
                completionCondition, completionMutex))
        {
            throw std::runtime_error("未达到失败上限的瓦块没有在刷新后重新请求");
        }
        (void)queue.takeCompletedPaths();

        writeTextFile(failedMarkerPathFor(exhaustedPath),
                      "request-failed\nattempts=" +
                          std::to_string(DomDownToolPreview::kPreviewTileFailureAttemptLimit) +
                          "\n");
        source.SetUrlTemplate(fileUrl(validSourcePath));
        queue.updateVisibleRequests(
            {{exhaustedPath, tile, &source, source.BuildTileHttpRequest(tile)}});
        std::this_thread::sleep_for(200ms);
        const DomDownToolPreview::TileDownloadStats exhaustedStats = queue.stats();
        const DomDownToolPreview::TilePersistentFailureState exhaustedFailureState =
            DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(exhaustedPath);
        if (std::filesystem::exists(exhaustedPath) || exhaustedStats.queuedTiles != 0 ||
            exhaustedStats.activeTiles != 0 || !exhaustedFailureState.exhausted)
        {
            throw std::runtime_error("达到失败上限的瓦块仍然被自动入队");
        }

        DomDownToolPreview::TileDownloadQueue::ClearPersistentFailureStatesUnder(
            testRoot / "cache");
        const DomDownToolPreview::TilePersistentFailureState clearedFailureState =
            DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(exhaustedPath);
        if (clearedFailureState.kind != DomDownToolPreview::TilePersistentFailureKind::None ||
            clearedFailureState.attempts != 0 || clearedFailureState.exhausted)
        {
            throw std::runtime_error("刷新瓦片没有清除达到上限的失败状态");
        }
        queue.updateVisibleRequests(
            {{exhaustedPath, tile, &source, source.BuildTileHttpRequest(tile)}});
        if (!waitUntil([&exhaustedPath] { return std::filesystem::exists(exhaustedPath); },
                       completionCondition, completionMutex))
        {
            throw std::runtime_error("刷新清除失败状态后，瓦块没有重新请求");
        }
        (void)queue.takeCompletedPaths();

        const std::filesystem::path invalidPath = testRoot / "cache" / "invalid.png";
        source.SetUrlTemplate(fileUrl(invalidSourcePath));
        queue.updateVisibleRequests(
            {{invalidPath, tile, &source, source.BuildTileHttpRequest(tile)}});
        if (!waitUntil([&queue] { return queue.stats().failedTiles > 0; }, completionCondition,
                       completionMutex))
        {
            throw std::runtime_error("无效响应没有进入失败状态");
        }
        const DomDownToolPreview::TilePersistentFailureState failedState =
            DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(invalidPath);
        if (failedState.kind != DomDownToolPreview::TilePersistentFailureKind::RequestFailed ||
            failedState.attempts == 0 || failedState.exhausted ||
            !std::filesystem::exists(failedMarkerPathFor(invalidPath)))
        {
            throw std::runtime_error("请求失败没有写入可诊断的失败标记");
        }
        queue.cancelPending();
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
