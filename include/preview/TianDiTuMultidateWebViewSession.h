#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace DomDownToolPreview
{
struct TianDiTuMultidateCookieResponse
{
    std::string cookie;
    std::string error;
};

struct TianDiTuMultidateFetchResponse
{
    int statusCode = 0;
    std::string body;
    std::string finalUrl;
    std::string error;
    std::string contentType;
};

class TianDiTuMultidateWebViewSession
{
  public:
    TianDiTuMultidateWebViewSession();

    ~TianDiTuMultidateWebViewSession();

    TianDiTuMultidateWebViewSession(const TianDiTuMultidateWebViewSession&) = delete;
    TianDiTuMultidateWebViewSession& operator=(const TianDiTuMultidateWebViewSession&) = delete;

    [[nodiscard]] TianDiTuMultidateCookieResponse requestSessionCookie();

    [[nodiscard]] TianDiTuMultidateFetchResponse fetchUrl(std::string_view url);

    [[nodiscard]] TianDiTuMultidateFetchResponse fetchBinaryUrl(std::string_view url);

    void cancelCurrent();

  private:
    struct ActiveRequest
    {
        uint64_t requestId = 0;
        TianDiTuMultidateCookieResponse response;
        bool completed = false;
    };

    struct ActiveFetchRequest
    {
        uint64_t requestId = 0;
        TianDiTuMultidateFetchResponse response;
        bool completed = false;
    };

    void runWebView();

    void showLoginWindow(uint64_t requestId);

    void requestCookieExtraction(uint64_t requestId);

    void extractSessionCookie(uint64_t requestId);

    [[nodiscard]] TianDiTuMultidateFetchResponse fetchUrl(std::string_view url,
                                                          bool binaryBody,
                                                          std::string_view timeoutError);

    void dispatchFetchRequest(uint64_t requestId, std::string url, bool binaryBody);

    void completeRequest(uint64_t requestId, TianDiTuMultidateCookieResponse response);

    void completeFetchRequest(uint64_t requestId, TianDiTuMultidateFetchResponse response);

    [[nodiscard]] std::string handlePageReady(const std::string& requestJson);

    [[nodiscard]] std::string handleLoginComplete(const std::string& requestJson);

    [[nodiscard]] std::string handleFetchComplete(const std::string& requestJson);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread webViewThread_;
    void* webView_ = nullptr;
    void* nativeWindow_ = nullptr;
    std::optional<ActiveRequest> activeRequest_;
    std::optional<ActiveFetchRequest> activeFetchRequest_;
    std::string startupError_;
    uint64_t nextRequestId_ = 0;
    bool startupComplete_ = false;
    bool stopping_ = false;
};
} // namespace DomDownToolPreview
