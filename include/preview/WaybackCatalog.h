#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "EsriWaybackDown.h"

namespace DomDownToolPreview
{
class WaybackCatalogResult
{
  public:
    explicit WaybackCatalogResult(std::vector<DownTool::WaybackRelease> releases);
    explicit WaybackCatalogResult(std::string error);

    [[nodiscard]] const std::vector<DownTool::WaybackRelease>& GetReleases() const
    {
        return releases_;
    }
    [[nodiscard]] const std::string& GetError() const { return error_; }

  protected:
    std::vector<DownTool::WaybackRelease> releases_;
    std::string error_;
};

class WaybackCatalog
{
  public:
    using Fetch = std::function<std::string(std::stop_token)>;

    explicit WaybackCatalog(std::function<void()> onComplete, Fetch fetch = {});

    bool Refresh();
    [[nodiscard]] bool IsLoading() const;
    [[nodiscard]] std::optional<WaybackCatalogResult> TakeCompleted();

  protected:
    std::function<void()> onComplete_;
    Fetch fetch_;
    mutable std::mutex mutex_;
    bool loading_ = false;
    std::optional<WaybackCatalogResult> completed_;
    // 析构时先取消并等待网络线程，再释放结果、互斥锁和回调。
    std::jthread worker_;
};
} // namespace DomDownToolPreview
