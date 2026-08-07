#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "DownToolBase.h"

namespace DomDownToolPreview
{
struct TileDownloadJob
{
    std::filesystem::path destination;
    DownTool::Tile tile;
    DownTool::DownToolBase* source = nullptr;
    DownTool::TileHttpRequest request;
    double priority = 0.0;
};

struct TileDownloadStats
{
    size_t queuedTiles = 0;
    size_t activeTiles = 0;
    size_t failedTiles = 0;
};

class TileDownloadQueue
{
  public:
    explicit TileDownloadQueue(std::function<void()> requestRedraw,
                               size_t workerCount = 0);

    ~TileDownloadQueue();

    TileDownloadQueue(const TileDownloadQueue&) = delete;
    TileDownloadQueue& operator=(const TileDownloadQueue&) = delete;

    void setEnabled(bool enabled);

    [[nodiscard]] bool enabled() const;

    [[nodiscard]] size_t GetWorkerCount() const noexcept { return workers_.size(); }

    void updateVisibleRequests(const std::vector<TileDownloadJob>& jobs);

    void cancelPending();

    [[nodiscard]] std::vector<std::filesystem::path> takeCompletedPaths();

    [[nodiscard]] TileDownloadStats stats() const;

  private:
    struct PendingJob
    {
        TileDownloadJob job;
        std::chrono::steady_clock::time_point notBefore;
    };

    struct DownloadResult
    {
        std::filesystem::path destination;
        bool succeeded = false;
    };

    [[nodiscard]] bool download(const TileDownloadJob& job) const;

    [[nodiscard]] static bool validateImage(const std::filesystem::path& path);

    void workerLoop();

    std::function<void()> requestRedraw_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<PendingJob> pendingJobs_;
    std::unordered_set<std::filesystem::path> pendingPaths_;
    std::unordered_set<std::filesystem::path> activePaths_;
    std::unordered_set<std::filesystem::path> visiblePaths_;
    std::deque<DownloadResult> completed_;
    std::vector<std::thread> workers_;
    size_t failedTileCount_ = 0;
    bool enabled_ = true;
    bool stopping_ = false;
};
} // namespace DomDownToolPreview
