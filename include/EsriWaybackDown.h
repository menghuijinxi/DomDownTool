#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "GoogleDownBase.h"

namespace DownTool
{
class WaybackRelease
{
  public:
    WaybackRelease(std::string number, std::string date, std::string itemId, std::string tileUrl);

    [[nodiscard]] const std::string& GetNumber() const { return number_; }
    [[nodiscard]] const std::string& GetDate() const { return date_; }
    [[nodiscard]] const std::string& GetItemId() const { return itemId_; }
    [[nodiscard]] const std::string& GetTileUrl() const { return tileUrl_; }

  protected:
    std::string number_;
    std::string date_;
    std::string itemId_;
    std::string tileUrl_;
};

[[nodiscard]] std::vector<WaybackRelease> ParseWaybackCatalog(std::string_view text);

class EsriWaybackDown : public GoogleDownBase
{
  public:
    EsriWaybackDown();

    void SetRelease(const WaybackRelease& release);

    [[nodiscard]] std::vector<std::string> GetRequestHeaders(const Tile& tile) const override;
};
} // namespace DownTool
