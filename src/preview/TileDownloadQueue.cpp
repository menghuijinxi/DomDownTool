#include "preview/TileDownloadQueue.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <windows.h>

#include "include/codec/SkCodec.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"

#include "preview/SkiaImageCodecs.h"

namespace
{
constexpr auto kRetryDelay = std::chrono::seconds(5);
constexpr long kRequestTimeoutSeconds = 20;
constexpr long kConnectTimeoutSeconds = 10;
constexpr size_t kMaximumImageDimension = 8192;

class CurlGlobalState
{
  public:
    CurlGlobalState()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            throw std::runtime_error("无法初始化 libcurl");
        }
    }

    ~CurlGlobalState()
    {
        curl_global_cleanup();
    }
};

class PartialFileGuard
{
  public:
    explicit PartialFileGuard(std::filesystem::path path) : path_(std::move(path))
    {
    }

    ~PartialFileGuard()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

  private:
    std::filesystem::path path_;
};

CurlGlobalState& curlGlobalState()
{
    static CurlGlobalState state;
    return state;
}

size_t writeFileData(void* data, size_t size, size_t count, void* userData)
{
    std::ofstream* stream = static_cast<std::ofstream*>(userData);
    const size_t byteCount = size * count;
    if (!stream || !stream->is_open())
    {
        return 0;
    }

    stream->write(static_cast<const char*>(data), static_cast<std::streamsize>(byteCount));
    return stream->good() ? byteCount : 0;
}

std::string trimAscii(std::string value)
{
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string lowerAscii(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

size_t writeHeaderData(void* data, size_t size, size_t count, void* userData)
{
    auto* headers = static_cast<std::map<std::string, std::string>*>(userData);
    const size_t byteCount = size * count;
    const std::string line(static_cast<const char*>(data), byteCount);
    const size_t separator = line.find(':');
    if (headers && separator != std::string::npos)
    {
        const std::string name = lowerAscii(trimAscii(line.substr(0, separator)));
        if (!name.empty())
        {
            (*headers)[name] = trimAscii(line.substr(separator + 1));
        }
    }
    return byteCount;
}

void cleanupCurlHeaders(curl_slist* headers)
{
    if (headers)
    {
        curl_slist_free_all(headers);
    }
}

bool replaceFileAtomically(const std::filesystem::path& source,
                           const std::filesystem::path& destination)
{
    return MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool requiresSuccessfulHttpStatus(const std::string& url)
{
    return url.starts_with("http://") || url.starts_with("https://");
}

bool isEmptyFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error) && !error &&
        std::filesystem::file_size(path, error) == 0 && !error;
}

bool writeTransparentPng(const std::filesystem::path& path)
{
    static constexpr unsigned char kTransparentPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return false;
    }
    file.write(reinterpret_cast<const char*>(kTransparentPng),
               static_cast<std::streamsize>(std::size(kTransparentPng)));
    return file.good();
}

std::filesystem::path emptyTileMarkerPath(const std::filesystem::path& tilePath)
{
    std::filesystem::path markerPath = tilePath;
    markerPath += L".empty";
    return markerPath;
}

std::filesystem::path failedTileMarkerPath(const std::filesystem::path& tilePath)
{
    std::filesystem::path markerPath = tilePath;
    markerPath += L".failed";
    return markerPath;
}

size_t readMarkerAttemptCount(const std::filesystem::path& markerPath)
{
    std::ifstream marker(markerPath, std::ios::binary);
    if (!marker.is_open())
    {
        return 0;
    }

    size_t attempts = 1;
    std::string line;
    while (std::getline(marker, line))
    {
        constexpr std::string_view kAttemptsPrefix = "attempts=";
        if (!line.starts_with(kAttemptsPrefix))
        {
            continue;
        }

        try
        {
            attempts = std::max<size_t>(1, std::stoull(
                line.substr(kAttemptsPrefix.size())));
        }
        catch (...)
        {
            attempts = 1;
        }
        break;
    }
    return attempts;
}

bool writeTileFailureMarker(const std::filesystem::path& markerPath,
                            std::string_view kind, size_t attempts)
{
    std::error_code error;
    std::filesystem::create_directories(markerPath.parent_path(), error);
    if (error)
    {
        return false;
    }

    std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
    if (!marker.is_open())
    {
        return false;
    }

    marker << kind << "\n";
    marker << "attempts=" << attempts << "\n";
    return marker.good();
}

DomDownToolPreview::TilePersistentFailureState recordEmptyTileResponse(
    const std::filesystem::path& tilePath)
{
    DomDownToolPreview::TilePersistentFailureState state =
        DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(tilePath);
    state.kind = DomDownToolPreview::TilePersistentFailureKind::EmptyResponse;
    state.attempts += 1;
    state.exhausted = state.attempts >= DomDownToolPreview::kPreviewTileFailureAttemptLimit;

    std::error_code error;
    std::filesystem::remove(failedTileMarkerPath(tilePath), error);
    if (!writeTileFailureMarker(emptyTileMarkerPath(tilePath), "empty-response",
                                state.attempts))
    {
        return DomDownToolPreview::TilePersistentFailureState{};
    }
    return state;
}

DomDownToolPreview::TilePersistentFailureState recordRequestFailed(
    const std::filesystem::path& tilePath)
{
    DomDownToolPreview::TilePersistentFailureState state =
        DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(tilePath);
    state.kind = DomDownToolPreview::TilePersistentFailureKind::RequestFailed;
    state.attempts += 1;
    state.exhausted = state.attempts >= DomDownToolPreview::kPreviewTileFailureAttemptLimit;

    std::error_code error;
    std::filesystem::remove(emptyTileMarkerPath(tilePath), error);
    if (!writeTileFailureMarker(failedTileMarkerPath(tilePath), "request-failed",
                                state.attempts))
    {
        return DomDownToolPreview::TilePersistentFailureState{};
    }
    return state;
}
} // namespace

DomDownToolPreview::TileDownloadQueue::TileDownloadQueue(
    std::function<void()> requestRedraw, size_t workerCount)
    : requestRedraw_(std::move(requestRedraw))
{
    (void)curlGlobalState();
    registerSkiaImageCodecs();
    const size_t hardwareThreadCount = std::max<size_t>(
        1, static_cast<size_t>(std::thread::hardware_concurrency()));
    if (workerCount == 0)
    {
        workerCount = hardwareThreadCount;
    }
    else
    {
        workerCount = std::min(workerCount, hardwareThreadCount);
    }
    workers_.reserve(workerCount);
    for (size_t index = 0; index < workerCount; ++index)
    {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

DomDownToolPreview::TileDownloadQueue::~TileDownloadQueue()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        pendingJobs_.clear();
        pendingPaths_.clear();
        visiblePaths_.clear();
    }
    condition_.notify_all();
    for (std::thread& worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}

void DomDownToolPreview::TileDownloadQueue::updateVisibleRequests(
    const std::vector<TileDownloadJob>& jobs)
{
    {
        const std::lock_guard lock(mutex_);
        if (stopping_)
        {
            return;
        }

        std::unordered_map<std::filesystem::path, const TileDownloadJob*> nextVisibleJobs;
        nextVisibleJobs.reserve(jobs.size());
        for (const TileDownloadJob& job : jobs)
        {
            if (job.source && !job.destination.empty())
            {
                nextVisibleJobs.emplace(job.destination, &job);
            }
        }
        visiblePaths_.clear();
        visiblePaths_.reserve(nextVisibleJobs.size());
        for (const auto& [destination, job] : nextVisibleJobs)
        {
            (void)job;
            visiblePaths_.insert(destination);
        }

        std::deque<PendingJob> retainedJobs;
        for (PendingJob& pending : pendingJobs_)
        {
            const auto visibleJob = nextVisibleJobs.find(pending.job.destination);
            if (visibleJob != nextVisibleJobs.end())
            {
                pending.job = *visibleJob->second;
                retainedJobs.push_back(std::move(pending));
            }
            else
            {
                pendingPaths_.erase(pending.job.destination);
            }
        }
        pendingJobs_ = std::move(retainedJobs);

        for (const TileDownloadJob& job : jobs)
        {
            if (!job.source || job.destination.empty() ||
                activePaths_.contains(job.destination) ||
                pendingPaths_.contains(job.destination) ||
                HasExhaustedPersistentFailures(job.destination))
            {
                continue;
            }

            pendingPaths_.insert(job.destination);
            pendingJobs_.push_back({job, std::chrono::steady_clock::now()});
        }
    }
    condition_.notify_all();
}

void DomDownToolPreview::TileDownloadQueue::cancelPending()
{
    const std::lock_guard lock(mutex_);
    pendingJobs_.clear();
    pendingPaths_.clear();
    visiblePaths_.clear();
}

std::vector<std::filesystem::path>
DomDownToolPreview::TileDownloadQueue::takeCompletedPaths()
{
    std::deque<DownloadResult> completed;
    {
        const std::lock_guard lock(mutex_);
        completed.swap(completed_);
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(completed.size());
    for (const DownloadResult& result : completed)
    {
        if (result.succeeded)
        {
            paths.push_back(result.destination);
        }
    }
    return paths;
}

DomDownToolPreview::TileDownloadStats DomDownToolPreview::TileDownloadQueue::stats() const
{
    const std::lock_guard lock(mutex_);
    return {pendingPaths_.size(), activePaths_.size(), failedTileCount_};
}

DomDownToolPreview::TilePersistentFailureState
DomDownToolPreview::TileDownloadQueue::GetPersistentFailureState(
    const std::filesystem::path& tilePath)
{
    const size_t emptyAttempts = readMarkerAttemptCount(emptyTileMarkerPath(tilePath));
    const size_t failedAttempts = readMarkerAttemptCount(failedTileMarkerPath(tilePath));
    if (emptyAttempts == 0 && failedAttempts == 0)
    {
        return {};
    }

    TilePersistentFailureState state;
    if (failedAttempts >= emptyAttempts)
    {
        state.kind = TilePersistentFailureKind::RequestFailed;
        state.attempts = failedAttempts;
    }
    else
    {
        state.kind = TilePersistentFailureKind::EmptyResponse;
        state.attempts = emptyAttempts;
    }
    state.exhausted = state.attempts >= kPreviewTileFailureAttemptLimit;
    return state;
}

bool DomDownToolPreview::TileDownloadQueue::HasExhaustedPersistentFailures(
    const std::filesystem::path& tilePath)
{
    return GetPersistentFailureState(tilePath).exhausted;
}

void DomDownToolPreview::TileDownloadQueue::ClearPersistentFailureState(
    const std::filesystem::path& tilePath)
{
    std::error_code error;
    std::filesystem::remove(emptyTileMarkerPath(tilePath), error);
    std::filesystem::remove(failedTileMarkerPath(tilePath), error);
}

void DomDownToolPreview::TileDownloadQueue::ClearPersistentFailureStatesUnder(
    const std::filesystem::path& root)
{
    if (root.empty())
    {
        return;
    }

    std::error_code error;
    if (!std::filesystem::exists(root, error) || error)
    {
        return;
    }

    std::filesystem::recursive_directory_iterator iterator(root, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end)
    {
        const std::filesystem::directory_entry entry = *iterator;
        if (!entry.is_regular_file(error) || error)
        {
            error.clear();
            iterator.increment(error);
            continue;
        }

        const std::filesystem::path extension = entry.path().extension();
        if (extension == L".empty" || extension == L".failed")
        {
            std::filesystem::remove(entry.path(), error);
            error.clear();
        }
        iterator.increment(error);
    }
}

bool DomDownToolPreview::TileDownloadQueue::download(const TileDownloadJob& job) const
{
    std::error_code error;
    std::filesystem::create_directories(job.destination.parent_path(), error);
    if (error)
    {
        return false;
    }

    std::filesystem::path partialPath = job.destination;
    partialPath += L".part";
    const PartialFileGuard partialFileGuard(partialPath);
    std::map<std::string, std::string> responseHeaders;
    if (job.customDownloader)
    {
        if (!job.customDownloader(job.request, partialPath))
        {
            return false;
        }
    }
    else
    {
        std::ofstream partialFile(partialPath, std::ios::binary | std::ios::trunc);
        if (!partialFile.is_open())
        {
            return false;
        }

        CURL* easy = curl_easy_init();
        if (!easy)
        {
            return false;
        }
        const auto easyCleanup = [](CURL* handle) { curl_easy_cleanup(handle); };
        std::unique_ptr<CURL, decltype(easyCleanup)> easyHandle(easy, easyCleanup);

        curl_slist* rawHeaders = nullptr;
        for (const std::string& header : job.request.headers)
        {
            curl_slist* nextHeaders = curl_slist_append(rawHeaders, header.c_str());
            if (!nextHeaders)
            {
                cleanupCurlHeaders(rawHeaders);
                return false;
            }
            rawHeaders = nextHeaders;
        }
        const std::unique_ptr<curl_slist, decltype(&cleanupCurlHeaders)> headers(
            rawHeaders, cleanupCurlHeaders);

        curl_easy_setopt(easy, CURLOPT_URL, job.request.url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, writeFileData);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &partialFile);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, writeHeaderData);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &responseHeaders);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "DomDownToolPreview/1.0");
        if (headers)
        {
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.get());
        }
        if (!job.request.proxy.empty())
        {
            curl_easy_setopt(easy, CURLOPT_PROXY, job.request.proxy.c_str());
        }

        const CURLcode result = curl_easy_perform(easy);
        long responseCode = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);
        partialFile.close();
        const bool httpStatusFailed = requiresSuccessfulHttpStatus(job.request.url) &&
            (responseCode < 200 || responseCode >= 300);
        if (result != CURLE_OK || httpStatusFailed)
        {
            return false;
        }
    }

    bool wroteTransparentPlaceholder = false;
    if (isEmptyFile(partialPath) && job.useTransparentPlaceholderOnEmptyResponse)
    {
        if (!writeTransparentPng(partialPath) || !validateImage(partialPath))
        {
            return false;
        }
        wroteTransparentPlaceholder = true;
    }
    else if (job.source->IsTileDownloadUnavailable(partialPath, responseHeaders, job.tile) ||
             !validateImage(partialPath))
    {
        return false;
    }

    if (!replaceFileAtomically(partialPath, job.destination))
    {
        return false;
    }

    if (wroteTransparentPlaceholder)
    {
        return recordEmptyTileResponse(job.destination).kind ==
            TilePersistentFailureKind::EmptyResponse;
    }

    ClearPersistentFailureState(job.destination);
    return true;
}

bool DomDownToolPreview::TileDownloadQueue::validateImage(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }
    const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)),
                                       std::istreambuf_iterator<char>());
    if (encoded.empty())
    {
        return false;
    }

    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(
        SkData::MakeWithCopy(encoded.data(), encoded.size()));
    if (!codec)
    {
        return false;
    }
    const SkISize dimensions = codec->dimensions();
    if (dimensions.width() <= 0 || dimensions.height() <= 0 ||
        static_cast<size_t>(dimensions.width()) > kMaximumImageDimension ||
        static_cast<size_t>(dimensions.height()) > kMaximumImageDimension)
    {
        return false;
    }

    const SkImageInfo imageInfo = SkImageInfo::MakeN32Premul(
        dimensions.width(), dimensions.height(), codec->getInfo().refColorSpace());
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(imageInfo))
    {
        return false;
    }
    return codec->getPixels(imageInfo, bitmap.getPixels(), bitmap.rowBytes()) ==
        SkCodec::kSuccess;
}

void DomDownToolPreview::TileDownloadQueue::workerLoop()
{
    while (true)
    {
        TileDownloadJob job;
        {
            std::unique_lock lock(mutex_);
            while (true)
            {
                condition_.wait(lock, [this] { return stopping_ || !pendingJobs_.empty(); });
                if (stopping_ && pendingJobs_.empty())
                {
                    return;
                }

                const auto now = std::chrono::steady_clock::now();
                const auto nextJob = std::min_element(
                    pendingJobs_.begin(), pendingJobs_.end(),
                    [now](const PendingJob& left, const PendingJob& right)
                    {
                        const bool leftReady = left.notBefore <= now;
                        const bool rightReady = right.notBefore <= now;
                        if (leftReady != rightReady)
                        {
                            return leftReady;
                        }
                        if (!leftReady)
                        {
                            return left.notBefore < right.notBefore;
                        }
                        if (left.job.priority != right.job.priority)
                        {
                            return left.job.priority < right.job.priority;
                        }
                        return left.notBefore < right.notBefore;
                    });
                if (nextJob->notBefore > now)
                {
                    condition_.wait_until(lock, nextJob->notBefore);
                    continue;
                }

                job = std::move(nextJob->job);
                pendingJobs_.erase(nextJob);
                pendingPaths_.erase(job.destination);
                activePaths_.insert(job.destination);
                break;
            }
        }

        const bool succeeded = download(job);
        {
            const std::lock_guard lock(mutex_);
            activePaths_.erase(job.destination);
            completed_.push_back({job.destination, succeeded});
            if (!succeeded)
            {
                ++failedTileCount_;
                const TilePersistentFailureState failureState =
                    recordRequestFailed(job.destination);
                if (visiblePaths_.contains(job.destination) && !stopping_ &&
                    !failureState.exhausted)
                {
                    pendingPaths_.insert(job.destination);
                    pendingJobs_.push_back({
                        std::move(job), std::chrono::steady_clock::now() + kRetryDelay});
                }
            }
        }
        condition_.notify_all();
        if (requestRedraw_)
        {
            requestRedraw_();
        }
    }
}
