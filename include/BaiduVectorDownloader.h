#pragma once

#include <string>
#include <vector>

#include "AmapVectorDownloader.h"
#include "BaiduVectorTileGrid.h"

namespace DownTool
{
class BaiduVectorDownloader
{
  public:
    static std::string TileUrl(const BaiduVectorTile& tile);
    static std::vector<BaiduVectorTile> SelectTiles(const AmapVectorDownloadOptions& options);
    AmapVectorDownloadResult Download(const AmapVectorDownloadOptions& options) const;
};
} // namespace DownTool
