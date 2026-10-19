#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DownloadRegion.h"

namespace DomDownToolPreview
{
class DownloadRangeEditor
{
  public:
    const DownTool::DownloadRegions& GetRegions() const { return regions_; }
    void SetRegions(const DownTool::DownloadRegions& value);
    const std::string& GetSelectedId() const { return selectedId_; }
    void SetSelectedId(const std::string& value) { selectedId_ = value; }
    const DownTool::DownloadRegion* GetSelectedRegion() const;
    bool GetCanUndo() const { return !undo_.empty(); }
    bool GetCanRedo() const { return !redo_.empty(); }

    bool AddRegion(const DownTool::Wgs84Ring& vertices, DownTool::DownloadRegionRole role,
                   std::string& error);
    bool AppendPolygons(const DownTool::Wgs84Polygons& polygons, const std::string& source,
                        std::string& error);
    bool SetSelectedVertices(const DownTool::Wgs84Ring& vertices, std::string& error);
    bool SetSelectedName(const std::string& name, std::string& error);
    bool SetSelectedEnabled(bool enabled, std::string& error);
    bool RemoveSelected(std::string& error);
    void Undo();
    void Redo();

  protected:
    bool Commit(DownTool::DownloadRegions regions, std::string& error);
    std::string NewId(const DownTool::DownloadRegions& regions);
    void RestoreSelection();

    DownTool::DownloadRegions regions_;
    std::string selectedId_;
    std::vector<DownTool::DownloadRegions> undo_;
    std::vector<DownTool::DownloadRegions> redo_;
    uint64_t serial_ = 1;
};
} // namespace DomDownToolPreview
