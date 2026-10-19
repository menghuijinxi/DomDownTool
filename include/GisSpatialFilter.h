#pragma once

#include <functional>
#include <string>

#include "AmapVectorDecoder.h"
#include "GeographicPolygon.h"

namespace DownTool
{
class GisSpatialRange
{
  public:
    const Wgs84Polygons& GetIncludedPolygons() const { return includedPolygons_; }
    void SetIncludedPolygons(const Wgs84Polygons& value) { includedPolygons_ = value; }
    const Wgs84Polygons& GetExcludedPolygons() const { return excludedPolygons_; }
    void SetExcludedPolygons(const Wgs84Polygons& value) { excludedPolygons_ = value; }
    bool IsEmpty() const { return includedPolygons_.empty() && excludedPolygons_.empty(); }

  protected:
    Wgs84Polygons includedPolygons_;
    Wgs84Polygons excludedPolygons_;
};

// 仅处理解码后的 GIS 副本；原始 PBF、卫星图和高程数据不受影响。
[[nodiscard]] bool ApplyGisSpatialFilter(AmapVectorDecodedData& decodedData,
                                         const GisSpatialRange& range, std::string& error,
                                         const std::function<bool()>& cancellationRequested = {});
} // namespace DownTool
