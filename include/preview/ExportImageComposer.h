#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace DomDownToolPreview
{
using ExportCancellationCallback = std::function<bool()>;

class ExportImageComposer
{
  public:
    static bool CopyDirectory(const std::filesystem::path& source,
                              const std::filesystem::path& destination,
                              std::string& error,
                              const ExportCancellationCallback& cancellationRequested = {});

    static bool CompositeImage(const std::filesystem::path& basePath,
                               const std::filesystem::path& overlayPath,
                               const std::filesystem::path& outputPath,
                               std::string& error,
                               const ExportCancellationCallback& cancellationRequested = {});

    static bool CompositeTileTree(const std::filesystem::path& baseRoot,
                                  const std::filesystem::path& overlayRoot,
                                  const std::filesystem::path& outputRoot,
                                  std::string& error,
                                  const ExportCancellationCallback& cancellationRequested = {});

    static bool CompositeXyzTileTree(const std::filesystem::path& baseRoot,
                                     const std::filesystem::path& overlayRoot,
                                     const std::filesystem::path& outputRoot,
                                     std::string& error,
                                     const ExportCancellationCallback& cancellationRequested = {});
};
} // namespace DomDownToolPreview
