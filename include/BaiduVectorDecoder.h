#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "AmapVectorDecoder.h"
#include "BaiduVectorTileGrid.h"

namespace DownTool
{
class BaiduVectorDecodeOptions
{
  public:
    explicit BaiduVectorDecodeOptions(const BaiduVectorTile& tile) : tile_(tile) {}
    const BaiduVectorTile& GetTile() const { return tile_; }
    const std::function<bool()>& GetCancellation() const { return cancellation_; }
    void SetCancellation(const std::function<bool()>& value) { cancellation_ = value; }

  protected:
    BaiduVectorTile tile_;
    std::function<bool()> cancellation_;
};

class BaiduVectorDecoder
{
  public:
    // 解码专用目录格式；面通过原始三角网格并集恢复，结果统一为 WGS84。
    static bool DecodeTile(std::string_view bytes, const BaiduVectorDecodeOptions& options,
                           AmapVectorDecodedData& decoded, std::string& error);
};
} // namespace DownTool
