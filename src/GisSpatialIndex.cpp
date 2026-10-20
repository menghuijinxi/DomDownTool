#include "GisSpatialIndex.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace
{
constexpr size_t kNodeCapacity = 64;
constexpr size_t kMaximumDepth = 12;

bool canceled(const std::function<bool()>& cancellationRequested)
{
    return cancellationRequested && cancellationRequested();
}
} // namespace

DownTool::GisSpatialBounds::GisSpatialBounds(double west, double south, double east, double north)
    : west_(west), south_(south), east_(east), north_(north)
{
}

void DownTool::GisSpatialBounds::Add(double x, double y)
{
    west_ = std::min(west_, x);
    south_ = std::min(south_, y);
    east_ = std::max(east_, x);
    north_ = std::max(north_, y);
}

bool DownTool::GisSpatialBounds::IsValid() const
{
    return std::isfinite(west_) && std::isfinite(south_) && std::isfinite(east_) &&
           std::isfinite(north_) && west_ <= east_ && south_ <= north_;
}

bool DownTool::GisSpatialBounds::Contains(const GisSpatialBounds& other) const
{
    return west_ <= other.west_ && south_ <= other.south_ && east_ >= other.east_ &&
           north_ >= other.north_;
}

bool DownTool::GisSpatialBounds::Intersects(const GisSpatialBounds& other) const
{
    return west_ <= other.east_ && south_ <= other.north_ && east_ >= other.west_ &&
           north_ >= other.south_;
}

bool DownTool::GisSpatialIndex::Build(std::vector<GisSpatialBounds> bounds,
                                      const std::function<bool()>& cancellationRequested)
{
    nodes_.clear();
    bounds_ = std::move(bounds);
    GisSpatialBounds rootBounds;
    for (size_t index = 0; index < bounds_.size(); ++index)
    {
        if (index % 256 == 0 && canceled(cancellationRequested))
        {
            bounds_.clear();
            return false;
        }
        if (bounds_[index].IsValid())
        {
            rootBounds.Add(bounds_[index].GetWest(), bounds_[index].GetSouth());
            rootBounds.Add(bounds_[index].GetEast(), bounds_[index].GetNorth());
        }
    }
    if (rootBounds.IsValid())
    {
        nodes_.push_back(Node{rootBounds});
        for (size_t index = 0; index < bounds_.size(); ++index)
        {
            if ((index % 256 == 0 && canceled(cancellationRequested)) ||
                (bounds_[index].IsValid() && !insert(0, index, 0, cancellationRequested)))
            {
                nodes_.clear();
                bounds_.clear();
                return false;
            }
        }
    }
    if (canceled(cancellationRequested))
    {
        nodes_.clear();
        bounds_.clear();
        return false;
    }
    return true;
}

bool DownTool::GisSpatialIndex::subdivide(size_t nodeIndex, size_t depth,
                                          const std::function<bool()>& cancellationRequested)
{
    if (canceled(cancellationRequested))
    {
        return false;
    }
    const auto bounds = nodes_[nodeIndex].bounds;
    const double centerX = bounds.GetWest() + (bounds.GetEast() - bounds.GetWest()) / 2.;
    const double centerY = bounds.GetSouth() + (bounds.GetNorth() - bounds.GetSouth()) / 2.;
    for (size_t quadrant = 0; quadrant < 4; ++quadrant)
    {
        const GisSpatialBounds childBounds{quadrant % 2 == 0 ? bounds.GetWest() : centerX,
                                           quadrant < 2 ? bounds.GetSouth() : centerY,
                                           quadrant % 2 == 0 ? centerX : bounds.GetEast(),
                                           quadrant < 2 ? centerY : bounds.GetNorth()};
        const size_t childIndex = nodes_.size();
        nodes_.push_back(Node{childBounds});
        nodes_[nodeIndex].children[quadrant] = childIndex;
    }
    auto previousEntries = std::move(nodes_[nodeIndex].entries);
    for (const size_t entryIndex : previousEntries)
    {
        if (!insert(nodeIndex, entryIndex, depth, cancellationRequested))
        {
            return false;
        }
    }
    return true;
}

bool DownTool::GisSpatialIndex::insert(size_t nodeIndex, size_t entryIndex, size_t depth,
                                       const std::function<bool()>& cancellationRequested)
{
    const auto bounds = nodes_[nodeIndex].bounds;
    if (nodes_[nodeIndex].children.front() == kNoChild &&
        nodes_[nodeIndex].entries.size() == kNodeCapacity && depth < kMaximumDepth &&
        bounds.GetWest() < bounds.GetEast() && bounds.GetSouth() < bounds.GetNorth())
    {
        if (!subdivide(nodeIndex, depth, cancellationRequested))
        {
            return false;
        }
    }
    if (nodes_[nodeIndex].children.front() != kNoChild)
    {
        const auto children = nodes_[nodeIndex].children;
        for (const size_t childIndex : children)
        {
            if (nodes_[childIndex].bounds.Contains(bounds_[entryIndex]))
            {
                return insert(childIndex, entryIndex, depth + 1, cancellationRequested);
            }
        }
    }
    nodes_[nodeIndex].entries.push_back(entryIndex);
    return true;
}

bool DownTool::GisSpatialIndex::Query(const GisSpatialBounds& bounds, std::vector<size_t>& matches,
                                      const std::function<bool()>& cancellationRequested) const
{
    matches.clear();
    if (canceled(cancellationRequested))
    {
        return false;
    }
    std::vector<size_t> pending;
    if (!nodes_.empty() && bounds.IsValid())
    {
        pending.push_back(0);
    }
    size_t visited = 0;
    while (!pending.empty())
    {
        const size_t nodeIndex = pending.back();
        pending.pop_back();
        if (++visited % 256 == 0 && canceled(cancellationRequested))
        {
            matches.clear();
            return false;
        }
        const Node& node = nodes_[nodeIndex];
        if (!node.bounds.Intersects(bounds))
        {
            continue;
        }
        for (const size_t entryIndex : node.entries)
        {
            if (++visited % 256 == 0 && canceled(cancellationRequested))
            {
                matches.clear();
                return false;
            }
            if (bounds_[entryIndex].Intersects(bounds))
            {
                matches.push_back(entryIndex);
            }
        }
        for (const size_t childIndex : node.children)
        {
            if (childIndex != kNoChild)
            {
                pending.push_back(childIndex);
            }
        }
    }
    if (canceled(cancellationRequested))
    {
        matches.clear();
        return false;
    }
    return true;
}
