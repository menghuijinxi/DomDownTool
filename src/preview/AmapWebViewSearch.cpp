#include "preview/AmapWebViewSearch.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>
#include <commctrl.h>

#include "fmt/format.h"
#include "nlohmann/json.hpp"
#include "webview/webview.h"

#include "AmapSearchPageUrl.h"
#include "preview/SearchTrace.h"

namespace
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view kAmapHomeUrl = "https://www.amap.com/";
constexpr std::string_view kAmapTipsUrlPrefix =
    "https://www.amap.com/service/poiTipsSearchlite";
constexpr auto kPageStartupTimeout = 20s;
constexpr auto kRequestTimeout = 30s;
constexpr UINT_PTR kVerificationWindowSubclassId = 1;

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
    if (iterator == object.end() || !iterator->is_number_integer())
    {
        return 0;
    }
    if (iterator->is_number_unsigned())
    {
        const uint64_t value = iterator->get<uint64_t>();
        return value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? static_cast<int64_t>(value)
            : 0;
    }
    return iterator->get<int64_t>();
}

bool GetJsonBool(const Json& object, const char* fieldName)
{
    if (!object.is_object())
    {
        return false;
    }
    const auto iterator = object.find(fieldName);
    return iterator != object.end() && iterator->is_boolean()
        ? iterator->get<bool>()
        : false;
}

LRESULT CALLBACK verificationWindowProcedure(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR,
                                             DWORD_PTR referenceData)
{
    if (message == WM_CLOSE)
    {
        auto* search = reinterpret_cast<DomDownToolPreview::AmapWebViewSearch*>(
            referenceData);
        ShowWindow(window, SW_HIDE);
        if (search)
        {
            search->cancelCurrent();
        }
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

std::string describeWebViewException(const webview::exception& exception)
{
    const webview::error_info& error = exception.error();
    return fmt::format("code={} message={} what={}", static_cast<int>(error.code()),
                       error.message(), exception.what());
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

std::optional<Json> firstBindingArgument(const std::string& requestJson)
{
    const Json root = Json::parse(requestJson, nullptr, false);
    if (root.is_discarded() || !root.is_array())
    {
        return std::nullopt;
    }

    if (root.empty() || !root.front().is_object())
    {
        return std::nullopt;
    }
    return Json(root.front());
}

std::string makeFetchScript(uint64_t requestId, const std::string& url)
{
    const std::string_view poiId = DomDownToolPreview::isPlaceDetailUrl(url)
        ? std::string_view(url).substr(DomDownToolPreview::kPlaceDetailPrefix.size())
        : std::string_view{};
    const Json request = {{"requestId", requestId}, {"url", url}, {"poiId", poiId}};
    return "(() => {"
           "const request=" + request.dump() + ";"
           "if(typeof window.__domDownToolAmapFetch==='function'){"
           "window.__domDownToolAmapFetch(request);return;}"
           "const binding=window.domDownToolAmapTransportResult;"
           "if(typeof binding==='function'){binding({requestId:request.requestId,"
           "statusCode:0,responseUrl:request.url,contentType:'',body:'',"
           "error:'Amap WebView bootstrap script is unavailable.',"
           "riskControl:false}).catch(()=>{});}})();";
}

std::string makeCollectScript(uint64_t requestId, const std::string& requestUrl)
{
    const Json request = {{"requestId", requestId}, {"url", requestUrl}};
    return "window.__domDownToolCollectAmapSearchPage(" +
        request.dump() + ");";
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

  const capturedJson = [];
  const rememberJson = (text) => {
    if (typeof text !== 'string' || text.length === 0 || text.length > 2097152) {
      return;
    }
    const firstCharacter = text.trimStart()[0];
    if (firstCharacter !== '{' && firstCharacter !== '[') {
      return;
    }
    try {
      capturedJson.push(JSON.parse(text));
      if (capturedJson.length > 32) {
        capturedJson.shift();
      }
      window.dispatchEvent(new Event('domdown-amap-json-captured'));
    } catch (_) {
    }
  };
  window.__domDownToolAmapCapturedJson = capturedJson;

  const originalFetch = window.fetch;
  if (typeof originalFetch === 'function') {
    window.fetch = async (...args) => {
      const response = await originalFetch.apply(window, args);
      response.clone().text().then(rememberJson).catch(() => {});
      return response;
    };
  }

  const originalXhrSend = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.send = function(...args) {
    this.addEventListener('load', () => {
      try {
        rememberJson(this.responseText);
      } catch (_) {
      }
    }, { once: true });
    return originalXhrSend.apply(this, args);
  };

  const installVerificationPanel = () => {
    if (!document.body || document.getElementById('domdown-amap-verification')) {
      return;
    }

    const panel = document.createElement('section');
    panel.id = 'domdown-amap-verification';
    panel.style.cssText = [
      'position:fixed', 'right:16px', 'top:16px', 'z-index:2147483647',
      'width:380px', 'box-sizing:border-box', 'padding:14px',
      'background:#17201d', 'color:#f3f6f4', 'border:1px solid #51605a',
      'border-radius:6px', 'box-shadow:0 8px 28px rgba(0,0,0,.35)',
      'font:14px/1.5 "Microsoft YaHei UI","Segoe UI",sans-serif'
    ].join(';');

    const title = document.createElement('div');
    title.textContent = '\u9ad8\u5fb7\u641c\u7d22\u9700\u8981\u4eba\u5de5\u9a8c\u8bc1';
    title.style.cssText = 'font-weight:600;margin-bottom:6px';

    const description = document.createElement('div');
    description.textContent =
      '\u8bf7\u5728\u9875\u9762\u4e2d\u5b8c\u6210\u9a8c\u8bc1\uff0c'
      + '\u7136\u540e\u70b9\u51fb\u4e0b\u65b9\u6309\u94ae\u91cd\u8bd5\u539f\u641c\u7d22\u3002';
    description.style.cssText = 'color:#c7d2cd;margin-bottom:10px';

    const retryButton = document.createElement('button');
    retryButton.type = 'button';
    retryButton.textContent =
      '\u5df2\u5b8c\u6210\u9a8c\u8bc1\uff0c\u91cd\u8bd5\u641c\u7d22';
    retryButton.style.cssText = [
      'height:36px', 'padding:0 13px', 'cursor:pointer', 'color:#fff',
      'background:#26785d', 'border:1px solid #3d9a78', 'border-radius:4px',
      'font:14px "Microsoft YaHei UI","Segoe UI",sans-serif'
    ].join(';');
    retryButton.addEventListener('click', () => {
      retryButton.disabled = true;
      retryButton.textContent =
        '\u6b63\u5728\u91cd\u8bd5\u641c\u7d22...';
      invokeNative('domDownToolAmapVerificationComplete', { url: location.href });
    });

    panel.append(title, description, retryButton);
    document.body.append(panel);
  };

  const detectRiskPage = () => {
    const signature = `${location.href} ${document.title}`.toLowerCase();
    const urlRisk = signature.includes('punish') || signature.includes('captcha') ||
      signature.includes('x5sec') || signature.includes('baxia');
    const riskElements = Array.from(document.querySelectorAll(
      'iframe[src*="captcha"], iframe[src*="punish"], [class*="captcha"]'));
    const elementRisk = riskElements.some((element) => {
      const style = getComputedStyle(element);
      const bounds = element.getBoundingClientRect();
      return style.display !== 'none' && style.visibility !== 'hidden' &&
        Number(style.opacity || 1) > 0 && bounds.width > 0 && bounds.height > 0;
    });
    if (urlRisk || elementRisk) {
      invokeNative('domDownToolAmapRiskDetected', { url: location.href });
    }
  };

  const detailPois = (html, poiId) => {
    // 详情页的 JSON-LD 由高德按 POI ID 生成，与当前地图视野无关。
    const page = new DOMParser().parseFromString(html, 'text/html');
    const element = page.getElementById('place-schema-' + poiId);
    if (!element) {
      return [];
    }
    const place = JSON.parse(element.textContent);
    const lon = place?.geo?.longitude;
    const lat = place?.geo?.latitude;
    const validCoordinate = (value, limit) =>
      (typeof value === 'number' || (typeof value === 'string' && value.trim() !== '')) &&
      Number.isFinite(Number(value)) && Math.abs(Number(value)) <= limit;
    if (!place?.name || !validCoordinate(lon, 180) || !validCoordinate(lat, 90)) {
      return [];
    }
    return [{ id: poiId, name: place.name, address: place.address?.streetAddress || '',
              location: [Number(lon), Number(lat)].join(',') }];
  };

  window.__domDownToolAmapFetch = async (request) => {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 15000);
    try {
      const response = await fetch(request.url, {
        method: 'GET',
        credentials: 'include',
        signal: controller.signal,
        headers: { 'Accept': request.poiId ? 'text/html' : 'application/json, text/plain, */*' }
      });
      const body = await response.text();
      const contentType = response.headers.get('content-type') || '';
      const finalUrl = new URL(response.url);
      if (request.poiId && finalUrl.origin + finalUrl.pathname === request.url) {
        const pois = response.ok ? detailPois(body, request.poiId) : [];
        // 已移除的详情页按空结果处理，允许沿用名称查找。
        const missing = response.status === 404 || response.status === 410;
        invokeNative('domDownToolAmapTransportResult', {
          requestId: request.requestId, statusCode: missing ? 200 : response.status,
          responseUrl: response.url,
          contentType: 'application/json', body: JSON.stringify({status: '1', pois}),
          hasResults: pois.length > 0, riskControl: false, resultSource: 'place-detail'
        });
        return;
      }
      const riskControl =
        !!request.poiId || !contentType.toLowerCase().includes('application/json');
      invokeNative('domDownToolAmapTransportResult', {
        requestId: request.requestId,
        statusCode: response.status,
        responseUrl: response.url,
        contentType,
        body,
        riskControl
      });
    } catch (error) {
      const message = error && error.message ? error.message : String(error);
      invokeNative('domDownToolAmapTransportResult', {
        requestId: request.requestId,
        statusCode: 0,
        responseUrl: request.url,
        contentType: '',
        body: '',
        error: message,
        riskControl: false
      });
    } finally {
      clearTimeout(timeout);
    }
  };

  let stopCollection;
  window.__domDownToolCollectAmapSearchPage = (request) => {
    // 人工验证后可能在同一页重试，旧采集的超时不能结束新一轮请求。
    stopCollection?.();

    const requestId = request.requestId;
    const searchQuery = new URL(request.url).searchParams.get('query')?.trim() || '';
    let completed = false;
    const finish = (pois, resultSource) => {
      if (completed) {
        return;
      }
      stopCollection();
      invokeNative('domDownToolAmapTransportResult', {
        requestId,
        statusCode: 200,
        responseUrl: location.href,
        contentType: 'application/json',
        body: JSON.stringify({ status: '1', info: 'OK', pois }),
        riskControl: false,
        hasResults: pois.length > 0,
        resultSource
      });
    };

    const toCoordinate = (longitude, latitude) => {
      const x = Number(longitude);
      const y = Number(latitude);
      if (!Number.isFinite(x) || !Number.isFinite(y) || x < 70 || x > 140 ||
          y < 0 || y > 60) {
        return null;
      }
      return [x, y];
    };

    const coordinateFromValue = (value) => {
      if (Array.isArray(value) && value.length >= 2) {
        return toCoordinate(value[0], value[1]);
      }
      if (typeof value === 'string') {
        const parts = value.split(',');
        return parts.length >= 2 ? toCoordinate(parts[0], parts[1]) : null;
      }
      if (!value || typeof value !== 'object') {
        return null;
      }
      return toCoordinate(value.x, value.y) || toCoordinate(value.lng, value.lat) ||
        toCoordinate(value.lon, value.lat) ||
        toCoordinate(value.longitude, value.latitude);
    };

    const coordinateFromObject = (value) => {
      return coordinateFromValue(value.location) || coordinateFromValue(value.lnglat) ||
        coordinateFromValue(value.point) || coordinateFromValue(value.coordinate) ||
        coordinateFromValue(value.center) || coordinateFromValue(value);
    };

    const matchesSearchQuery = (name) => {
      if (typeof name !== 'string') {
        return false;
      }
      const normalizedName = name.trim();
      return normalizedName.length > 0 &&
        (searchQuery === normalizedName || searchQuery.startsWith(`${normalizedName} `));
    };

    const collectCapturedPois = () => {
      const pois = [];
      const coordinates = new Set();
      const objects = new Set();
      let visited = 0;
      const visit = (value, depth) => {
        if (pois.length >= 8 || depth > 12 || ++visited > 50000 || value === null ||
            typeof value !== 'object' || objects.has(value)) {
          return;
        }
        objects.add(value);
        if (Array.isArray(value)) {
          value.forEach((item) => visit(item, depth + 1));
          return;
        }

        const coordinate = coordinateFromObject(value);
        const name = value.name || value.poiName || value.title || value.shortName;
        if (coordinate && matchesSearchQuery(name)) {
          const coordinateText = `${coordinate[0]},${coordinate[1]}`;
          if (!coordinates.has(coordinateText)) {
            coordinates.add(coordinateText);
            pois.push({
              id: String(value.id || value.poiid || value.poiId || value.uid || ''),
              name: name.trim(),
              address: String(value.address || value.addr || value.district || ''),
              location: coordinateText,
              type: String(value.type || value.typecode || value.typeCode || '')
            });
          }
        }
        Object.values(value).forEach((item) => visit(item, depth + 1));
      };
      capturedJson.forEach((value) => visit(value, 0));
      return pois;
    };

    const collectDomPois = () => {
      const rows = Array.from(document.querySelectorAll([
        '.poibox[data-poiinfo-x][data-poiinfo-y]',
        '[data-x][data-y]',
        '[data-lng][data-lat]',
        '[data-longitude][data-latitude]'
      ].join(','))).slice(0, 500);
      return rows.map((row) => {
        const coordinate =
          toCoordinate(row.getAttribute('data-poiinfo-x'),
                       row.getAttribute('data-poiinfo-y')) ||
          toCoordinate(row.getAttribute('data-x'), row.getAttribute('data-y')) ||
          toCoordinate(row.getAttribute('data-lng'), row.getAttribute('data-lat')) ||
          toCoordinate(row.getAttribute('data-longitude'),
                       row.getAttribute('data-latitude'));
        const nameElement = row.querySelector('[class*="name"], [class*="title"]');
        const addressElement = row.querySelector('[class*="address"], [class*="addr"]');
        return {
          id: row.id || row.getAttribute('data-poiid') || '',
          name: row.getAttribute('data-poiinfo-name') ||
            row.getAttribute('data-name') || nameElement?.textContent?.trim() || '',
          address: row.getAttribute('data-poiinfo-address') ||
            row.getAttribute('data-address') || addressElement?.textContent?.trim() || '',
          location: coordinate ? `${coordinate[0]},${coordinate[1]}` : '',
          type: ''
        };
      }).filter((poi) => poi.location && matchesSearchQuery(poi.name));
    };

    const collect = () => {
      detectRiskPage();
      const domPois = collectDomPois();
      if (domPois.length > 0) {
        finish(domPois.slice(0, 8), 'dom');
        return;
      }
      const capturedPois = collectCapturedPois();
      if (capturedPois.length > 0) {
        finish(capturedPois, 'network');
      }
    };

    const observer = new MutationObserver(collect);
    observer.observe(document.documentElement, {
      childList: true,
      subtree: true
    });
    window.addEventListener('domdown-amap-json-captured', collect);
    const timer = setInterval(collect, 250);
    const timeout = setTimeout(() => finish([], 'timeout'), 10000);
    stopCollection = () => {
      completed = true;
      observer.disconnect();
      clearInterval(timer);
      clearTimeout(timeout);
      window.removeEventListener('domdown-amap-json-captured', collect);
      stopCollection = undefined;
    };
    collect();
  };

  const onPageReady = () => {
    installVerificationPanel();
    invokeNative('domDownToolAmapPageReady', { url: location.href });
    detectRiskPage();
  };

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', onPageReady, { once: true });
  } else {
    onPageReady();
  }
})();
)javascript";
}

bool isFetchUrl(std::string_view url)
{
    return url.starts_with(kAmapTipsUrlPrefix);
}
} // namespace

DomDownToolPreview::AmapWebViewSearch::AmapWebViewSearch()
{
    writeSearchTrace("WebView", "constructor_start");
    webViewThread_ = std::thread([this] { runWebView(); });
}

DomDownToolPreview::AmapWebViewSearch::~AmapWebViewSearch()
{
    writeSearchTrace("WebView", "destructor_start");
    webview::webview* webView = nullptr;
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        if (activeRequest_.has_value() && !activeRequest_->completed)
        {
            activeRequest_->response.error = "Amap WebView search stopped.";
            activeRequest_->completed = true;
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
    writeSearchTrace("WebView", "destructor_complete");
}

DomDownToolPreview::PlaceSearchHttpResponse DomDownToolPreview::AmapWebViewSearch::get(
    const std::string& url)
{
    RequestKind kind;
    if (isFetchUrl(url) || isPlaceDetailUrl(url))
    {
        kind = RequestKind::Fetch;
    }
    else if (isSearchPageUrl(url))
    {
        kind = RequestKind::SearchPage;
    }
    else
    {
        writeSearchTrace("WebView", "request_rejected", "reason=unsupported_url");
        return {0, {}, "Unsupported Amap WebView search URL."};
    }
    const std::string_view kindName = kind == RequestKind::Fetch ? "fetch" : "search_page";

    writeSearchTrace("WebView", "request_waiting_for_page",
                     fmt::format("kind={}", kindName));

    uint64_t requestId = 0;
    {
        std::unique_lock lock(mutex_);
        const bool ready = condition_.wait_for(lock, kPageStartupTimeout, [this]
                                               {
                                                   return stopping_ || !startupError_.empty() ||
                                                       (startupComplete_ && pageReady_);
                                               });
        if (!ready)
        {
            writeSearchTrace("WebView", "request_failed", "reason=startup_timeout");
            return {0, {}, "Amap WebView page startup timed out."};
        }
        if (stopping_)
        {
            writeSearchTrace("WebView", "request_failed", "reason=stopping");
            return {0, {}, "Amap WebView search stopped."};
        }
        if (!startupError_.empty())
        {
            writeSearchTrace("WebView", "request_failed",
                             "reason=startup_error error=" + startupError_);
            return {0, {}, startupError_};
        }
        if (activeRequest_.has_value())
        {
            writeSearchTrace("WebView", "request_failed", "reason=already_active");
            return {0, {}, "Amap WebView already has an active request."};
        }

        ActiveRequest request;
        request.requestId = ++nextRequestId_;
        request.kind = kind;
        request.url = url;
        requestId = request.requestId;
        activeRequest_ = std::move(request);
    }

    writeSearchTrace("WebView", "request_created",
                     fmt::format("id={} kind={}", requestId, kindName));

    dispatchActiveRequest(requestId);

    std::unique_lock lock(mutex_);
    const auto requestFinished = [this, requestId]
    {
        return stopping_ || !activeRequest_.has_value() ||
            activeRequest_->requestId != requestId || activeRequest_->completed;
    };
    while (!requestFinished())
    {
        if (activeRequest_->waitingForVerification)
        {
            writeSearchTrace("WebView", "request_waiting_for_verification",
                             fmt::format("id={}", requestId));
            condition_.wait(lock, [this, &requestFinished]
                            {
                                return requestFinished() ||
                                    !activeRequest_->waitingForVerification;
                            });
            continue;
        }

        const bool stateChanged = condition_.wait_for(
            lock, kRequestTimeout,
            [this, &requestFinished]
            {
                return requestFinished() || activeRequest_->waitingForVerification;
            });
        if (!stateChanged)
        {
            writeSearchTrace("WebView", "request_timeout",
                             fmt::format("id={} seconds={}", requestId,
                                         kRequestTimeout.count()));
            activeRequest_->response.error =
                "高德搜索等待 WebView2 页面回调超时，请重试。";
            activeRequest_->completed = true;
        }
    }
    if (!activeRequest_.has_value() || activeRequest_->requestId != requestId)
    {
        return {0, {}, "Amap WebView request was replaced."};
    }

    PlaceSearchHttpResponse response = std::move(activeRequest_->response);
    activeRequest_.reset();
    writeSearchTrace(
        "WebView", "request_complete",
        fmt::format("id={} status={} body_bytes={} error={}", requestId,
                    response.statusCode, response.body.size(), response.error));
    return response;
}

void DomDownToolPreview::AmapWebViewSearch::cancelCurrent()
{
    webview::webview* webView = nullptr;
    void* nativeWindow = nullptr;
    bool returnHome = false;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->completed)
        {
            writeSearchTrace("WebView", "cancel_ignored", "reason=no_active_request");
            return;
        }
        returnHome = activeRequest_->waitingForVerification;
        activeRequest_->response.error = "Amap WebView request was cancelled.";
        activeRequest_->completed = true;
        webView = static_cast<webview::webview*>(webView_);
        nativeWindow = nativeWindow_;
        if (returnHome)
        {
            pageReady_ = false;
            retryOnPageReady_ = false;
        }
    }
    writeSearchTrace("WebView", "request_cancelled",
                     fmt::format("return_home={}", returnHome));
    condition_.notify_all();
    if (returnHome && webView)
    {
        webView->dispatch(
            [webView, nativeWindow]
            {
                ShowWindow(static_cast<HWND>(nativeWindow), SW_HIDE);
                webView->navigate(std::string(kAmapHomeUrl));
            });
    }
}

void DomDownToolPreview::AmapWebViewSearch::runWebView()
{
    writeSearchTrace("WebView", "ui_thread_start");
    try
    {
        webview::webview window(false, nullptr);
        writeSearchTrace("WebView", "controller_created");
        window.set_title("DomDownTool Amap verification");
        window.set_size(1200, 780, WEBVIEW_HINT_NONE);
        webview::result<void*> nativeWindowResult = window.window();
        nativeWindowResult.ensure_ok();
        const HWND nativeWindow = static_cast<HWND>(nativeWindowResult.value());
        if (!SetWindowSubclass(nativeWindow, verificationWindowProcedure,
                               kVerificationWindowSubclassId,
                               reinterpret_cast<DWORD_PTR>(this)))
        {
            throw std::runtime_error("Unable to subclass the Amap verification window.");
        }
        ShowWindow(nativeWindow, SW_HIDE);
        writeSearchTrace("WebView", "top_level_window_hidden");

        window.bind("domDownToolAmapPageReady",
                    [this](const std::string& request) { return handlePageReady(request); });
        window.bind("domDownToolAmapTransportResult",
                    [this](const std::string& request)
                    { return handleTransportResult(request); });
        window.bind("domDownToolAmapRiskDetected",
                    [this](const std::string& request) { return handleRiskDetected(request); });
        window.bind("domDownToolAmapVerificationComplete",
                    [this](const std::string& request)
                    { return handleVerificationComplete(request); });
        window.init(bootstrapScript());
        writeSearchTrace("WebView", "bindings_and_bootstrap_installed");
        window.navigate(std::string(kAmapHomeUrl));
        writeSearchTrace("WebView", "home_navigation_queued");

        {
            const std::lock_guard lock(mutex_);
            webView_ = &window;
            nativeWindow_ = nativeWindow;
            startupComplete_ = true;
        }
        condition_.notify_all();
        writeSearchTrace("WebView", "event_loop_start");
        window.run();
        writeSearchTrace("WebView", "event_loop_stopped");

        {
            const std::lock_guard lock(mutex_);
            webView_ = nullptr;
            nativeWindow_ = nullptr;
            if (!stopping_)
            {
                startupError_ = "Amap WebView window stopped unexpectedly.";
            }
            if (activeRequest_.has_value() && !activeRequest_->completed)
            {
                activeRequest_->response.error = startupError_;
                activeRequest_->completed = true;
            }
        }
        condition_.notify_all();
    }
    catch (const webview::exception& exception)
    {
        const std::string details = describeWebViewException(exception);
        writeSearchTrace("WebView", "ui_thread_webview_exception", details);
        {
            const std::lock_guard lock(mutex_);
            startupError_ = "Unable to start Amap WebView: " + details;
            startupComplete_ = true;
            if (activeRequest_.has_value() && !activeRequest_->completed)
            {
                activeRequest_->response.error = startupError_;
                activeRequest_->completed = true;
            }
        }
        condition_.notify_all();
    }
    catch (const std::exception& exception)
    {
        writeSearchTrace("WebView", "ui_thread_exception", exception.what());
        {
            const std::lock_guard lock(mutex_);
            startupError_ = "Unable to start Amap WebView: " + std::string(exception.what());
            startupComplete_ = true;
            if (activeRequest_.has_value() && !activeRequest_->completed)
            {
                activeRequest_->response.error = startupError_;
                activeRequest_->completed = true;
            }
        }
        condition_.notify_all();
    }
}

void DomDownToolPreview::AmapWebViewSearch::dispatchActiveRequest(uint64_t requestId)
{
    webview::webview* webView = nullptr;
    ActiveRequest request;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->requestId != requestId ||
            activeRequest_->completed || !webView_)
        {
            writeSearchTrace("WebView", "dispatch_skipped",
                             fmt::format("id={}", requestId));
            return;
        }
        request = *activeRequest_;
        webView = static_cast<webview::webview*>(webView_);
        if (request.kind == RequestKind::SearchPage)
        {
            pageReady_ = false;
        }
    }

    if (request.kind == RequestKind::Fetch)
    {
        writeSearchTrace("WebView", "fetch_dispatch_start",
                         fmt::format("id={}", requestId));
        const std::string script = makeFetchScript(request.requestId, request.url);
        const webview::noresult dispatchResult = webView->dispatch(
            [this, webView, script, requestId]
            {
                const webview::noresult evalResult = webView->eval(script);
                if (evalResult.has_error())
                {
                    writeSearchTrace("WebView", "fetch_eval_failed",
                                     fmt::format("id={} code={}", requestId,
                                                 static_cast<int>(
                                                     evalResult.error().code())));
                    completeWithError(requestId,
                                      "Amap WebView failed to evaluate the search script.");
                }
            });
        if (dispatchResult.has_error())
        {
            writeSearchTrace("WebView", "fetch_dispatch_failed",
                             fmt::format("id={} code={}", requestId,
                                         static_cast<int>(dispatchResult.error().code())));
            completeWithError(requestId, "Amap WebView failed to dispatch the search script.");
        }
        else
        {
            writeSearchTrace("WebView", "fetch_dispatch_queued",
                             fmt::format("id={}", requestId));
        }
        return;
    }
    writeSearchTrace("WebView", "navigation_dispatch_start",
                     fmt::format("id={}", requestId));
    const webview::noresult dispatchResult = webView->dispatch(
        [this, webView, url = request.url, requestId]
        {
            const webview::noresult navigationResult = webView->navigate(url);
            if (navigationResult.has_error())
            {
                writeSearchTrace("WebView", "navigation_failed",
                                 fmt::format("id={} code={}", requestId,
                                             static_cast<int>(
                                                 navigationResult.error().code())));
                completeWithError(requestId, "Amap WebView failed to navigate to search.");
            }
        });
    if (dispatchResult.has_error())
    {
        writeSearchTrace("WebView", "navigation_dispatch_failed",
                         fmt::format("id={} code={}", requestId,
                                     static_cast<int>(dispatchResult.error().code())));
        completeWithError(requestId, "Amap WebView failed to dispatch search navigation.");
    }
    else
    {
        writeSearchTrace("WebView", "navigation_dispatch_queued",
                         fmt::format("id={}", requestId));
    }
}

void DomDownToolPreview::AmapWebViewSearch::completeWithError(uint64_t requestId,
                                                              std::string error)
{
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->requestId != requestId ||
            activeRequest_->completed)
        {
            return;
        }
        activeRequest_->response.error = std::move(error);
        activeRequest_->completed = true;
    }
    writeSearchTrace("WebView", "request_completed_with_error",
                     fmt::format("id={}", requestId));
    condition_.notify_all();
}

void DomDownToolPreview::AmapWebViewSearch::showVerification(const std::string& url,
                                                             bool navigate)
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
        writeSearchTrace("WebView", "verification_show_failed",
                         "reason=missing_window_or_webview");
        return;
    }

    writeSearchTrace("WebView", "verification_show",
                     fmt::format("navigate={}", navigate));

    webView->dispatch(
        [webView, nativeWindow, url, navigate]
        {
            if (navigate && !url.empty())
            {
                webView->navigate(url);
            }
            const HWND window = static_cast<HWND>(nativeWindow);
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        });
}

void DomDownToolPreview::AmapWebViewSearch::retryAfterVerification()
{
    webview::webview* webView = nullptr;
    void* nativeWindow = nullptr;
    uint64_t requestId = 0;
    RequestKind requestKind = RequestKind::Fetch;
    std::string requestUrl;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->completed || !webView_)
        {
            writeSearchTrace("WebView", "verification_retry_ignored");
            return;
        }
        requestId = activeRequest_->requestId;
        requestKind = activeRequest_->kind;
        requestUrl = activeRequest_->url;
        activeRequest_->verificationConfirmed = true;
        activeRequest_->waitingForVerification = false;
        retryOnPageReady_ = requestKind == RequestKind::Fetch;
        pageReady_ = requestKind == RequestKind::SearchPage;
        webView = static_cast<webview::webview*>(webView_);
        nativeWindow = nativeWindow_;
    }
    writeSearchTrace(
        "WebView", requestKind == RequestKind::SearchPage
            ? "verification_continue_current_search"
            : "verification_retry_home_navigation",
        fmt::format("id={}", requestId));
    condition_.notify_all();

    webView->dispatch(
        [webView, nativeWindow, requestId, requestKind, requestUrl]
        {
            ShowWindow(static_cast<HWND>(nativeWindow), SW_HIDE);
            if (requestKind == RequestKind::SearchPage)
            {
                webView->eval(makeCollectScript(requestId, requestUrl));
            }
            else
            {
                webView->navigate(std::string(kAmapHomeUrl));
            }
        });
}

std::string DomDownToolPreview::AmapWebViewSearch::handlePageReady(
    const std::string& requestJson)
{
    const std::optional<Json> payload = firstBindingArgument(requestJson);
    if (!payload.has_value())
    {
        writeSearchTrace("WebView", "page_ready_invalid_payload");
        return makeBindingResult(false, "Invalid page-ready payload.");
    }

    const std::string pageUrl = GetJsonString(*payload, "url");
    writeSearchTrace("WebView", "page_ready", "url=" + pageUrl);
    uint64_t requestToDispatch = 0;
    std::string requestUrl;
    bool collectSearchPage = false;
    {
        const std::lock_guard lock(mutex_);
        pageReady_ = true;
        if (activeRequest_.has_value() && !activeRequest_->completed)
        {
            requestToDispatch = activeRequest_->requestId;
            requestUrl = activeRequest_->url;
            if (retryOnPageReady_)
            {
                retryOnPageReady_ = false;
            }
            else if (activeRequest_->kind == RequestKind::SearchPage &&
                     isSearchPageUrl(pageUrl))
            {
                collectSearchPage = true;
            }
            else
            {
                requestToDispatch = 0;
            }
        }
    }
    condition_.notify_all();

    if (requestToDispatch != 0)
    {
        if (collectSearchPage)
        {
            webview::webview* webView = nullptr;
            {
                const std::lock_guard lock(mutex_);
                webView = static_cast<webview::webview*>(webView_);
            }
            if (webView)
            {
                const std::string script = makeCollectScript(requestToDispatch, requestUrl);
                webView->eval(script);
            }
        }
        else
        {
            dispatchActiveRequest(requestToDispatch);
        }
    }
    return makeBindingResult(true);
}

std::string DomDownToolPreview::AmapWebViewSearch::handleTransportResult(
    const std::string& requestJson)
{
    const std::optional<Json> payload = firstBindingArgument(requestJson);
    if (!payload.has_value())
    {
        writeSearchTrace("WebView", "transport_invalid_payload");
        return makeBindingResult(false, "Invalid transport-result payload.");
    }

    const uint64_t requestId = static_cast<uint64_t>(
        std::max<int64_t>(0, GetJsonInteger(*payload, "requestId")));
    const bool riskControl = GetJsonBool(*payload, "riskControl");
    const bool hasResults = GetJsonBool(*payload, "hasResults");
    const std::string responseUrl = GetJsonString(*payload, "responseUrl");
    writeSearchTrace(
        "WebView", "transport_result",
        fmt::format("id={} status={} risk={} has_results={} source={} content_type={} "
                    "body_bytes={} error={}",
                    requestId, GetJsonInteger(*payload, "statusCode"), riskControl,
                    hasResults, GetJsonString(*payload, "resultSource"),
                    GetJsonString(*payload, "contentType"),
                    GetJsonString(*payload, "body").size(),
                    GetJsonString(*payload, "error")));
    if (riskControl)
    {
        {
            const std::lock_guard lock(mutex_);
            if (!activeRequest_.has_value() || activeRequest_->requestId != requestId ||
                activeRequest_->completed)
            {
                return makeBindingResult(true);
            }
            activeRequest_->waitingForVerification = true;
            pageReady_ = false;
        }
        condition_.notify_all();
        showVerification(responseUrl, true);
        return makeBindingResult(true);
    }

    void* nativeWindow = nullptr;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->requestId != requestId ||
            activeRequest_->completed)
        {
            return makeBindingResult(true);
        }
        if (activeRequest_->waitingForVerification && !hasResults)
        {
            writeSearchTrace("WebView", "transport_result_ignored",
                             fmt::format("id={} reason=waiting_for_verification", requestId));
            return makeBindingResult(true);
        }
        activeRequest_->waitingForVerification = false;
        activeRequest_->response.statusCode = GetJsonInteger(*payload, "statusCode");
        activeRequest_->response.body = GetJsonString(*payload, "body");
        activeRequest_->response.error = GetJsonString(*payload, "error");
        activeRequest_->completed = true;
        nativeWindow = nativeWindow_;
    }
    if (nativeWindow)
    {
        ShowWindow(static_cast<HWND>(nativeWindow), SW_HIDE);
    }
    condition_.notify_all();
    return makeBindingResult(true);
}

std::string DomDownToolPreview::AmapWebViewSearch::handleRiskDetected(
    const std::string& requestJson)
{
    const std::optional<Json> payload = firstBindingArgument(requestJson);
    if (!payload.has_value())
    {
        writeSearchTrace("WebView", "risk_invalid_payload");
        return makeBindingResult(false, "Invalid risk-control payload.");
    }

    writeSearchTrace("WebView", "risk_detected", "url=" + GetJsonString(*payload, "url"));

    bool shouldShowVerification = false;
    {
        const std::lock_guard lock(mutex_);
        if (!activeRequest_.has_value() || activeRequest_->completed)
        {
            return makeBindingResult(true);
        }
        if (activeRequest_->kind == RequestKind::SearchPage &&
            activeRequest_->verificationConfirmed)
        {
            writeSearchTrace("WebView", "risk_ignored",
                             "reason=verification_already_confirmed");
            return makeBindingResult(true);
        }
        if (activeRequest_->waitingForVerification)
        {
            writeSearchTrace("WebView", "risk_ignored",
                             "reason=verification_already_visible");
            return makeBindingResult(true);
        }
        activeRequest_->waitingForVerification = true;
        shouldShowVerification = true;
    }
    condition_.notify_all();
    if (shouldShowVerification)
    {
        showVerification(GetJsonString(*payload, "url"), false);
    }
    return makeBindingResult(true);
}

std::string DomDownToolPreview::AmapWebViewSearch::handleVerificationComplete(
    const std::string&)
{
    writeSearchTrace("WebView", "verification_complete_clicked");
    retryAfterVerification();
    return makeBindingResult(true);
}
