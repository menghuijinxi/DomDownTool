#pragma once

#include <map>
#include <string>
#include <vector>

#include "AmapVectorDecoder.h"

namespace DownTool
{
using BaiduFeatureTypes = std::map<uint32_t, std::string>;

class BaiduVectorClassification
{
  public:
    static constexpr const char* kVersion = "088";
    static constexpr const char* kUpdateDate = "20261021";
    static bool DecodeResponse(const std::string& response, const std::vector<uint32_t>& ids,
                               BaiduFeatureTypes& types, std::string& error);
    static bool Apply(AmapVectorDecodedData& decoded, const BaiduFeatureTypes& types,
                      std::string& error);
};
} // namespace DownTool
