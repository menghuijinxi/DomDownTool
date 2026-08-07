#include "preview/AmapPlaceSearch.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "cpl_json.h"
#include "curl/curl.h"
#include "fmt/format.h"

#include "GCJ02GoogleDownBase.h"
#include "preview/SearchTrace.h"

namespace
{
constexpr std::string_view kDefaultAmapApiKey = "d1f15ad3fdf0c704434eaf23c7c18698";
constexpr std::string_view kPlaceSearchEndpoint = "https://restapi.amap.com/v5/place/text";
constexpr std::string_view kInputTipsEndpoint =
    "https://restapi.amap.com/v3/assistant/inputtips";
constexpr std::string_view kGeocodeEndpoint = "https://restapi.amap.com/v3/geocode/geo";
constexpr std::string_view kWebPageSearchEndpoint = "https://www.amap.com/search";
constexpr std::string_view kWebPageTipsEndpoint =
    "https://www.amap.com/service/poiTipsSearchlite";
constexpr long kConnectTimeoutSeconds = 5;
constexpr long kRequestTimeoutSeconds = 12;
constexpr size_t kMaximumResponseBytes = 2ULL * 1024ULL * 1024ULL;

class CurlGlobalState
{
  public:
    CurlGlobalState()
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            throw std::runtime_error("无法初始化高德地点搜索网络模块。");
        }
    }

    ~CurlGlobalState()
    {
        curl_global_cleanup();
    }
};

struct ResponseBuffer
{
    std::string body;
    bool exceededLimit = false;
};

CurlGlobalState& curlGlobalState()
{
    static CurlGlobalState state;
    return state;
}

size_t writeResponseBody(void* content, size_t size, size_t count, void* userData)
{
    auto* buffer = static_cast<ResponseBuffer*>(userData);
    const size_t byteCount = size * count;
    if (buffer->body.size() + byteCount > kMaximumResponseBytes)
    {
        buffer->exceededLimit = true;
        return 0;
    }

    buffer->body.append(static_cast<const char*>(content), byteCount);
    return byteCount;
}

DomDownToolPreview::PlaceSearchHttpResponse performHttpGet(const std::string& url)
{
    CURL* rawHandle = curl_easy_init();
    if (!rawHandle)
    {
        return {0, {}, "无法创建高德地点搜索请求。"};
    }
    const auto cleanup = [](CURL* handle) { curl_easy_cleanup(handle); };
    const std::unique_ptr<CURL, decltype(cleanup)> handle(rawHandle, cleanup);
    ResponseBuffer buffer;

    curl_easy_setopt(rawHandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(rawHandle, CURLOPT_WRITEFUNCTION, writeResponseBody);
    curl_easy_setopt(rawHandle, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(rawHandle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(rawHandle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(rawHandle, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
    curl_easy_setopt(rawHandle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(rawHandle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(rawHandle, CURLOPT_USERAGENT, "DomDownToolPreview/1.0");

    const CURLcode result = curl_easy_perform(rawHandle);
    long statusCode = 0;
    curl_easy_getinfo(rawHandle, CURLINFO_RESPONSE_CODE, &statusCode);
    if (buffer.exceededLimit)
    {
        return {statusCode, {}, "高德地点搜索响应超过大小限制。"};
    }
    if (result != CURLE_OK)
    {
        return {statusCode, {},
                "高德地点搜索请求失败：" + std::string(curl_easy_strerror(result))};
    }
    return {statusCode, std::move(buffer.body), {}};
}

std::string trimAscii(std::string value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string urlEncode(std::string_view value)
{
    CURL* rawHandle = curl_easy_init();
    if (!rawHandle)
    {
        return {};
    }
    const auto cleanup = [](CURL* handle) { curl_easy_cleanup(handle); };
    const std::unique_ptr<CURL, decltype(cleanup)> handle(rawHandle, cleanup);
    char* rawEncoded = curl_easy_escape(rawHandle, value.data(), static_cast<int>(value.size()));
    if (!rawEncoded)
    {
        return {};
    }
    const std::unique_ptr<char, decltype(&curl_free)> encoded(rawEncoded, curl_free);
    return encoded.get();
}

bool loadSuccessfulResponse(const DomDownToolPreview::PlaceSearchHttpResponse& response,
                            CPLJSONDocument& document, CPLJSONObject& root, std::string& error)
{
    if (!response.error.empty())
    {
        error = response.error;
        return false;
    }
    if (response.statusCode < 200 || response.statusCode >= 300)
    {
        error = "高德地点搜索返回 HTTP " + std::to_string(response.statusCode) + "。";
        return false;
    }
    if (!document.LoadMemory(response.body))
    {
        error = "高德地点搜索返回了无法解析的数据。";
        return false;
    }

    root = document.GetRoot();
    if (root.GetString("status") != "1")
    {
        const std::string infoCode = root.GetString("infocode");
        if (infoCode == "10009")
        {
            error = "当前高德 Key 不是 Web 服务类型，请设置 AMAP_WEB_SERVICE_KEY。";
            return false;
        }
        const std::string information = root.GetString("info", infoCode);
        error = information.empty() ? "高德地点搜索请求被拒绝。"
                                    : "高德地点搜索请求被拒绝：" + information;
        return false;
    }
    return true;
}

std::optional<DownTool::LonLat> parseGcj02Location(const std::string& location)
{
    const size_t separator = location.find(',');
    if (separator == std::string::npos)
    {
        return std::nullopt;
    }

    double longitude = 0.;
    double latitude = 0.;
    const std::string_view longitudeText(location.data(), separator);
    const std::string_view latitudeText(location.data() + separator + 1,
                                        location.size() - separator - 1);
    const auto longitudeResult =
        std::from_chars(longitudeText.data(), longitudeText.data() + longitudeText.size(),
                        longitude);
    const auto latitudeResult =
        std::from_chars(latitudeText.data(), latitudeText.data() + latitudeText.size(), latitude);
    if (longitudeResult.ec != std::errc{} || latitudeResult.ec != std::errc{} ||
        longitude < -180. || longitude > 180. || latitude < -90. || latitude > 90.)
    {
        return std::nullopt;
    }

    return DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84({longitude, latitude});
}

std::optional<DownTool::LonLat> parseGcj02CoordinateFields(const CPLJSONObject& item)
{
    if (const std::optional<DownTool::LonLat> location =
            parseGcj02Location(item.GetString("location")))
    {
        return location;
    }

    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kFieldPairs = {
        std::pair{"x", "y"}, std::pair{"lng", "lat"}, std::pair{"lon", "lat"},
        std::pair{"longitude", "latitude"}};
    const auto parseFields = [&kFieldPairs](const CPLJSONObject& object)
        -> std::optional<DownTool::LonLat>
    {
        for (const auto& [longitudeField, latitudeField] : kFieldPairs)
        {
            const std::string longitude = object.GetString(std::string(longitudeField));
            const std::string latitude = object.GetString(std::string(latitudeField));
            if (longitude.empty() || latitude.empty())
            {
                continue;
            }
            if (const std::optional<DownTool::LonLat> coordinate =
                    parseGcj02Location(longitude + "," + latitude))
            {
                return coordinate;
            }
        }
        return std::nullopt;
    };

    if (const std::optional<DownTool::LonLat> coordinate = parseFields(item))
    {
        return coordinate;
    }
    return parseFields(item.GetObj("point"));
}

std::string joinDistrict(const CPLJSONObject& item)
{
    std::string district;
    const std::array<std::string, 3> fields = {
        item.GetString("pname"), item.GetString("cityname"), item.GetString("adname")};
    for (const std::string& field : fields)
    {
        if (field.empty() || district.ends_with(field))
        {
            continue;
        }
        district += field;
    }
    return district;
}

double recommendedZoomForLevel(const std::string& level)
{
    if (level.find("国家") != std::string::npos)
    {
        return 5.;
    }
    if (level.find("省") != std::string::npos)
    {
        return 7.;
    }
    if (level.find("市") != std::string::npos)
    {
        return 10.;
    }
    if (level.find("区县") != std::string::npos ||
        level.find("开发区") != std::string::npos)
    {
        return 12.;
    }
    if (level.find("乡镇") != std::string::npos)
    {
        return 13.;
    }
    if (level.find("道路") != std::string::npos)
    {
        return 15.;
    }
    return 16.;
}

bool parsePlaces(const CPLJSONObject& root,
                 std::vector<DomDownToolPreview::PlaceSearchResult>& results)
{
    const CPLJSONArray places = root.GetArray("pois");
    for (int index = 0; index < places.Size(); ++index)
    {
        const CPLJSONObject item = places[index];
        const std::optional<DownTool::LonLat> coordinate =
            parseGcj02Location(item.GetString("location"));
        if (!coordinate.has_value())
        {
            continue;
        }

        DomDownToolPreview::PlaceSearchResult result;
        result.id = item.GetString("id");
        result.name = item.GetString("name");
        result.address = item.GetString("address");
        result.district = joinDistrict(item);
        result.category = item.GetString("type");
        result.coordinateWgs84 = coordinate;
        results.push_back(std::move(result));
    }
    return true;
}

bool parseSuggestions(const CPLJSONObject& root,
                      std::vector<DomDownToolPreview::PlaceSearchResult>& results)
{
    const CPLJSONArray tips = root.GetArray("tips");
    const int resultCount = std::min(tips.Size(), 8);
    for (int index = 0; index < resultCount; ++index)
    {
        const CPLJSONObject item = tips[index];
        DomDownToolPreview::PlaceSearchResult result;
        result.id = item.GetString("id");
        result.name = item.GetString("name");
        result.address = item.GetString("address");
        result.district = item.GetString("district");
        result.category = item.GetString("typecode");
        result.coordinateWgs84 = parseGcj02Location(item.GetString("location"));
        if (!result.name.empty())
        {
            results.push_back(std::move(result));
        }
    }
    return true;
}

bool parseWebPageSuggestions(const CPLJSONObject& root,
                             std::vector<DomDownToolPreview::PlaceSearchResult>& results)
{
    const CPLJSONArray tips = root.GetObj("data").GetArray("tip_list");
    const int resultCount = std::min(tips.Size(), 8);
    for (int index = 0; index < resultCount; ++index)
    {
        const CPLJSONObject item = tips[index].GetObj("tip");
        DomDownToolPreview::PlaceSearchResult result;
        result.id = item.GetString("poiid");
        result.name = item.GetString("name");
        result.address = item.GetString("address");
        result.district = item.GetString("district");
        result.category = item.GetString("t_tag_class");
        result.coordinateWgs84 = parseGcj02CoordinateFields(item);
        if (!result.name.empty())
        {
            results.push_back(std::move(result));
        }
    }
    return true;
}

bool parseGeocodes(const CPLJSONObject& root, const std::string& query,
                   std::vector<DomDownToolPreview::PlaceSearchResult>& results)
{
    const CPLJSONArray geocodes = root.GetArray("geocodes");
    for (int index = 0; index < geocodes.Size(); ++index)
    {
        const CPLJSONObject item = geocodes[index];
        const std::optional<DownTool::LonLat> coordinate =
            parseGcj02Location(item.GetString("location"));
        if (!coordinate.has_value())
        {
            continue;
        }

        DomDownToolPreview::PlaceSearchResult result;
        result.name = item.GetString("formatted_address", query);
        result.address = item.GetString("formatted_address");
        result.district = item.GetString("province") + item.GetString("city") +
            item.GetString("district");
        result.level = item.GetString("level");
        result.coordinateWgs84 = coordinate;
        result.recommendedZoom = recommendedZoomForLevel(result.level);
        results.push_back(std::move(result));
    }
    return true;
}

std::string makePlaceSearchUrl(const std::string& apiKey, const std::string& query)
{
    return fmt::format("{}?key={}&keywords={}&page_size=8", kPlaceSearchEndpoint,
                       urlEncode(apiKey), urlEncode(query));
}

std::string makeGeocodeUrl(const std::string& apiKey, const std::string& query)
{
    return fmt::format("{}?key={}&address={}", kGeocodeEndpoint, urlEncode(apiKey),
                       urlEncode(query));
}

std::string makeSuggestionsUrl(const std::string& apiKey, const std::string& query,
                               const std::optional<DownTool::LonLat>& centerWgs84)
{
    std::string url = fmt::format("{}?key={}&keywords={}", kInputTipsEndpoint, urlEncode(apiKey),
                                  urlEncode(query));
    if (centerWgs84.has_value())
    {
        const DownTool::LonLat centerGcj02 =
            DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(*centerWgs84);
        url += fmt::format("&location={:.6f},{:.6f}", centerGcj02.lon, centerGcj02.lat);
    }
    return url;
}

std::string makeGeoObject(const std::optional<DownTool::LonLat>& centerWgs84)
{
    if (!centerWgs84.has_value())
    {
        return {};
    }

    constexpr double kLongitudeRadius = 0.25;
    constexpr double kLatitudeRadius = 0.20;
    const DownTool::LonLat centerGcj02 =
        DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(*centerWgs84);
    return fmt::format("{:.6f}|{:.6f}|{:.6f}|{:.6f}",
                       centerGcj02.lon - kLongitudeRadius,
                       centerGcj02.lat - kLatitudeRadius,
                       centerGcj02.lon + kLongitudeRadius,
                       centerGcj02.lat + kLatitudeRadius);
}

std::string appendWebPageSearchContext(std::string url,
                                       const std::optional<DownTool::LonLat>& centerWgs84)
{
    url += "&city=000000";
    const std::string geoObject = makeGeoObject(centerWgs84);
    if (!geoObject.empty())
    {
        url += "&geoobj=" + urlEncode(geoObject);
    }
    return url;
}

std::string makeWebPagePlaceSearchUrl(
    const std::string& query, const std::optional<DownTool::LonLat>& centerWgs84)
{
    std::string url =
        fmt::format("{}?query={}", kWebPageSearchEndpoint, urlEncode(query));
    url = appendWebPageSearchContext(std::move(url), centerWgs84);
    return url + "&zoom=12";
}

std::string makeWebPageSuggestionsUrl(
    const std::string& query, const std::optional<DownTool::LonLat>& centerWgs84)
{
    std::string url = fmt::format("{}?words={}", kWebPageTipsEndpoint, urlEncode(query));
    return appendWebPageSearchContext(std::move(url), centerWgs84);
}

std::string_view requestKindName(DomDownToolPreview::PlaceSearchRequestKind kind)
{
    return kind == DomDownToolPreview::PlaceSearchRequestKind::Suggestions ? "suggestions"
                                                                           : "search";
}
} // namespace

std::optional<size_t> DomDownToolPreview::findExactPlaceResultIndex(
    const std::vector<PlaceSearchResult>& results, std::string_view expectedId,
    std::string_view expectedName)
{
    for (size_t index = 0; index < results.size(); ++index)
    {
        const PlaceSearchResult& result = results[index];
        if (!result.coordinateWgs84.has_value())
        {
            continue;
        }
        if (!expectedId.empty() && !result.id.empty())
        {
            if (result.id == expectedId)
            {
                return index;
            }
            continue;
        }
        if (!expectedName.empty() && result.name == expectedName)
        {
            return index;
        }
    }
    return std::nullopt;
}

DomDownToolPreview::AmapPlaceSearch::AmapPlaceSearch(
    std::string apiKey, std::function<void()> completionCallback, HttpGet httpGet)
    : apiKey_(trimAscii(std::move(apiKey))),
      completionCallback_(std::move(completionCallback)), httpGet_(std::move(httpGet))
{
    writeSearchTrace("PlaceSearch", "web_service_constructor");
    (void)curlGlobalState();
    if (!httpGet_)
    {
        httpGet_ = performHttpGet;
    }
    worker_ = std::thread([this] { workerLoop(); });
}

DomDownToolPreview::AmapPlaceSearch::AmapPlaceSearch(
    std::function<void()> completionCallback, HttpGet webPageGet, CancelHttp cancelHttp)
    : completionCallback_(std::move(completionCallback)), httpGet_(std::move(webPageGet)),
      cancelHttp_(std::move(cancelHttp)), backend_(Backend::WebPage)
{
    writeSearchTrace("PlaceSearch", "web_page_constructor");
    if (!httpGet_)
    {
        throw std::invalid_argument("Web page search transport is required.");
    }
    worker_ = std::thread([this] { workerLoop(); });
}

DomDownToolPreview::AmapPlaceSearch::~AmapPlaceSearch()
{
    writeSearchTrace("PlaceSearch", "destructor_start");
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
    }
    condition_.notify_all();
    if (cancelHttp_)
    {
        cancelHttp_();
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    writeSearchTrace("PlaceSearch", "destructor_complete");
}

uint64_t DomDownToolPreview::AmapPlaceSearch::submitSearch(std::string query)
{
    PendingRequest request;
    request.kind = PlaceSearchRequestKind::Search;
    request.query = trimAscii(std::move(query));
    request.executeAfter = std::chrono::steady_clock::now();
    return submit(std::move(request));
}

uint64_t DomDownToolPreview::AmapPlaceSearch::submitSearch(
    std::string query, const DownTool::LonLat& centerWgs84)
{
    PendingRequest request;
    request.kind = PlaceSearchRequestKind::Search;
    request.query = trimAscii(std::move(query));
    request.centerWgs84 = centerWgs84;
    request.executeAfter = std::chrono::steady_clock::now();
    return submit(std::move(request));
}

uint64_t DomDownToolPreview::AmapPlaceSearch::submitSuggestions(
    std::string query, const DownTool::LonLat& centerWgs84, std::chrono::milliseconds debounce)
{
    PendingRequest request;
    request.kind = PlaceSearchRequestKind::Suggestions;
    request.query = trimAscii(std::move(query));
    request.centerWgs84 = centerWgs84;
    request.executeAfter =
        std::chrono::steady_clock::now() +
        std::max(debounce, std::chrono::milliseconds::zero());
    return submit(std::move(request));
}

void DomDownToolPreview::AmapPlaceSearch::cancelPending()
{
    uint64_t cancellationId = 0;
    {
        const std::lock_guard lock(mutex_);
        cancellationId = ++latestRequestId_;
        pending_.reset();
        completed_.clear();
    }
    writeSearchTrace("PlaceSearch", "cancel_pending",
                     fmt::format("generation={}", cancellationId));
    condition_.notify_all();
    if (cancelHttp_)
    {
        cancelHttp_();
    }
}

std::vector<DomDownToolPreview::PlaceSearchResponse>
DomDownToolPreview::AmapPlaceSearch::takeCompleted()
{
    std::deque<PlaceSearchResponse> completed;
    {
        const std::lock_guard lock(mutex_);
        completed.swap(completed_);
    }
    return {std::make_move_iterator(completed.begin()),
            std::make_move_iterator(completed.end())};
}

std::string DomDownToolPreview::AmapPlaceSearch::defaultApiKey()
{
    const char* environmentKey = std::getenv("AMAP_WEB_SERVICE_KEY");
    if (environmentKey && environmentKey[0] != '\0')
    {
        return environmentKey;
    }
    return std::string(kDefaultAmapApiKey);
}

uint64_t DomDownToolPreview::AmapPlaceSearch::submit(PendingRequest request)
{
    {
        const std::lock_guard lock(mutex_);
        request.requestId = ++latestRequestId_;
        pending_ = request;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
        request.executeAfter - std::chrono::steady_clock::now());
    writeSearchTrace(
        "PlaceSearch", "request_submitted",
        fmt::format("id={} kind={} debounce_ms={} query={}", request.requestId,
                    requestKindName(request.kind), std::max<int64_t>(delay.count(), 0),
                    request.query));
    if (cancelHttp_)
    {
        cancelHttp_();
    }
    condition_.notify_all();
    return request.requestId;
}

DomDownToolPreview::PlaceSearchResponse DomDownToolPreview::AmapPlaceSearch::execute(
    const PendingRequest& request) const
{
    if (request.kind == PlaceSearchRequestKind::Suggestions)
    {
        return executeSuggestions(request);
    }
    return executeSearch(request);
}

DomDownToolPreview::PlaceSearchResponse DomDownToolPreview::AmapPlaceSearch::executeSearch(
    const PendingRequest& request) const
{
    PlaceSearchResponse response;
    response.requestId = request.requestId;
    response.kind = request.kind;
    response.query = request.query;
    if (request.query.empty())
    {
        return response;
    }
    if (backend_ == Backend::WebPage)
    {
        CPLJSONDocument document;
        CPLJSONObject root;
        const PlaceSearchHttpResponse http =
            httpGet_(makeWebPagePlaceSearchUrl(request.query, request.centerWgs84));
        writeSearchTrace(
            "PlaceSearch", "search_http_complete",
            fmt::format("id={} status={} body_bytes={} transport_error={}", request.requestId,
                        http.statusCode, http.body.size(), http.error));
        if (!loadSuccessfulResponse(http, document, root, response.error))
        {
            return response;
        }
        parsePlaces(root, response.results);
        writeSearchTrace("PlaceSearch", "search_parse_complete",
                         fmt::format("id={} results={}", request.requestId,
                                     response.results.size()));
        return response;
    }
    if (apiKey_.empty())
    {
        response.error = "未配置高德 Web 服务 Key。";
        return response;
    }

    CPLJSONDocument placeDocument;
    CPLJSONObject placeRoot;
    const PlaceSearchHttpResponse placeHttp = httpGet_(makePlaceSearchUrl(apiKey_, request.query));
    if (!loadSuccessfulResponse(placeHttp, placeDocument, placeRoot, response.error))
    {
        return response;
    }
    parsePlaces(placeRoot, response.results);
    if (!response.results.empty())
    {
        return response;
    }

    response.usedGeocodeFallback = true;
    CPLJSONDocument geocodeDocument;
    CPLJSONObject geocodeRoot;
    const PlaceSearchHttpResponse geocodeHttp = httpGet_(makeGeocodeUrl(apiKey_, request.query));
    if (!loadSuccessfulResponse(geocodeHttp, geocodeDocument, geocodeRoot, response.error))
    {
        return response;
    }
    parseGeocodes(geocodeRoot, request.query, response.results);
    return response;
}

DomDownToolPreview::PlaceSearchResponse DomDownToolPreview::AmapPlaceSearch::executeSuggestions(
    const PendingRequest& request) const
{
    PlaceSearchResponse response;
    response.requestId = request.requestId;
    response.kind = request.kind;
    response.query = request.query;
    if (request.query.empty())
    {
        return response;
    }
    if (backend_ == Backend::WebPage)
    {
        CPLJSONDocument document;
        CPLJSONObject root;
        const PlaceSearchHttpResponse http =
            httpGet_(makeWebPageSuggestionsUrl(request.query, request.centerWgs84));
        writeSearchTrace(
            "PlaceSearch", "suggestions_http_complete",
            fmt::format("id={} status={} body_bytes={} transport_error={}", request.requestId,
                        http.statusCode, http.body.size(), http.error));
        if (!loadSuccessfulResponse(http, document, root, response.error))
        {
            return response;
        }
        parseWebPageSuggestions(root, response.results);
        writeSearchTrace("PlaceSearch", "suggestions_parse_complete",
                         fmt::format("id={} results={}", request.requestId,
                                     response.results.size()));
        return response;
    }
    if (apiKey_.empty())
    {
        response.error = "未配置高德 Web 服务 Key。";
        return response;
    }

    CPLJSONDocument document;
    CPLJSONObject root;
    const PlaceSearchHttpResponse http =
        httpGet_(makeSuggestionsUrl(apiKey_, request.query, request.centerWgs84));
    if (!loadSuccessfulResponse(http, document, root, response.error))
    {
        return response;
    }
    parseSuggestions(root, response.results);
    return response;
}

void DomDownToolPreview::AmapPlaceSearch::workerLoop()
{
    while (true)
    {
        PendingRequest request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
            if (stopping_)
            {
                return;
            }

            const auto now = std::chrono::steady_clock::now();
            if (pending_->executeAfter > now)
            {
                condition_.wait_until(lock, pending_->executeAfter);
                continue;
            }
            request = std::move(*pending_);
            pending_.reset();
        }

        writeSearchTrace("PlaceSearch", "worker_execute_start",
                         fmt::format("id={} kind={}", request.requestId,
                                     requestKindName(request.kind)));

        PlaceSearchResponse response = execute(request);
        const size_t resultCount = response.results.size();
        const std::string responseError = response.error;
        bool accepted = false;
        {
            const std::lock_guard lock(mutex_);
            if (!stopping_ && response.requestId == latestRequestId_)
            {
                completed_.push_back(std::move(response));
                accepted = true;
            }
        }
        writeSearchTrace(
            "PlaceSearch", accepted ? "worker_response_accepted" : "worker_response_stale",
            fmt::format("id={} results={} error={}", request.requestId, resultCount,
                        responseError));
        if (accepted && completionCallback_)
        {
            writeSearchTrace("PlaceSearch", "completion_callback_start",
                             fmt::format("id={}", request.requestId));
            completionCallback_();
            writeSearchTrace("PlaceSearch", "completion_callback_complete",
                             fmt::format("id={}", request.requestId));
        }
    }
}
