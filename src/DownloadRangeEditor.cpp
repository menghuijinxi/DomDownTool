#include "preview/DownloadRangeEditor.h"

#include <algorithm>
#include <utility>

void DomDownToolPreview::DownloadRangeEditor::SetRegions(const DownTool::DownloadRegions& value)
{
    regions_ = value;
    selectedId_.clear();
    undo_.clear();
    redo_.clear();
}

const DownTool::DownloadRegion* DomDownToolPreview::DownloadRangeEditor::GetSelectedRegion() const
{
    const auto selected =
        std::ranges::find(regions_, selectedId_, &DownTool::DownloadRegion::GetId);
    return selected == regions_.end() ? nullptr : &*selected;
}

std::string DomDownToolPreview::DownloadRangeEditor::NewId(const DownTool::DownloadRegions& regions)
{
    std::string id;
    do
    {
        id = "region-" + std::to_string(serial_++);
    } while (std::ranges::find(regions, id, &DownTool::DownloadRegion::GetId) != regions.end());
    return id;
}

bool DomDownToolPreview::DownloadRangeEditor::Commit(DownTool::DownloadRegions regions,
                                                     std::string& error)
{
    if (!DownTool::ValidateDownloadRegions(regions, error))
    {
        return false;
    }
    undo_.push_back(regions_);
    if (undo_.size() > 100)
    {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
    regions_ = std::move(regions);
    RestoreSelection();
    return true;
}

bool DomDownToolPreview::DownloadRangeEditor::AddRegion(const DownTool::Wgs84Ring& vertices,
                                                        DownTool::DownloadRegionRole role,
                                                        std::string& error)
{
    auto next = regions_;
    DownTool::DownloadRegion region;
    region.SetId(NewId(next));
    const auto count = std::ranges::count(next, role, &DownTool::DownloadRegion::GetRole);
    region.SetName((role == DownTool::DownloadRegionRole::Include ? "下载区 " : "GIS 排除区 ") +
                   std::to_string(count + 1));
    region.SetRole(role);
    region.SetVertices(vertices);
    next.push_back(region);
    if (!Commit(std::move(next), error))
    {
        return false;
    }
    selectedId_ = region.GetId();
    return true;
}

bool DomDownToolPreview::DownloadRangeEditor::AppendPolygons(
    const DownTool::Wgs84Polygons& polygons, const std::string& source, std::string& error)
{
    auto next = regions_;
    for (auto region : DownTool::MakeDownloadRegions(polygons, source))
    {
        region.SetId(NewId(next));
        next.push_back(std::move(region));
    }
    return Commit(std::move(next), error);
}

bool DomDownToolPreview::DownloadRangeEditor::SetSelectedVertices(
    const DownTool::Wgs84Ring& vertices, std::string& error)
{
    auto next = regions_;
    for (auto& region : next)
    {
        if (region.GetId() == selectedId_)
        {
            region.SetVertices(vertices);
            return Commit(std::move(next), error);
        }
    }
    error = "请先选择一个区域。";
    return false;
}

bool DomDownToolPreview::DownloadRangeEditor::SetSelectedName(const std::string& name,
                                                              std::string& error)
{
    auto next = regions_;
    for (auto& region : next)
    {
        if (region.GetId() == selectedId_)
        {
            region.SetName(name);
            return Commit(std::move(next), error);
        }
    }
    return false;
}

bool DomDownToolPreview::DownloadRangeEditor::SetSelectedEnabled(bool enabled, std::string& error)
{
    auto next = regions_;
    for (auto& region : next)
    {
        if (region.GetId() == selectedId_)
        {
            region.SetEnabled(enabled);
            return Commit(std::move(next), error);
        }
    }
    return false;
}

bool DomDownToolPreview::DownloadRangeEditor::RemoveSelected(std::string& error)
{
    auto next = regions_;
    std::erase_if(next, [this](const auto& region) { return region.GetId() == selectedId_; });
    return next.size() != regions_.size() && Commit(std::move(next), error);
}

void DomDownToolPreview::DownloadRangeEditor::RestoreSelection()
{
    if (!GetSelectedRegion())
    {
        selectedId_.clear();
    }
}

void DomDownToolPreview::DownloadRangeEditor::Undo()
{
    if (!undo_.empty())
    {
        redo_.push_back(regions_);
        regions_ = std::move(undo_.back());
        undo_.pop_back();
        RestoreSelection();
    }
}

void DomDownToolPreview::DownloadRangeEditor::Redo()
{
    if (!redo_.empty())
    {
        undo_.push_back(regions_);
        regions_ = std::move(redo_.back());
        redo_.pop_back();
        RestoreSelection();
    }
}
