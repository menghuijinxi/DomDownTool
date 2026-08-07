#include "PostProcessTestSupport.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "GCJ02GoogleDownBase.h"
#include "preview/AmapPlaceSearch.h"

namespace
{
using namespace std::chrono_literals;

class CompletionSignal
{
  public:
    void notify()
    {
        {
            const std::lock_guard lock(mutex_);
            notified_ = true;
        }
        condition_.notify_all();
    }

    bool wait()
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 3s, [this] { return notified_; });
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool notified_ = false;
};

bool isNear(const DownTool::LonLat& left, const DownTool::LonLat& right)
{
    return std::abs(left.lon - right.lon) < 0.000001 &&
        std::abs(left.lat - right.lat) < 0.000001;
}

bool testPoiSearchAndCoordinateConversion()
{
    CompletionSignal completion;
    std::vector<std::string> urls;
    DomDownToolPreview::AmapPlaceSearch search(
        "test-key", [&completion] { completion.notify(); },
        [&urls](const std::string& url)
        {
            urls.push_back(url);
            return DomDownToolPreview::PlaceSearchHttpResponse{
                200,
                R"json({"status":"1","info":"OK","pois":[{"id":"B000A",)json"
                R"json("name":"天安门","address":"东长安街",)json"
                R"json("location":"116.397428,39.909230","pname":"北京市",)json"
                R"json("cityname":"北京市","adname":"东城区","type":"风景名胜"}]})json",
                {}};
        });

    search.submitSearch("天安门");
    if (!postprocess_test::Require(completion.wait(), "POI 搜索没有完成"))
    {
        return false;
    }

    const std::vector<DomDownToolPreview::PlaceSearchResponse> responses =
        search.takeCompleted();
    const DownTool::LonLat expected =
        DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84({116.397428, 39.909230});
    return postprocess_test::Require(
               urls.size() == 1 && urls.front().find("/v5/place/text") != std::string::npos,
               "POI 搜索接口错误") &&
        postprocess_test::Require(
               urls.front().find("%E5%A4%A9%E5%AE%89%E9%97%A8") != std::string::npos,
               "中文关键词没有进行 URL 编码") &&
        postprocess_test::Require(
               responses.size() == 1 && responses.front().succeeded() &&
                   responses.front().results.size() == 1 &&
                   responses.front().results.front().coordinateWgs84.has_value(),
               "POI 搜索结果解析失败") &&
        postprocess_test::Require(
               isNear(*responses.front().results.front().coordinateWgs84, expected),
               "POI 坐标没有转换为 WGS84");
}

bool testGeocodeFallback()
{
    CompletionSignal completion;
    std::vector<std::string> urls;
    DomDownToolPreview::AmapPlaceSearch search(
        "test-key", [&completion] { completion.notify(); },
        [&urls](const std::string& url)
        {
            urls.push_back(url);
            if (url.find("/v5/place/text") != std::string::npos)
            {
                return DomDownToolPreview::PlaceSearchHttpResponse{
                    200, R"json({"status":"1","info":"OK","pois":[]})json", {}};
            }
            return DomDownToolPreview::PlaceSearchHttpResponse{
                200,
                R"json({"status":"1","info":"OK","geocodes":[{)json"
                R"json("formatted_address":"北京市东城区东长安街1号",)json"
                R"json("province":"北京市","city":"北京市","district":"东城区",)json"
                R"json("location":"116.397500,39.908700","level":"门牌号"}]})json",
                {}};
        });

    search.submitSearch("北京市东城区东长安街1号");
    if (!postprocess_test::Require(completion.wait(), "地理编码兜底没有完成"))
    {
        return false;
    }

    const std::vector<DomDownToolPreview::PlaceSearchResponse> responses =
        search.takeCompleted();
    return postprocess_test::Require(
               urls.size() == 2 && urls.back().find("/v3/geocode/geo") != std::string::npos,
               "POI 空结果后没有调用地理编码") &&
        postprocess_test::Require(
               responses.size() == 1 && responses.front().usedGeocodeFallback &&
                   responses.front().results.size() == 1 &&
                   responses.front().results.front().recommendedZoom == 16.,
               "地理编码结果解析失败");
}

bool testSuggestionDebounceKeepsLatestQuery()
{
    CompletionSignal completion;
    std::mutex urlsMutex;
    std::vector<std::string> urls;
    DomDownToolPreview::AmapPlaceSearch search(
        "test-key", [&completion] { completion.notify(); },
        [&urls, &urlsMutex](const std::string& url)
        {
            {
                const std::lock_guard lock(urlsMutex);
                urls.push_back(url);
            }
            return DomDownToolPreview::PlaceSearchHttpResponse{
                200,
                R"json({"status":"1","info":"OK","tips":[{"id":"B1",)json"
                R"json("name":"天安门广场","district":"北京市东城区",)json"
                R"json("address":"东长安街","location":"116.397700,39.903200",)json"
                R"json("typecode":"110101"}]})json",
                {}};
        });

    search.submitSuggestions("天", {116.397389, 39.908722}, 150ms);
    search.submitSuggestions("天安门", {116.397389, 39.908722}, 150ms);
    if (!postprocess_test::Require(completion.wait(), "输入提示没有完成"))
    {
        return false;
    }

    const std::vector<DomDownToolPreview::PlaceSearchResponse> responses =
        search.takeCompleted();
    const std::lock_guard lock(urlsMutex);
    return postprocess_test::Require(urls.size() == 1, "防抖期间执行了过期输入请求") &&
        postprocess_test::Require(
               urls.front().find("%E5%A4%A9%E5%AE%89%E9%97%A8") != std::string::npos,
               "输入提示没有保留最新关键词") &&
        postprocess_test::Require(
               responses.size() == 1 &&
                   responses.front().kind ==
                       DomDownToolPreview::PlaceSearchRequestKind::Suggestions &&
                   responses.front().query == "天安门" && responses.front().results.size() == 1,
               "输入提示结果解析失败");
}

bool testWebPageSuggestions()
{
    CompletionSignal completion;
    std::vector<std::string> urls;
    DomDownToolPreview::AmapPlaceSearch search(
        [&completion] { completion.notify(); },
        [&urls](const std::string& url)
        {
            urls.push_back(url);
            return DomDownToolPreview::PlaceSearchHttpResponse{
                200,
                R"json({"status":"1","data":{"result":"true","tip_list":[{"tip":{)json"
                R"json("poiid":"B02F38IPWZ","name":"深圳北站",)json"
                R"json("address":"致远中路28号","district":"广东省深圳市龙华区",)json"
                R"json("x":"114.029506","y":"22.609878",)json"
                R"json("t_tag_class":"交通设施;火车站"}}]}})json",
                {}};
        },
        [] {});

    search.submitSuggestions("深圳北站", {114.029506, 22.609878}, 0ms);
    if (!postprocess_test::Require(completion.wait(), "网页联想搜索没有完成"))
    {
        return false;
    }

    const std::vector<DomDownToolPreview::PlaceSearchResponse> responses =
        search.takeCompleted();
    const DownTool::LonLat expected =
        DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84({114.029506, 22.609878});
    return postprocess_test::Require(
               urls.size() == 1 &&
                   urls.front().find("/service/poiTipsSearchlite") != std::string::npos,
               "网页联想接口错误") &&
        postprocess_test::Require(urls.front().find("city=000000") != std::string::npos &&
                                      urls.front().find("geoobj=") != std::string::npos,
                                  "网页联想没有携带全国模式和中心范围") &&
        postprocess_test::Require(
               responses.size() == 1 && responses.front().succeeded() &&
                   responses.front().results.size() == 1 &&
                   responses.front().results.front().name == "深圳北站" &&
                   responses.front().results.front().coordinateWgs84.has_value(),
               "网页联想坐标解析失败") &&
        postprocess_test::Require(
            isNear(*responses.front().results.front().coordinateWgs84, expected),
            "网页联想坐标没有转换为 WGS84");
}

bool testWebPageSearchAndCoordinateConversion()
{
    CompletionSignal completion;
    std::vector<std::string> urls;
    DomDownToolPreview::AmapPlaceSearch search(
        [&completion] { completion.notify(); },
        [&urls](const std::string& url)
        {
            urls.push_back(url);
            return DomDownToolPreview::PlaceSearchHttpResponse{
                200,
                R"json({"status":"1","info":"OK","pois":[{"id":"B02F38IPWZ",)json"
                R"json("name":"深圳北站","address":"致远中路28号",)json"
                R"json("location":"114.029506,22.609878","type":"交通设施"}]})json",
                {}};
        },
        [] {});

    search.submitSearch("深圳北站", {114.029506, 22.609878});
    if (!postprocess_test::Require(completion.wait(), "网页完整搜索没有完成"))
    {
        return false;
    }

    const std::vector<DomDownToolPreview::PlaceSearchResponse> responses =
        search.takeCompleted();
    const DownTool::LonLat expected =
        DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84({114.029506, 22.609878});
    return postprocess_test::Require(
               urls.size() == 1 && urls.front().find("/search?") != std::string::npos,
               "网页完整搜索接口错误") &&
        postprocess_test::Require(urls.front().find("city=000000") != std::string::npos &&
                                      urls.front().find("geoobj=") != std::string::npos,
                                  "网页完整搜索没有携带全国模式和中心范围") &&
        postprocess_test::Require(
               responses.size() == 1 && responses.front().succeeded() &&
                   responses.front().results.size() == 1 &&
                   responses.front().results.front().coordinateWgs84.has_value(),
               "网页完整搜索结果解析失败") &&
        postprocess_test::Require(
               isNear(*responses.front().results.front().coordinateWgs84, expected),
               "网页搜索坐标没有转换为 WGS84");
}

bool testResolvedLocationRequiresExactPoiMatch()
{
    using DomDownToolPreview::PlaceSearchResult;

    const std::vector<PlaceSearchResult> results = {
        {.id = "WRONG-1",
         .name = "普渡寺东巷",
         .coordinateWgs84 = DownTool::LonLat{116.398756, 39.911317}},
        {.id = "WRONG-2",
         .name = "全国",
         .coordinateWgs84 = DownTool::LonLat{116.362107, 39.913799}},
        {.id = "SAME-NAME-WRONG-ID",
         .name = "点石商务公园",
         .coordinateWgs84 = DownTool::LonLat{116.390000, 39.920000}},
        {.id = "TARGET-ID",
         .name = "点石商务公园",
         .coordinateWgs84 = DownTool::LonLat{116.196000, 39.914000}}};

    const std::optional<size_t> idMatch =
        DomDownToolPreview::findExactPlaceResultIndex(results, "TARGET-ID", "点石商务公园");
    const std::optional<size_t> nameMatch =
        DomDownToolPreview::findExactPlaceResultIndex(results, "", "点石商务公园");
    const std::optional<size_t> conflictingIdMatch =
        DomDownToolPreview::findExactPlaceResultIndex(results, "MISSING", "点石商务公园");
    const std::optional<size_t> missingMatch =
        DomDownToolPreview::findExactPlaceResultIndex(results, "MISSING", "不存在的地点");

    return postprocess_test::Require(idMatch == 3, "没有按 POI ID 精确匹配定位结果") &&
        postprocess_test::Require(nameMatch == 2, "没有按地点名称精确匹配定位结果") &&
        postprocess_test::Require(!conflictingIdMatch.has_value(),
                                  "错误地接受了 POI ID 冲突的同名地点") &&
        postprocess_test::Require(!missingMatch.has_value(),
                                  "错误地使用了第一个无关坐标作为定位结果");
}

bool testApiFailureIsReported()
{
    CompletionSignal completion;
    DomDownToolPreview::AmapPlaceSearch search(
        "invalid-key", [&completion] { completion.notify(); },
        [](const std::string&)
        {
            return DomDownToolPreview::PlaceSearchHttpResponse{
                200,
                R"json({"status":"0","info":"USERKEY_PLAT_NOMATCH","infocode":"10009"})json",
                {}};
        });

    search.submitSearch("测试");
    if (!postprocess_test::Require(completion.wait(), "失败响应没有完成"))
    {
        return false;
    }
    const std::vector<DomDownToolPreview::PlaceSearchResponse> responses =
        search.takeCompleted();
    return postprocess_test::Require(
        responses.size() == 1 && !responses.front().succeeded() &&
            responses.front().error.find("Web 服务类型") != std::string::npos,
        "高德 API 错误没有传递给调用方");
}

bool runTest()
{
    const bool succeeded = testPoiSearchAndCoordinateConversion() && testGeocodeFallback() &&
        testSuggestionDebounceKeepsLatestQuery() && testWebPageSuggestions() &&
        testWebPageSearchAndCoordinateConversion() &&
        testResolvedLocationRequiresExactPoiMatch() && testApiFailureIsReported();
    if (succeeded)
    {
        std::cout << "AmapPlaceSearchTest passed\n";
    }
    return succeeded;
}
} // namespace

int main()
{
    return postprocess_test::RunGuarded(runTest);
}
