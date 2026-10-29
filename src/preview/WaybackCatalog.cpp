#include "preview/WaybackCatalog.h"

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include "curl/curl.h"

#include "../SystemProxy.h"

namespace
{
class CurlGlobalState
{
  public:
    CurlGlobalState()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            throw std::runtime_error("无法初始化 Wayback 网络模块。");
        }
    }

    ~CurlGlobalState() { curl_global_cleanup(); }
};

size_t AppendCatalog(char* bytes, size_t size, size_t count, void* context)
{
    auto& text = *static_cast<std::string*>(context);
    constexpr size_t maximumBytes = 4 * 1024 * 1024;
    if (size != 0 && count > (maximumBytes - text.size()) / size)
    {
        return 0;
    }
    text.append(bytes, size * count);
    return size * count;
}

int CancelCatalog(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const std::stop_token*>(context)->stop_requested() ? 1 : 0;
}

std::string FetchCatalog(std::stop_token stop)
{
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(),
                                                                   curl_easy_cleanup);
    if (!easy)
    {
        throw std::runtime_error("无法初始化 Wayback 网络请求。");
    }
    std::string text;
    const std::string proxy = DownTool::detail::GetSystemHttpsProxy();
    curl_easy_setopt(easy.get(), CURLOPT_URL,
                     "https://s3-us-west-2.amazonaws.com/"
                     "config.maptiles.arcgis.com/waybackconfig.json");
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendCatalog);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &text);
    curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_USERAGENT, "DomDownToolPreview/1.0");
    curl_easy_setopt(easy.get(), CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
    curl_easy_setopt(easy.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy.get(), CURLOPT_XFERINFOFUNCTION, CancelCatalog);
    curl_easy_setopt(easy.get(), CURLOPT_XFERINFODATA, &stop);
    if (!proxy.empty())
    {
        curl_easy_setopt(easy.get(), CURLOPT_PROXY, proxy.c_str());
    }
    CURLcode result = curl_easy_perform(easy.get());
    long status = 0;
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    if (!stop.stop_requested() && !proxy.empty() && (result != CURLE_OK || status != 200))
    {
        // 目录与瓦片分属不同域名，代理故障不应阻止可直连的公开目录。
        text.clear();
        curl_easy_setopt(easy.get(), CURLOPT_PROXY, "");
        result = curl_easy_perform(easy.get());
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    }
    if (result != CURLE_OK)
    {
        throw std::runtime_error("Wayback 目录请求失败：" +
                                 std::string(curl_easy_strerror(result)));
    }
    if (status != 200)
    {
        throw std::runtime_error("Wayback 目录 HTTP " + std::to_string(status));
    }
    return text;
}
} // namespace

DomDownToolPreview::WaybackCatalogResult::WaybackCatalogResult(
    std::vector<DownTool::WaybackRelease> releases)
    : releases_(std::move(releases))
{
}

DomDownToolPreview::WaybackCatalogResult::WaybackCatalogResult(std::string error)
    : error_(std::move(error))
{
}

DomDownToolPreview::WaybackCatalog::WaybackCatalog(std::function<void()> onComplete, Fetch fetch)
    : onComplete_(std::move(onComplete)), fetch_(fetch ? std::move(fetch) : FetchCatalog)
{
    static CurlGlobalState curlState;
}

bool DomDownToolPreview::WaybackCatalog::Refresh()
{
    {
        const std::lock_guard lock(mutex_);
        if (loading_)
        {
            return false;
        }
        loading_ = true;
        completed_.reset();
    }
    worker_ = std::jthread([this](std::stop_token stop) {
        std::optional<WaybackCatalogResult> result;
        try
        {
            result.emplace(DownTool::ParseWaybackCatalog(fetch_(stop)));
        }
        catch (const std::exception& error)
        {
            result.emplace(std::string(error.what()));
        }
        if (stop.stop_requested())
        {
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            completed_ = std::move(result);
            loading_ = false;
        }
        onComplete_();
    });
    return true;
}

bool DomDownToolPreview::WaybackCatalog::IsLoading() const
{
    const std::lock_guard lock(mutex_);
    return loading_;
}

std::optional<DomDownToolPreview::WaybackCatalogResult> DomDownToolPreview::WaybackCatalog::
    TakeCompleted()
{
    const std::lock_guard lock(mutex_);
    auto result = std::move(completed_);
    completed_.reset();
    return result;
}
