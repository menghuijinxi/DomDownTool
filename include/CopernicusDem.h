#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "DataType.h"

namespace DownTool
{
struct CopernicusDemProgress
{
    std::string stage;
    uint64_t completed = 0;
    uint64_t total = 0;
};

using CopernicusDemCancellationCallback = std::function<bool()>;
using CopernicusDemProgressCallback = std::function<void(const CopernicusDemProgress&)>;
using CopernicusDemDownloadCallback = std::function<bool(
    std::string_view url,
    const std::filesystem::path& outputPath,
    const CopernicusDemCancellationCallback& cancellationRequested,
    std::string& error)>;

struct CopernicusDemOptions
{
    LonLat centerWgs84;
    double radiusMeters = 0.;
    std::filesystem::path cachePath =
        std::filesystem::u8path("cache/Copernicus高程");
    CopernicusDemCancellationCallback cancellationRequested;
    CopernicusDemProgressCallback progressCallback;
    CopernicusDemDownloadCallback downloadCallback;
};

struct CopernicusDemBounds
{
    double west = 0.;
    double east = 0.;
    double south = 0.;
    double north = 0.;
};

class CopernicusDemData
{
  public:
    ~CopernicusDemData();

    CopernicusDemData(const CopernicusDemData&) = delete;
    CopernicusDemData& operator=(const CopernicusDemData&) = delete;

    [[nodiscard]] static std::shared_ptr<CopernicusDemData> Prepare(
        const CopernicusDemOptions& options, std::string& error);

    [[nodiscard]] std::optional<double> Sample(const LonLat& lonLat) const;

    [[nodiscard]] double GetMinimumElevationMeters() const;

    [[nodiscard]] const CopernicusDemBounds& GetBounds() const;

    [[nodiscard]] const std::vector<std::filesystem::path>& GetSourceFiles() const;

  private:
    class Impl;

    explicit CopernicusDemData(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};
} // namespace DownTool
