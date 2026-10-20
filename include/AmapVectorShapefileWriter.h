#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "AmapVectorDecoder.h"

namespace DownTool
{
class AmapVectorShapefileWriter
{
  public:
    static bool WriteDirectory(const AmapVectorDecodedData& decodedData,
                               const std::filesystem::path& outputDirectory, std::string& error,
                               const std::function<bool()>& cancellationRequested = {});
};
} // namespace DownTool
