#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
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
    bool useTransparentPlaceholderOnEmptyResponse = false;
    std::function<bool(const DownTool::TileHttpRequest&, const std::filesystem::path&)>
        customDownloader;
};

struct TileDownloadStats
{
    size_t queuedTiles = 0;
    size_t activeTiles = 0;
    size_t failedTiles = 0;
};

inline constexpr size_t kPreviewTileFailureAttemptLimit = 12;

enum class TilePersistentFailureKind
{
    None,
    EmptyResponse,
    RequestFailed
};

struct TilePersistentFailureState
{
    TilePersistentFailureKind kind = TilePersistentFailureKind::None;
    size_t attempts = 0;
    bool exhausted = false;
};

class TileDownloadQueue
{
  public:
    explicit TileDownloadQueue(std::function<void()> requestRedraw,
                               size_t workerCount = 0);

    ~TileDownloadQueue();

    TileDownloadQueue(const TileDownloadQueue&) = delete;
    TileDownloadQueue& operator=(const TileDownloadQueue&) = delete;

    [[nodiscard]] size_t GetWorkerCount() const noexcept { return workers_.size(); }

    void updateVisibleRequests(const std::vector<TileDownloadJob>& jobs);

    void cancelPending();

    [[nodiscard]] std::vector<std::filesystem::path> takeCompletedPaths();

    [[nodiscard]] TileDownloadStats stats() const;

    [[nodiscard]] static TilePersistentFailureState GetPersistentFailureState(
        const std::filesystem::path& tilePath);

    [[nodiscard]] static bool HasExhaustedPersistentFailures(
        const std::filesystem::path& tilePath);

    static void ClearPersistentFailureState(const std::filesystem::path& tilePath);

    static void ClearPersistentFailureStatesUnder(const std::filesystem::path& root);

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

    [[nodiscard]] bool download(
        const TileDownloadJob& job, const std::atomic_bool& cancellationRequested) const;

    [[nodiscard]] static bool validateImage(const std::filesystem::path& path);

    void workerLoop();

    std::function<void()> requestRedraw_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<PendingJob> pendingJobs_;
    std::unordered_set<std::filesystem::path> pendingPaths_;
    std::unordered_map<std::filesystem::path, std::shared_ptr<std::atomic_bool>> activeRequests_;
    std::unordered_set<std::filesystem::path> visiblePaths_;
    std::deque<DownloadResult> completed_;
    std::vector<std::thread> workers_;
    size_t failedTileCount_ = 0;
    bool stopping_ = false;
};
} // namespace DomDownToolPreview
