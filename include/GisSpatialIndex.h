#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <vector>

namespace DownTool
{
class GisSpatialBounds
{
  public:
    GisSpatialBounds() = default;
    GisSpatialBounds(double west, double south, double east, double north);

    double GetWest() const { return west_; }
    double GetSouth() const { return south_; }
    double GetEast() const { return east_; }
    double GetNorth() const { return north_; }
    void Add(double x, double y);
    bool IsValid() const;
    bool Contains(const GisSpatialBounds& other) const;
    bool Intersects(const GisSpatialBounds& other) const;

  protected:
    double west_ = std::numeric_limits<double>::infinity();
    double south_ = std::numeric_limits<double>::infinity();
    double east_ = -std::numeric_limits<double>::infinity();
    double north_ = -std::numeric_limits<double>::infinity();
};

// 索引拥有包围盒副本，查询返回输入位置；跨象限要素留在父节点，单次查询不重复返回。
class GisSpatialIndex
{
  public:
    bool Build(std::vector<GisSpatialBounds> bounds,
               const std::function<bool()>& cancellationRequested = {});
    bool Query(const GisSpatialBounds& bounds, std::vector<size_t>& matches,
               const std::function<bool()>& cancellationRequested = {}) const;
    const GisSpatialBounds& GetBounds(size_t index) const { return bounds_[index]; }
    size_t GetSize() const { return bounds_.size(); }

  private:
    static constexpr size_t kNoChild = std::numeric_limits<size_t>::max();
    struct Node
    {
        GisSpatialBounds bounds;
        std::vector<size_t> entries;
        std::array<size_t, 4> children = {kNoChild, kNoChild, kNoChild, kNoChild};
    };

    bool insert(size_t nodeIndex, size_t entryIndex, size_t depth,
                const std::function<bool()>& cancellationRequested);
    bool subdivide(size_t nodeIndex, size_t depth,
                   const std::function<bool()>& cancellationRequested);

    std::vector<GisSpatialBounds> bounds_;
    std::vector<Node> nodes_;
};
} // namespace DownTool
