#pragma once

#include <string>
#include <vector>

#include "AmapVectorDownloader.h"

namespace DownTool
{
    class TencentVectorDownloader
    {
      public:
        static std::string TileUrl(const AmapVectorTile& tile);
        static std::vector<AmapVectorTile> SelectTiles(const AmapVectorDownloadOptions& options);
        AmapVectorDownloadResult Download(const AmapVectorDownloadOptions& options) const;
    };
} // namespace DownTool
