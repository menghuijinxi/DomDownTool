#pragma once

#include <string>
#include <vector>

#include "nlohmann/json_fwd.hpp"

#include "GeographicPolygon.h"

namespace DownTool
{
enum class DownloadRegionRole
{
    Include,
    ExcludeGis
};

class DownloadRegion
{
  public:
    const std::string& GetId() const { return id_; }
    void SetId(const std::string& value) { id_ = value; }
    const std::string& GetName() const { return name_; }
    void SetName(const std::string& value) { name_ = value; }
    const std::string& GetSource() const { return source_; }
    void SetSource(const std::string& value) { source_ = value; }
    DownloadRegionRole GetRole() const { return role_; }
    void SetRole(DownloadRegionRole value) { role_ = value; }
    bool GetEnabled() const { return enabled_; }
    void SetEnabled(bool value) { enabled_ = value; }
    const Wgs84Ring& GetVertices() const { return vertices_; }
    void SetVertices(const Wgs84Ring& value) { vertices_ = value; }

  protected:
    std::string id_;
    std::string name_;
    std::string source_;
    DownloadRegionRole role_ = DownloadRegionRole::Include;
    bool enabled_ = true;
    Wgs84Ring vertices_;
};

using DownloadRegions = std::vector<DownloadRegion>;

[[nodiscard]] Wgs84Polygons GetRegionPolygons(const DownloadRegions& regions,
                                              DownloadRegionRole role);

[[nodiscard]] bool ValidateDownloadRegions(const DownloadRegions& regions, std::string& error);

[[nodiscard]] nlohmann::json DownloadRegionsToJson(const DownloadRegions& regions);

[[nodiscard]] bool ReadDownloadRegions(const nlohmann::json& value, DownloadRegions& regions,
                                       std::string& error);

[[nodiscard]] DownloadRegions MakeDownloadRegions(const Wgs84Polygons& polygons,
                                                  const std::string& source = {});
} // namespace DownTool
