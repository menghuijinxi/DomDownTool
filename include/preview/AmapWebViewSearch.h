#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "preview/AmapPlaceSearch.h"

namespace DomDownToolPreview
{
class AmapWebViewSearch
{
  public:
    AmapWebViewSearch();

    ~AmapWebViewSearch();

    AmapWebViewSearch(const AmapWebViewSearch&) = delete;
    AmapWebViewSearch& operator=(const AmapWebViewSearch&) = delete;

    [[nodiscard]] PlaceSearchHttpResponse get(const std::string& url);

    void cancelCurrent();

  private:
    enum class RequestKind
    {
        Fetch,
        SearchPage
    };

    struct ActiveRequest
    {
        uint64_t requestId = 0;
        RequestKind kind = RequestKind::Fetch;
        std::string url;
        PlaceSearchHttpResponse response;
        bool waitingForVerification = false;
        bool verificationConfirmed = false;
        bool completed = false;
    };

    void runWebView();

    void dispatchActiveRequest(uint64_t requestId);

    void completeWithError(uint64_t requestId, std::string error);

    void showVerification(const std::string& url, bool navigate);

    void retryAfterVerification();

    [[nodiscard]] std::string handlePageReady(const std::string& requestJson);

    [[nodiscard]] std::string handleTransportResult(const std::string& requestJson);

    [[nodiscard]] std::string handleRiskDetected(const std::string& requestJson);

    [[nodiscard]] std::string handleVerificationComplete(const std::string& requestJson);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread webViewThread_;
    void* webView_ = nullptr;
    void* nativeWindow_ = nullptr;
    std::optional<ActiveRequest> activeRequest_;
    std::string startupError_;
    uint64_t nextRequestId_ = 0;
    bool startupComplete_ = false;
    bool pageReady_ = false;
    bool retryOnPageReady_ = false;
    bool stopping_ = false;
};
} // namespace DomDownToolPreview
