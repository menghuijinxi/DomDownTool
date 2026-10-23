#pragma once

#include <string>

#include "AmapVectorDecoder.h"
#include "AmapVectorTileGrid.h"

namespace DownTool
{
    class TencentVectorDecoder
    {
      public:
        // tile 使用 GCJ-02 Web Mercator XYZ；compressed 对应请求的 compress 参数。
        static bool DecodeEnvelope(const std::string& envelope, const AmapVectorTile& tile,
                                   AmapVectorDecodedData& decodedData, std::string& error,
                                   bool compressed = true);
    };
} // namespace DownTool
