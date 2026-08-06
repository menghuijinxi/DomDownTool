#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "include/core/SkImage.h"

class GrDirectContext;
class SkCanvas;

namespace DomDownToolPreview
{
struct TileCacheStats
{
    size_t residentTiles = 0;
    size_t pendingTiles = 0;
    size_t cpuBytes = 0;
    size_t gpuBytes = 0;
};

class TileImageCache
{
  public:
    explicit TileImageCache(std::function<void()> requestRedraw,
                            size_t maximumBytes = 512ULL * 1024ULL * 1024ULL,
                            size_t workerCount = 0);

    ~TileImageCache();

    TileImageCache(const TileImageCache&) = delete;
    TileImageCache& operator=(const TileImageCache&) = delete;

    void beginFrame(SkCanvas& canvas);

    [[nodiscard]] sk_sp<SkImage> findOrRequest(const std::filesystem::path& path, SkCanvas& canvas);

    [[nodiscard]] bool isKnownMissing(const std::filesystem::path& path) const;

    void invalidate(const std::filesystem::path& path);

    void clear();

    [[nodiscard]] TileCacheStats stats() const;

  private:
    struct CacheEntry
    {
        sk_sp<SkImage> rasterImage;
        sk_sp<SkImage> textureImage;
        GrDirectContext* textureContext = nullptr;
        size_t byteSize = 0;
        uint64_t lastUsedFrame = 0;
        std::chrono::steady_clock::time_point retryAfter;
    };

    struct DecodeResult
    {
        std::filesystem::path path;
        sk_sp<SkImage> image;
        size_t byteSize = 0;
    };

    void request(const std::filesystem::path& path);

    void workerLoop();

    [[nodiscard]] static DecodeResult decode(const std::filesystem::path& path);

    void promoteCompleted();

    [[nodiscard]] sk_sp<SkImage> textureFor(CacheEntry& entry, SkCanvas& canvas);

    void evictIfNeeded();

    std::function<void()> requestRedraw_;
    const size_t maximumBytes_;
    mutable std::mutex workerMutex_;
    std::condition_variable workerCondition_;
    std::deque<std::filesystem::path> requests_;
    std::unordered_set<std::filesystem::path> pendingPaths_;
    std::deque<DecodeResult> completed_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    std::unordered_map<std::filesystem::path, CacheEntry> entries_;
    uint64_t frameNumber_ = 0;
    size_t cpuBytes_ = 0;
    size_t gpuBytes_ = 0;
};
} // namespace DomDownToolPreview
