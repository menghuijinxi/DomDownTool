#include "EsriWaybackDown.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "nlohmann/json.hpp"

#include "PostProcessTestSupport.h"
#include "preview/WaybackCatalog.h"

namespace
{
using postprocess_test::Require;
using namespace std::chrono_literals;

std::string Fixture()
{
    std::ifstream input(std::filesystem::path(DOMDOWNTOOL_SOURCE_DIR) /
                        "tests/fixtures/wayback-catalog.json");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool TestCatalogAndRequests()
{
    const auto releases = DownTool::ParseWaybackCatalog(Fixture());
    if (!Require(releases.size() == 3 && releases.front().GetNumber() == "63116" &&
                     releases.back().GetNumber() == "10",
                 "版本必须按发布日期排序") ||
        !Require(releases.front().GetItemId() == "ab83ada1bd3145978eecfb115aaa10c3",
                 "版本身份丢失"))
    {
        return false;
    }
    DownTool::EsriWaybackDown projection;
    if (!Require(projection.GetUrlTemplate().empty(), "目录未加载时不能请求未选定版本"))
    {
        return false;
    }
    const DownTool::Tile tile(215847, 99347, 18);
    projection.SetRelease(releases.front());
    const auto request = projection.BuildTileHttpRequest(tile);
    const auto firstCache = projection.GetCachePath();
    if (!Require(request.url == "https://wayback.maptiles.arcgis.com/arcgis/rest/services/"
                                "World_Imagery/WMTS/1.0.0/default028mm/MapServer/tile/"
                                "63116/18/99347/215847",
                 "WMTS 层级、行、列顺序错误"))
    {
        return false;
    }
    projection.SetRelease(releases.back());
    return Require(projection.GetCachePath() != firstCache &&
                       projection.GetCachePath() == "cache/Esri Wayback/preview/10" &&
                       projection.MakeUri(tile).find("/tile/10/18/99347/215847") !=
                           std::string::npos,
                   "切换版本必须隔离缓存和请求");
}

bool Rejects(const std::string& text)
{
    try
    {
        (void)DownTool::ParseWaybackCatalog(text);
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

bool TestInvalidCatalog()
{
    if (!Require(Rejects("{}") && Rejects("[]") && Rejects("{broken") &&
                     Rejects(std::string(4 * 1024 * 1024 + 1, ' ')),
                 "空目录、非对象、损坏或超大目录必须报告失败"))
    {
        return false;
    }
    const auto fixture = nlohmann::json::parse(Fixture());
    for (const auto& [field, value] :
         {std::pair{"itemTitle", "World Imagery (Wayback 2026-02-30)"},
          {"itemTitle", "World Imagery (Wayback 2026-13-01)"},
          {"itemID", "invalid"},
          {"itemURL", "https://untrusted.example/{level}/{row}/{col}"}})
    {
        auto broken = fixture;
        broken["63116"][field] = value;
        if (!Require(Rejects(broken.dump()), "未拒绝无效版本字段"))
        {
            return false;
        }
    }
    auto broken = fixture;
    broken["../63116"] = broken["63116"];
    return Require(Rejects(broken.dump()), "版本编号不能成为缓存路径穿越入口");
}

bool TestAsyncRefreshAndRecovery()
{
    std::atomic_int calls = 0;
    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    DomDownToolPreview::WaybackCatalog catalog(
        [&] {
            const std::lock_guard lock(mutex);
            completed = true;
            condition.notify_one();
        },
        [&](std::stop_token) {
            if (++calls == 1)
            {
                throw std::runtime_error("测试网络失败");
            }
            return Fixture();
        });
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        {
            const std::lock_guard lock(mutex);
            completed = false;
        }
        if (!Require(catalog.Refresh(), "无法发起异步目录请求"))
        {
            return false;
        }
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return completed; }))
        {
            return Require(false, "目录完成通知超时");
        }
        lock.unlock();
        const auto result = catalog.TakeCompleted();
        if (!Require(result.has_value() && !catalog.IsLoading(), "异步结果丢失") ||
            !Require(attempt == 0 ? !result->GetError().empty()
                                  : result->GetError().empty() && result->GetReleases().size() == 3,
                     "目录失败后应能刷新恢复") ||
            !Require(!catalog.TakeCompleted(), "目录结果只能消费一次"))
        {
            return false;
        }
    }
    return true;
}

bool TestShutdownCancellation()
{
    std::mutex mutex;
    std::condition_variable_any condition;
    std::atomic_bool started = false;
    std::atomic_bool cancelled = false;
    std::atomic_bool notified = false;
    const auto before = std::chrono::steady_clock::now();
    {
        DomDownToolPreview::WaybackCatalog catalog([&] { notified = true; },
                                                   [&](std::stop_token stop) {
                                                       started = true;
                                                       condition.notify_all();
                                                       std::unique_lock lock(mutex);
                                                       condition.wait(lock, stop,
                                                                      [] { return false; });
                                                       cancelled = stop.stop_requested();
                                                       return Fixture();
                                                   });
        catalog.Refresh();
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, 2s, [&] { return started.load(); }) ||
            !Require(!catalog.Refresh(), "加载中不能重复发起请求"))
        {
            return false;
        }
    }
    return Require(cancelled && !notified && std::chrono::steady_clock::now() - before < 2s,
                   "关闭时应取消网络工作且不再回调 UI");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--live")
    {
        DomDownToolPreview::WaybackCatalog catalog([] {});
        catalog.Refresh();
        const auto deadline = std::chrono::steady_clock::now() + 25s;
        while (catalog.IsLoading() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(20ms);
        }
        const auto result = catalog.TakeCompleted();
        if (!Require(result && result->GetError().empty(),
                     result ? result->GetError() : "在线目录请求未完成"))
        {
            return 1;
        }
        std::cout << "Live catalog: " << result->GetReleases().size() << " releases, latest "
                  << result->GetReleases().front().GetDate() << '\n';
        return 0;
    }
    const bool passed = TestCatalogAndRequests() && TestInvalidCatalog() &&
                        TestAsyncRefreshAndRecovery() && TestShutdownCancellation();
    std::cout << (passed ? "WaybackTest passed\n" : "WaybackTest failed\n");
    return passed ? 0 : 1;
}
