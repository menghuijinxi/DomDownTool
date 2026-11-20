#pragma once

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "DataType.h"

namespace DomDownToolPreview
{
enum class PlaceSearchRequestKind
{
    Search,
    Suggestions
};

struct PlaceSearchResult
{
    std::string id;
    std::string name;
    std::string address;
    std::string district;
    std::string category;
    std::string level;
    std::optional<DownTool::LonLat> coordinateWgs84;
    double recommendedZoom = 16.;
};

struct PlaceSearchResponse
{
    uint64_t requestId = 0;
    PlaceSearchRequestKind kind = PlaceSearchRequestKind::Search;
    std::string query;
    std::vector<PlaceSearchResult> results;
    std::string error;
    bool usedGeocodeFallback = false;

    [[nodiscard]] bool succeeded() const
    {
        return error.empty();
    }
};

struct PlaceSearchHttpResponse
{
    long statusCode = 0;
    std::string body;
    std::string error;
};

[[nodiscard]] std::optional<size_t> findExactPlaceResultIndex(
    const std::vector<PlaceSearchResult>& results, std::string_view expectedId,
    std::string_view expectedName);

class AmapPlaceSearch
{
  public:
    using HttpGet = std::function<PlaceSearchHttpResponse(const std::string&)>;
    using CancelHttp = std::function<void()>;

    explicit AmapPlaceSearch(std::string apiKey, std::function<void()> completionCallback,
                             HttpGet httpGet = {});

    AmapPlaceSearch(std::function<void()> completionCallback, HttpGet webPageGet,
                    CancelHttp cancelHttp);

    ~AmapPlaceSearch();

    AmapPlaceSearch(const AmapPlaceSearch&) = delete;
    AmapPlaceSearch& operator=(const AmapPlaceSearch&) = delete;

    uint64_t submitSearch(std::string query);

    uint64_t submitSearch(std::string query, const DownTool::LonLat& centerWgs84,
                          std::string poiId = {});

    uint64_t submitSuggestions(std::string query, const DownTool::LonLat& centerWgs84,
                               std::chrono::milliseconds debounce =
                                   std::chrono::milliseconds(350));

    void cancelPending();

    [[nodiscard]] std::vector<PlaceSearchResponse> takeCompleted();

    [[nodiscard]] static std::string defaultApiKey();

  private:
    enum class Backend
    {
        WebService,
        WebPage
    };

    struct PendingRequest
    {
        uint64_t requestId = 0;
        PlaceSearchRequestKind kind = PlaceSearchRequestKind::Search;
        std::string query;
        std::string poiId;
        std::optional<DownTool::LonLat> centerWgs84;
        std::chrono::steady_clock::time_point executeAfter;
    };

    uint64_t submit(PendingRequest request);

    [[nodiscard]] bool isCurrent(const PendingRequest& request) const;

    [[nodiscard]] PlaceSearchResponse execute(const PendingRequest& request) const;

    [[nodiscard]] PlaceSearchResponse executeSearch(const PendingRequest& request) const;

    [[nodiscard]] PlaceSearchResponse executeSuggestions(const PendingRequest& request) const;

    void workerLoop();

    std::string apiKey_;
    std::function<void()> completionCallback_;
    HttpGet httpGet_;
    CancelHttp cancelHttp_;
    Backend backend_ = Backend::WebService;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<PendingRequest> pending_;
    std::deque<PlaceSearchResponse> completed_;
    std::thread worker_;
    uint64_t latestRequestId_ = 0;
    bool stopping_ = false;
};
} // namespace DomDownToolPreview
