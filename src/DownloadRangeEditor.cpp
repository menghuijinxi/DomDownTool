#include "preview/DownloadRangeEditor.h"

#include <algorithm>
#include <cmath>
#include <numbers>
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

std::optional<double> DomDownToolPreview::DownloadRangeEditor::GetSelectedSquareRadiusMeters() const
{
    const auto* region = GetSelectedRegion();
    if (!region || region->GetRole() != DownTool::DownloadRegionRole::Include)
    {
        return std::nullopt;
    }
    const auto& vertices = region->GetVertices();
    const bool closed = vertices.size() == 5 && vertices.front().lon == vertices.back().lon &&
                        vertices.front().lat == vertices.back().lat;
    if (vertices.size() != 4 && !closed)
    {
        return std::nullopt;
    }
    const auto bounds = DownTool::CalculateGeographicBounds({vertices});
    if (!bounds || bounds->west >= bounds->east || bounds->south >= bounds->north)
    {
        return std::nullopt;
    }
    for (const auto& vertex : vertices)
    {
        if ((std::abs(vertex.lon - bounds->west) > 1.e-9 &&
             std::abs(vertex.lon - bounds->east) > 1.e-9) ||
            (std::abs(vertex.lat - bounds->south) > 1.e-9 &&
             std::abs(vertex.lat - bounds->north) > 1.e-9))
        {
            return std::nullopt;
        }
    }
    const auto center = DownTool::GeographicBoundsCenter(*bounds);
    const double halfHeight = (bounds->north - bounds->south) * 0.5;
    const double halfWidth =
        (bounds->east - bounds->west) * 0.5 * std::cos(center.lat * std::numbers::pi / 180.);
    if (std::abs(halfWidth - halfHeight) > halfHeight * 0.001)
    {
        return std::nullopt;
    }
    // 沿用预览方框的球面半边长定义，避免旧半径区域转换后数值跳变。
    constexpr double kEarthRadiusMeters = 6378137.;
    return halfHeight * std::numbers::pi / 180. * kEarthRadiusMeters;
}

bool DomDownToolPreview::DownloadRangeEditor::SetSelectedSquareRadiusMeters(double radiusMeters,
                                                                            std::string& error)
{
    const auto currentRadius = GetSelectedSquareRadiusMeters();
    if (!currentRadius)
    {
        error = "请选择一个正方形下载区域。";
        return false;
    }
    if (!std::isfinite(radiusMeters) || radiusMeters <= 0.)
    {
        error = "半径必须是大于 0 的有效数值。";
        return false;
    }
    if (std::abs(radiusMeters - *currentRadius) < 1.e-6)
    {
        return true;
    }
    auto vertices = GetSelectedRegion()->GetVertices();
    const auto bounds = DownTool::CalculateGeographicBounds({vertices});
    const auto center = DownTool::GeographicBoundsCenter(*bounds);
    const double scale = radiusMeters / *currentRadius;
    for (auto& vertex : vertices)
    {
        vertex.lon = center.lon + (vertex.lon - center.lon) * scale;
        vertex.lat = center.lat + (vertex.lat - center.lat) * scale;
        if (!std::isfinite(vertex.lon) || !std::isfinite(vertex.lat) || vertex.lon < -180. ||
            vertex.lon > 180. || std::abs(vertex.lat) > 85.05112878)
        {
            error = "半径过大，区域超出地图支持的经纬度范围。";
            return false;
        }
    }
    return SetSelectedVertices(vertices, error);
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
    const std::string prefix = role == DownTool::DownloadRegionRole::Include      ? "下载区 "
                               : role == DownTool::DownloadRegionRole::ExcludeGis ? "GIS 排除区 "
                               : role == DownTool::DownloadRegionRole::ExcludeTerrain
                                   ? "底板排除区 "
                                   : "平整区域 ";
    region.SetName(prefix + std::to_string(count + 1));
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

bool DomDownToolPreview::DownloadRangeEditor::CopySelectedAs(DownTool::DownloadRegionRole role,
                                                             std::string& error)
{
    const auto* selected = GetSelectedRegion();
    if (!selected)
    {
        error = "请选择要复制的区域。";
        return false;
    }
    auto next = regions_;
    auto copy = *selected;
    copy.SetId(NewId(next));
    copy.SetName(selected->GetName() + "（" + DownTool::DownloadRegionRoleName(role) + "）");
    copy.SetRole(role);
    copy.SetEnabled(true);
    next.push_back(std::move(copy));
    // 保持原区域选中，后续编辑不会误改刚复制的区域。
    return Commit(std::move(next), error);
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
