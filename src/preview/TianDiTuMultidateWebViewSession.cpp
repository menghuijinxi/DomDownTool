#include "preview/TianDiTuMultidateWebViewSession.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <wrl/event.h>

#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "webview/webview.h"
#include "WebView2.h"

#include "preview/SearchTrace.h"

namespace
{
using Json = nlohmann::json;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

constexpr std::string_view kTianDiTuMultidateHomeUrl =
    "https://image.tianditu.gov.cn/multidate";
constexpr std::wstring_view kTianDiTuCookieUrl =
    L"https://image.tianditu.gov.cn/";
constexpr auto kPageStartupTimeout = 20s;
constexpr auto kFetchTimeout = 20s;
constexpr UINT_PTR kLoginWindowSubclassId = 1;

struct ExtractedCookieSet
{
    std::string header;
    uint32_t cookieCount = 0;
    uint32_t sessionCount = 0;
    uint32_t persistentCount = 0;
};

std::string GetJsonString(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return {};
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_string()
        ? iterator->get<std::string>()
        : std::string{};
}

int64_t GetJsonInteger(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return 0;
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_number_integer()
        ? iterator->get<int64_t>()
        : 0;
}

std::optional<Json> firstBindingArgument(const std::string& requestJson)
{
    const Json root = Json::parse(requestJson, nullptr, false);
    if (root.is_discarded() || !root.is_array() || root.empty() ||
        !root.front().is_object())
    {
        return std::nullopt;
    }
    return Json(root.front());
}

std::string makeBindingResult(bool succeeded, std::string_view message = {})
{
    Json result = {{"ok", succeeded}};
    if (!message.empty())
    {
        result["message"] = std::string(message);
    }
    return result.dump();
}

std::string describeWebViewException(const webview::exception& exception)
{
    const webview::error_info& error = exception.error();
    return fmt::format("code={} message={} what={}", static_cast<int>(error.code()),
                       error.message(), exception.what());
}

std::string wideToUtf8(const wchar_t* value)
{
    if (!value || *value == L'\0')
    {
        return {};
    }
    const int sourceLength = lstrlenW(value);
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, sourceLength, nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, sourceLength, result.data(), length, nullptr,
                        nullptr);
    return result;
}

ComPtr<ICoreWebView2CookieManager> cookieManagerFromWebView(webview::webview& webView,
                                                            std::string& error)
{
    const webview::result<void*> controllerResult = webView.browser_controller();
    if (controllerResult.has_error())
    {
        error = "无法访问 WebView2 控制器。";
        return {};
    }

    auto* controller = static_cast<ICoreWebView2Controller*>(controllerResult.value());
    ComPtr<ICoreWebView2> coreWebView;
    if (FAILED(controller->get_CoreWebView2(coreWebView.GetAddressOf())) || !coreWebView)
    {
        error = "无法访问 WebView2 页面实例。";
        return {};
    }

    ComPtr<ICoreWebView2_2> coreWebView2;
    if (FAILED(coreWebView.As(&coreWebView2)) || !coreWebView2)
    {
        error = "当前 WebView2 运行时不支持 CookieManager。";
        return {};
    }

    ComPtr<ICoreWebView2CookieManager> cookieManager;
    if (FAILED(coreWebView2->get_CookieManager(cookieManager.GetAddressOf())) ||
        !cookieManager)
    {
        error = "无法读取天地图多时相 Cookie。";
        return {};
    }
    return cookieManager;
}

class CoTaskMemString
{
  public:
    ~CoTaskMemString()
    {
        if (value_)
        {
            CoTaskMemFree(value_);
        }
    }

    LPWSTR* address() { return &value_; }

    [[nodiscard]] std::string toUtf8() const { return wideToUtf8(value_); }

  private:
    LPWSTR value_ = nullptr;
};

ExtractedCookieSet cookieListToCookieSet(ICoreWebView2CookieList* cookieList)
{
    ExtractedCookieSet result;
    if (!cookieList)
    {
        return result;
    }

    UINT count = 0;
    if (FAILED(cookieList->get_Count(&count)))
    {
        return result;
    }

    for (UINT index = 0; index < count; ++index)
    {
        ComPtr<ICoreWebView2Cookie> cookie;
        if (FAILED(cookieList->GetValueAtIndex(index, cookie.GetAddressOf())) || !cookie)
        {
            continue;
        }

        CoTaskMemString name;
        CoTaskMemString value;
        if (FAILED(cookie->get_Name(name.address())) ||
            FAILED(cookie->get_Value(value.address())))
        {
            continue;
        }
        BOOL isSession = TRUE;
        cookie->get_IsSession(&isSession);

        const std::string nameText = name.toUtf8();
        const std::string valueText = value.toUtf8();
        if (nameText.empty())
        {
            continue;
        }

        if (!result.header.empty())
        {
            result.header += "; ";
        }
        result.header += nameText;
        result.header += '=';
        result.header += valueText;

        ++result.cookieCount;
        if (isSession != FALSE)
        {
            ++result.sessionCount;
        }
        else
        {
            ++result.persistentCount;
        }
    }
    return result;
}

std::string makeActivateLoginScript(uint64_t requestId)
{
    const Json payload = {{"requestId", requestId}};
    return "(() => {"
           "const payload=" + payload.dump() + ";"
           "window.__domDownToolTianDiTuRequestId=payload.requestId;"
           "if(typeof window.__domDownToolTianDiTuInstallPanel==='function'){"
           "window.__domDownToolTianDiTuInstallPanel(payload.requestId);}})();";
}

std::string makeFetchScript(uint64_t requestId, std::string_view url)
{
    const Json payload = {{"requestId", requestId}, {"url", std::string(url)}};
    return R"javascript(
(() => {
  const payload = )javascript" + payload.dump() + R"javascript(;
  const complete = (result) => {
    const binding = window.domDownToolTianDiTuFetchComplete;
    if (typeof binding === 'function') {
      binding(result).catch(() => {});
    }
  };
  const decodeGetRegionBody = async (response) => {
    const buffer = await response.arrayBuffer();
    const encoded = new Int8Array(buffer);
    const decoded = new Int8Array(Math.floor(encoded.length / 4));
    for (let offset = 0, output = 0; offset + 3 < encoded.length; offset += 4, ++output) {
      let value = 0;
      for (let byteIndex = 0; byteIndex < 4; ++byteIndex) {
        value += (encoded[offset + byteIndex] & 255) << (8 * (3 - byteIndex));
      }
      value >>= 2;
      decoded[output] = value;
    }
    return new TextDecoder('utf-8').decode(decoded);
  };
  fetch(payload.url, {
    method: 'GET',
    credentials: 'include',
    headers: { Accept: 'application/json, text/plain, */*' }
  })
    .then(async (response) => {
      const body = payload.url.includes('/basic/GetRegion')
        ? await decodeGetRegionBody(response)
        : await response.text();
      complete({
        requestId: payload.requestId,
        status: response.status,
        finalUrl: response.url,
        body
      });
    })
    .catch((error) => {
      complete({
        requestId: payload.requestId,
        status: 0,
        finalUrl: location.href,
        error: String(error && error.message ? error.message : error)
      });
    });
})();
)javascript";
}

std::string bootstrapScript()
{
    return R"javascript(
(() => {
  if (window.top !== window) {
    return;
  }

  const invokeNative = (name, payload) => {
    const binding = window[name];
    if (typeof binding === 'function') {
      binding(payload).catch(() => {});
    }
  };

  window.__domDownToolTianDiTuInstallPanel = (requestId) => {
    if (!document.body) {
      return;
    }
    const previousPanel = document.getElementById('domdown-tianditu-login');
    if (previousPanel) {
      previousPanel.remove();
    }

    const panel = document.createElement('section');
    panel.id = 'domdown-tianditu-login';
    panel.style.cssText = [
      'position:fixed', 'right:16px', 'top:16px', 'z-index:2147483647',
      'width:390px', 'box-sizing:border-box', 'padding:14px',
      'background:#17201d', 'color:#f3f6f4', 'border:1px solid #51605a',
      'border-radius:6px', 'box-shadow:0 8px 28px rgba(0,0,0,.35)',
      'font:14px/1.5 "Microsoft YaHei UI","Segoe UI",sans-serif'
    ].join(';');

    const title = document.createElement('div');
    title.textContent = '天地图多时相需要登录';
    title.style.cssText = 'font-weight:600;margin-bottom:6px';

    const description = document.createElement('div');
    description.textContent = '请在当前窗口完成登录，然后点击下方按钮把登录态同步回预览。';
    description.style.cssText = 'color:#c7d2cd;margin-bottom:10px';

    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = '已完成登录，同步登录态';
    button.style.cssText = [
      'height:36px', 'padding:0 13px', 'cursor:pointer', 'color:#fff',
      'background:#26785d', 'border:1px solid #3d9a78', 'border-radius:4px',
      'font:14px "Microsoft YaHei UI","Segoe UI",sans-serif'
    ].join(';');
    button.addEventListener('click', () => {
      button.disabled = true;
      button.textContent = '正在同步登录态...';
      invokeNative('domDownToolTianDiTuLoginComplete', {
        requestId,
        url: location.href
      });
    });

    panel.append(title, description, button);
    document.body.append(panel);
  };

  const onReady = () => {
    invokeNative('domDownToolTianDiTuPageReady', { url: location.href });
    const requestId = Number(window.__domDownToolTianDiTuRequestId || 0);
    if (requestId > 0) {
      window.__domDownToolTianDiTuInstallPanel(requestId);
    }
  };

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', onReady, { once: true });
  } else {
    onReady();
  }
  window.addEventListener('load', onReady, { once: true });
})();
)javascript";
}

LRESULT CALLBACK loginWindowProcedure(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam, UINT_PTR,
                                      DWORD_PTR referenceData)
{
    if (message == WM_CLOSE)
    {
        auto* session = reinterpret_cast<DomDownToolPreview::TianDiTuMultidateWebViewSession*>(
            referenceData);
        ShowWindow(window, SW_HIDE);
        if (session)
        {
            session->cancelCurrent();
        }
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
} // namespace

DomDownToolPreview::TianDiTuMultidateWebViewSession::TianDiTuMultidateWebViewSession()
{
    webViewThread_ = std::thread([this] { runWebView(); });
}

DomDownToolPreview::TianDiTuMultidateWebViewSession::~TianDiTuMultidateWebViewSession()
{
    webview::webview* webView = nullptr;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        if (activeRequest_.has_value() && !activeRequest_->completed)
        {
            activeRequest_->response.error = "天地图多时相登录窗口已关闭。";
            activeRequest_->completed = true;
        }
        if (activeFetchRequest_.has_value() && !activeFetchRequest_->completed)
        {
            activeFetchRequest_->response.error = "天地图多时相登录窗口已关闭。";
            activeFetchRequest_->completed = true;
        }
        webView = static_cast<webview::webview*>(webView_);
    }
    condition_.notify_all();
    if (webView)
    {
        webView->dispatch([webView] { webView->terminate(); });
    }
    if (webViewThread_.joinable())
    {
        webViewThread_.join();
    }
}

DomDownToolPreview::TianDiTuMultidateCookieResponse
DomDownToolPreview::TianDiTuMultidateWebViewSession::requestSessionCookie()
{
    uint64_t requestId = 0;
    {
        std::unique_lock lock(mutex_);
        const bool ready = condition_.wait_for(lock, kPageStartupTimeout,
                                               [this]
                                               {
                                                   return stopping_ ||
                                                       !startupError_.empty() ||
                                                       startupComplete_;
                                               });
        if (!ready)
        {
            return {{}, "天地图多时相登录窗口启动超时。"};
        }
        if (stopping_)
        {
            return {{}, "天地图多时相登录窗口已停止。"};
        }
        if (!startupError_.empty())
        {
            return {{}, startupError_};
        }
        if (activeRequest_.has_value())
        {
            return {{}, "天地图多时相已有登录请求正在进行。"};
        }

        ActiveRequest request;
        request.requestId = ++nextRequestId_;
        requestId = request.requestId;
        activeRequest_ = std::move(request);
    }

    showLoginWindow(requestId);

    std::unique_lock lock(mutex_);
    condition_.wait(lock,
                    [this, requestId]
                    {
                        return stopping_ || !activeRequest_.has_value() ||
                            activeRequest_->requestId != requestId ||
                            activeRequest_->completed;
                    });
    if (!activeRequest_.has_value() || activeRequest_->requestId != requestId)
    {
        return {{}, "天地图多时相登录请求已被替换。"};
    }

    TianDiTuMultidateCookieResponse response = std::move(activeRequest_->response);
    activeRequest_.reset();
    return response;
}

DomDownToolPreview::TianDiTuMultidateFetchResponse
DomDownToolPreview::TianDiTuMultidateWebViewSession::fetchUrl(std::string_view url)
{
    if (url.empty())
    {
        return {0, {}, {}, "天地图多时相请求 URL 为空。"};
    }

    uint64_t requestId = 0;
    {
        std::unique_lock lock(mutex_);
        const bool ready = condition_.wait_for(lock, kPageStartupTimeout,
                                               [this]
                                               {
                                                   return stopping_ ||
                                                       !startupError_.empty() ||
                                                       startupComplete_;
                                               });
        if (!ready)
        {
            return {0, {}, {}, "天地图多时相登录窗口启动超时。"};
        }
        if (stopping_)
        {
            return {0, {}, {}, "天地图多时相登录窗口已停止。"};
        }
        if (!startupError_.empty())
        {
            return {0, {}, {}, startupError_};
        }
        if (activeRequest_.has_value())
        {
            return {0, {}, {}, "天地图多时相登录同步仍在进行。"};
        }
        if (activeFetchRequest_.has_value())
        {
            return {0, {}, {}, "天地图多时相已有刷新请求正在进行。"};
        }

        ActiveFetchRequest request;
        request.requestId = ++nextRequestId_;
        requestId = request.requestId;
        activeFetchRequest_ = std::move(request);
    }

    dispatchFetchRequest(requestId, std::string(url));

    std::unique_lock lock(mutex_);
    const bool completed = condition_.wait_for(
        lock, kFetchTimeout,
        [this, requestId]
        {
            return stopping_ || !activeFetchRequest_.has_value() ||
                activeFetchRequest_->requestId != requestId ||
                activeFetchRequest_->completed;
        });
    if (!completed)
    {
        activeFetchRequest_.reset();
        return {0, {}, {}, "天地图多时相时间线请求超时。"};
    }
    if (!activeFetchRequest_.has_value() || activeFetchRequest_->requestId != requestId)
    {
        return {0, {}, {}, "天地图多时相刷新请求已被替换。"};
    }

    TianDiTuMultidateFetchResponse response = std::move(activeFetchRequest_->response);
    activeFetchRequest_.reset();
    return response;
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::cancelCurrent()
{
    webview::webview* webView = nullptr;
    void* nativeWindow = nullptr;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->completed)
        {
            return;
        }
        activeRequest_->response.error = "天地图多时相登录已取消。";
        activeRequest_->completed = true;
        webView = static_cast<webview::webview*>(webView_);
        nativeWindow = nativeWindow_;
    }
    condition_.notify_all();
    if (webView && nativeWindow)
    {
        webView->dispatch([nativeWindow] { ShowWindow(static_cast<HWND>(nativeWindow), SW_HIDE); });
    }
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::runWebView()
{
    try
    {
        webview::webview window(false, nullptr);
        window.set_title("DomDownTool 天地图多时相登录");
        window.set_size(1200, 780, WEBVIEW_HINT_NONE);
        webview::result<void*> nativeWindowResult = window.window();
        nativeWindowResult.ensure_ok();
        const HWND nativeWindow = static_cast<HWND>(nativeWindowResult.value());
        if (!SetWindowSubclass(nativeWindow, loginWindowProcedure, kLoginWindowSubclassId,
                               reinterpret_cast<DWORD_PTR>(this)))
        {
            throw std::runtime_error("Unable to subclass the TianDiTu login window.");
        }
        ShowWindow(nativeWindow, SW_HIDE);

        window.bind("domDownToolTianDiTuPageReady",
                    [this](const std::string& request) { return handlePageReady(request); });
        window.bind("domDownToolTianDiTuLoginComplete",
                    [this](const std::string& request) { return handleLoginComplete(request); });
        window.bind("domDownToolTianDiTuFetchComplete",
                    [this](const std::string& request) { return handleFetchComplete(request); });
        window.init(bootstrapScript());
        window.navigate(std::string(kTianDiTuMultidateHomeUrl));

        {
            const std::lock_guard lock(mutex_);
            webView_ = &window;
            nativeWindow_ = nativeWindow;
            startupComplete_ = true;
        }
        condition_.notify_all();
        window.run();

        {
            const std::lock_guard lock(mutex_);
            webView_ = nullptr;
            nativeWindow_ = nullptr;
            if (!stopping_)
            {
                startupError_ = "天地图多时相登录窗口意外停止。";
            }
            if (activeRequest_.has_value() && !activeRequest_->completed)
            {
                activeRequest_->response.error = startupError_;
                activeRequest_->completed = true;
            }
            if (activeFetchRequest_.has_value() && !activeFetchRequest_->completed)
            {
                activeFetchRequest_->response.error = startupError_;
                activeFetchRequest_->completed = true;
            }
        }
        condition_.notify_all();
    }
    catch (const webview::exception& exception)
    {
        const std::string details = describeWebViewException(exception);
        writeSearchTrace("TianDiTuWebView", "ui_thread_webview_exception", details);
        {
            const std::lock_guard lock(mutex_);
            startupError_ = "无法启动天地图多时相登录窗口：" + details;
            startupComplete_ = true;
            if (activeRequest_.has_value() && !activeRequest_->completed)
            {
                activeRequest_->response.error = startupError_;
                activeRequest_->completed = true;
            }
            if (activeFetchRequest_.has_value() && !activeFetchRequest_->completed)
            {
                activeFetchRequest_->response.error = startupError_;
                activeFetchRequest_->completed = true;
            }
        }
        condition_.notify_all();
    }
    catch (const std::exception& exception)
    {
        writeSearchTrace("TianDiTuWebView", "ui_thread_exception", exception.what());
        {
            const std::lock_guard lock(mutex_);
            startupError_ = "无法启动天地图多时相登录窗口：" + std::string(exception.what());
            startupComplete_ = true;
            if (activeRequest_.has_value() && !activeRequest_->completed)
            {
                activeRequest_->response.error = startupError_;
                activeRequest_->completed = true;
            }
            if (activeFetchRequest_.has_value() && !activeFetchRequest_->completed)
            {
                activeFetchRequest_->response.error = startupError_;
                activeFetchRequest_->completed = true;
            }
        }
        condition_.notify_all();
    }
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::showLoginWindow(uint64_t requestId)
{
    webview::webview* webView = nullptr;
    void* nativeWindow = nullptr;
    {
        const std::lock_guard lock(mutex_);
        webView = static_cast<webview::webview*>(webView_);
        nativeWindow = nativeWindow_;
    }
    if (!webView || !nativeWindow)
    {
        completeRequest(requestId, {{}, "天地图多时相登录窗口不可用。"});
        return;
    }

    webView->dispatch(
        [webView, nativeWindow, requestId]
        {
            webView->navigate(std::string(kTianDiTuMultidateHomeUrl));
            webView->eval(makeActivateLoginScript(requestId));
            const HWND window = static_cast<HWND>(nativeWindow);
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        });
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::requestCookieExtraction(
    uint64_t requestId)
{
    webview::webview* webView = nullptr;
    {
        const std::lock_guard lock(mutex_);
        webView = static_cast<webview::webview*>(webView_);
    }
    if (!webView)
    {
        completeRequest(requestId, {{}, "天地图多时相登录窗口不可用。"});
        return;
    }

    webView->dispatch([this, requestId] { extractSessionCookie(requestId); });
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::dispatchFetchRequest(
    uint64_t requestId, std::string url)
{
    webview::webview* webView = nullptr;
    {
        const std::lock_guard lock(mutex_);
        webView = static_cast<webview::webview*>(webView_);
    }
    if (!webView)
    {
        completeFetchRequest(requestId, {0, {}, {}, "天地图多时相登录窗口不可用。"});
        return;
    }

    writeSearchTrace("TianDiTuWebView", "fetch_dispatch_start",
                     fmt::format("id={}", requestId));
    const webview::noresult dispatchResult = webView->dispatch(
        [this, webView, requestId, url = std::move(url)]
        {
            const std::string script = makeFetchScript(requestId, url);
            const webview::noresult evalResult = webView->eval(script);
            if (evalResult.has_error())
            {
                writeSearchTrace("TianDiTuWebView", "fetch_eval_failed",
                                 fmt::format("id={} code={}", requestId,
                                             static_cast<int>(
                                                 evalResult.error().code())));
                completeFetchRequest(
                    requestId,
                    {0, {}, {}, "无法发起天地图多时相 WebView 请求。"});
            }
        });
    if (dispatchResult.has_error())
    {
        writeSearchTrace("TianDiTuWebView", "fetch_dispatch_failed",
                         fmt::format("id={} code={}", requestId,
                                     static_cast<int>(dispatchResult.error().code())));
        completeFetchRequest(requestId,
                             {0, {}, {}, "无法调度天地图多时相 WebView 请求。"});
        return;
    }

    writeSearchTrace("TianDiTuWebView", "fetch_dispatch_queued",
                     fmt::format("id={}", requestId));
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::extractSessionCookie(
    uint64_t requestId)
{
    webview::webview* webView = nullptr;
    {
        const std::lock_guard lock(mutex_);
        webView = static_cast<webview::webview*>(webView_);
    }
    if (!webView)
    {
        completeRequest(requestId, {{}, "天地图多时相登录窗口不可用。"});
        return;
    }

    std::string cookieManagerError;
    ComPtr<ICoreWebView2CookieManager> cookieManager =
        cookieManagerFromWebView(*webView, cookieManagerError);
    if (!cookieManager)
    {
        completeRequest(requestId, {{}, cookieManagerError});
        return;
    }

    const auto handler = Microsoft::WRL::Callback<ICoreWebView2GetCookiesCompletedHandler>(
        [this, requestId](HRESULT result, ICoreWebView2CookieList* cookieList) -> HRESULT
        {
            if (FAILED(result))
            {
                completeRequest(requestId, {{}, "读取天地图多时相 Cookie 失败。"});
                return S_OK;
            }

            const ExtractedCookieSet cookieSet = cookieListToCookieSet(cookieList);
            writeSearchTrace(
                "TianDiTuWebView", "cookie_extract_complete",
                fmt::format("count={} session={} persistent={} header_bytes={}",
                            cookieSet.cookieCount, cookieSet.sessionCount,
                            cookieSet.persistentCount, cookieSet.header.size()));
            if (cookieSet.header.empty())
            {
                completeRequest(
                    requestId,
                    {{}, "未读取到天地图 Cookie，请确认登录完成后再同步。"});
                return S_OK;
            }

            completeRequest(requestId, {cookieSet.header, {}});
            return S_OK;
        });

    const std::wstring cookieUrl(kTianDiTuCookieUrl.data(), kTianDiTuCookieUrl.size());
    if (FAILED(cookieManager->GetCookies(cookieUrl.c_str(), handler.Get())))
    {
        completeRequest(requestId, {{}, "无法发起天地图 Cookie 读取请求。"});
    }
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::completeRequest(
    uint64_t requestId, TianDiTuMultidateCookieResponse response)
{
    void* nativeWindow = nullptr;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->requestId != requestId ||
            activeRequest_->completed)
        {
            return;
        }
        activeRequest_->response = std::move(response);
        activeRequest_->completed = true;
        nativeWindow = nativeWindow_;
    }
    if (nativeWindow)
    {
        ShowWindow(static_cast<HWND>(nativeWindow), SW_HIDE);
    }
    condition_.notify_all();
}

void DomDownToolPreview::TianDiTuMultidateWebViewSession::completeFetchRequest(
    uint64_t requestId, TianDiTuMultidateFetchResponse response)
{
    {
        const std::lock_guard lock(mutex_);
        if (!activeFetchRequest_.has_value() ||
            activeFetchRequest_->requestId != requestId ||
            activeFetchRequest_->completed)
        {
            return;
        }
        activeFetchRequest_->response = std::move(response);
        activeFetchRequest_->completed = true;
    }
    condition_.notify_all();
}

std::string DomDownToolPreview::TianDiTuMultidateWebViewSession::handlePageReady(
    const std::string& requestJson)
{
    const std::optional<Json> payload = firstBindingArgument(requestJson);
    if (!payload.has_value())
    {
        return makeBindingResult(false, "Invalid page-ready payload.");
    }

    const std::string pageUrl = GetJsonString(*payload, "url");
    writeSearchTrace("TianDiTuWebView", "page_ready", "url=" + pageUrl);

    uint64_t requestId = 0;
    webview::webview* webView = nullptr;
    {
        const std::lock_guard lock(mutex_);
        if (activeRequest_.has_value() && !activeRequest_->completed)
        {
            requestId = activeRequest_->requestId;
            webView = static_cast<webview::webview*>(webView_);
        }
    }
    if (requestId != 0 && webView)
    {
        webView->eval(makeActivateLoginScript(requestId));
    }
    return makeBindingResult(true);
}

std::string DomDownToolPreview::TianDiTuMultidateWebViewSession::handleLoginComplete(
    const std::string& requestJson)
{
    const std::optional<Json> payload = firstBindingArgument(requestJson);
    if (!payload.has_value())
    {
        return makeBindingResult(false, "Invalid login-complete payload.");
    }

    const uint64_t requestId = static_cast<uint64_t>(
        std::max<int64_t>(0, GetJsonInteger(*payload, "requestId")));
    const std::string pageUrl = GetJsonString(*payload, "url");
    writeSearchTrace("TianDiTuWebView", "login_complete_clicked", "url=" + pageUrl);
    requestCookieExtraction(requestId);
    return makeBindingResult(true);
}

std::string DomDownToolPreview::TianDiTuMultidateWebViewSession::handleFetchComplete(
    const std::string& requestJson)
{
    const std::optional<Json> payload = firstBindingArgument(requestJson);
    if (!payload.has_value())
    {
        writeSearchTrace("TianDiTuWebView", "fetch_invalid_payload");
        return makeBindingResult(false, "Invalid fetch-complete payload.");
    }

    const uint64_t requestId = static_cast<uint64_t>(
        std::max<int64_t>(0, GetJsonInteger(*payload, "requestId")));
    TianDiTuMultidateFetchResponse response;
    response.statusCode = static_cast<int>(
        std::max<int64_t>(0, GetJsonInteger(*payload, "status")));
    response.body = GetJsonString(*payload, "body");
    response.finalUrl = GetJsonString(*payload, "finalUrl");
    response.error = GetJsonString(*payload, "error");

    writeSearchTrace(
        "TianDiTuWebView", "fetch_complete",
        fmt::format("id={} status={} body_bytes={} final_url={} error={}",
                    requestId, response.statusCode, response.body.size(),
                    response.finalUrl, response.error));
    completeFetchRequest(requestId, std::move(response));
    return makeBindingResult(true);
}
