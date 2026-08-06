#include "preview/TileImageCache.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"
#include "include/gpu/ganesh/SkImageGanesh.h"

#include "preview/SkiaImageCodecs.h"

namespace
{
constexpr auto kMissingTileRetryDelay = std::chrono::seconds(2);
constexpr size_t kMaximumWorkerCount = 8;
} // namespace

DomDownToolPreview::TileImageCache::TileImageCache(std::function<void()> requestRedraw,
                                                   size_t maximumBytes, size_t workerCount)
    : requestRedraw_(std::move(requestRedraw)), maximumBytes_(std::max<size_t>(1, maximumBytes))
{
    registerSkiaImageCodecs();
    if (workerCount == 0)
    {
        const size_t hardwareThreads = std::max(1U, std::thread::hardware_concurrency());
        workerCount = std::clamp(hardwareThreads / 2, size_t{2}, kMaximumWorkerCount);
    }

    workers_.reserve(workerCount);
    for (size_t index = 0; index < workerCount; index++)
    {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

DomDownToolPreview::TileImageCache::~TileImageCache()
{
    {
        const std::lock_guard lock(workerMutex_);
        stopping_ = true;
    }
    workerCondition_.notify_all();
    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void DomDownToolPreview::TileImageCache::beginFrame(SkCanvas& canvas)
{
    (void)canvas;
    frameNumber_++;
    promoteCompleted();
    evictIfNeeded();
}

sk_sp<SkImage> DomDownToolPreview::TileImageCache::findOrRequest(const std::filesystem::path& path,
                                                                 SkCanvas& canvas)
{
    const auto entryIterator = entries_.find(path);
    if (entryIterator != entries_.end())
    {
        CacheEntry& entry = entryIterator->second;
        entry.lastUsedFrame = frameNumber_;
        if (entry.rasterImage)
        {
            return textureFor(entry, canvas);
        }

        if (std::chrono::steady_clock::now() < entry.retryAfter)
        {
            return nullptr;
        }
    }

    request(path);
    return nullptr;
}

bool DomDownToolPreview::TileImageCache::isKnownMissing(
    const std::filesystem::path& path) const
{
    const auto entryIterator = entries_.find(path);
    return entryIterator != entries_.end() && !entryIterator->second.rasterImage &&
        entryIterator->second.retryAfter != std::chrono::steady_clock::time_point{};
}

void DomDownToolPreview::TileImageCache::invalidate(const std::filesystem::path& path)
{
    const auto entryIterator = entries_.find(path);
    if (entryIterator == entries_.end())
    {
        return;
    }

    cpuBytes_ -= std::min(cpuBytes_, entryIterator->second.byteSize);
    if (entryIterator->second.textureImage)
    {
        gpuBytes_ -= std::min(gpuBytes_, entryIterator->second.byteSize);
    }
    entries_.erase(entryIterator);
}

void DomDownToolPreview::TileImageCache::clear()
{
    entries_.clear();
    cpuBytes_ = 0;
    gpuBytes_ = 0;
}

DomDownToolPreview::TileCacheStats DomDownToolPreview::TileImageCache::stats() const
{
    TileCacheStats result;
    result.residentTiles = entries_.size();
    result.cpuBytes = cpuBytes_;
    result.gpuBytes = gpuBytes_;
    {
        const std::lock_guard lock(workerMutex_);
        result.pendingTiles = pendingPaths_.size();
    }
    return result;
}

void DomDownToolPreview::TileImageCache::request(const std::filesystem::path& path)
{
    {
        const std::lock_guard lock(workerMutex_);
        if (stopping_ || !pendingPaths_.insert(path).second)
        {
            return;
        }
        requests_.push_back(path);
    }
    workerCondition_.notify_one();
}

void DomDownToolPreview::TileImageCache::workerLoop()
{
    while (true)
    {
        std::filesystem::path path;
        {
            std::unique_lock lock(workerMutex_);
            workerCondition_.wait(lock, [this] { return stopping_ || !requests_.empty(); });
            if (stopping_ && requests_.empty())
            {
                return;
            }

            path = std::move(requests_.front());
            requests_.pop_front();
        }

        DecodeResult result = decode(path);
        {
            const std::lock_guard lock(workerMutex_);
            pendingPaths_.erase(path);
            completed_.push_back(std::move(result));
        }
        if (requestRedraw_)
        {
            requestRedraw_();
        }
    }
}

DomDownToolPreview::TileImageCache::DecodeResult DomDownToolPreview::TileImageCache::decode(
    const std::filesystem::path& path)
{
    DecodeResult result;
    result.path = path;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return result;
    }

    std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
    if (encoded.empty())
    {
        return result;
    }

    sk_sp<SkData> encodedData = SkData::MakeWithCopy(encoded.data(), encoded.size());
    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(encodedData));
    if (!codec)
    {
        return result;
    }

    const SkImageInfo imageInfo =
        SkImageInfo::MakeN32Premul(codec->dimensions().width(), codec->dimensions().height(),
                                   codec->getInfo().refColorSpace());
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(imageInfo))
    {
        return result;
    }

    const SkCodec::Result decodeResult =
        codec->getPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes());
    if (decodeResult != SkCodec::kSuccess && decodeResult != SkCodec::kIncompleteInput)
    {
        return result;
    }

    bitmap.setImmutable();
    result.image = SkImages::RasterFromBitmap(bitmap);
    if (result.image)
    {
        result.byteSize = bitmap.computeByteSize();
    }
    return result;
}

void DomDownToolPreview::TileImageCache::promoteCompleted()
{
    std::deque<DecodeResult> completed;
    {
        const std::lock_guard lock(workerMutex_);
        completed.swap(completed_);
    }

    const auto retryAfter = std::chrono::steady_clock::now() + kMissingTileRetryDelay;
    for (DecodeResult& result : completed)
    {
        CacheEntry& entry = entries_[result.path];
        cpuBytes_ -= std::min(cpuBytes_, entry.byteSize);
        if (entry.textureImage)
        {
            gpuBytes_ -= std::min(gpuBytes_, entry.byteSize);
        }

        entry.rasterImage = std::move(result.image);
        entry.textureImage.reset();
        entry.textureContext = nullptr;
        entry.byteSize = result.byteSize;
        entry.lastUsedFrame = frameNumber_;
        entry.retryAfter = entry.rasterImage ? std::chrono::steady_clock::time_point{} : retryAfter;
        cpuBytes_ += entry.byteSize;
    }
}

sk_sp<SkImage> DomDownToolPreview::TileImageCache::textureFor(CacheEntry& entry, SkCanvas& canvas)
{
    GrDirectContext* context = GrAsDirectContext(canvas.recordingContext());
    if (!context)
    {
        return entry.rasterImage;
    }

    if (!entry.textureImage || entry.textureContext != context)
    {
        if (entry.textureImage)
        {
            gpuBytes_ -= std::min(gpuBytes_, entry.byteSize);
        }
        entry.textureImage = SkImages::TextureFromImage(context, entry.rasterImage);
        entry.textureContext = entry.textureImage ? context : nullptr;
        if (entry.textureImage)
        {
            gpuBytes_ += entry.byteSize;
        }
    }
    return entry.textureImage ? entry.textureImage : entry.rasterImage;
}

void DomDownToolPreview::TileImageCache::evictIfNeeded()
{
    while (cpuBytes_ + gpuBytes_ > maximumBytes_ && !entries_.empty())
    {
        const auto oldestEntry = std::min_element(
            entries_.begin(), entries_.end(), [](const auto& left, const auto& right)
            { return left.second.lastUsedFrame < right.second.lastUsedFrame; });
        if (oldestEntry == entries_.end() || oldestEntry->second.lastUsedFrame == frameNumber_)
        {
            return;
        }

        const CacheEntry& entry = oldestEntry->second;
        cpuBytes_ -= std::min(cpuBytes_, entry.byteSize);
        if (entry.textureImage)
        {
            gpuBytes_ -= std::min(gpuBytes_, entry.byteSize);
        }
        entries_.erase(oldestEntry);
    }
}
