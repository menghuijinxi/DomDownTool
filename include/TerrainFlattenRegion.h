#pragma once

#include "GeographicPolygon.h"

namespace DownTool
{
class TerrainFlattenRegion
{
  public:
    TerrainFlattenRegion(const Wgs84Ring& polygon, double elevationMeters)
        : polygon_(polygon), elevationMeters_(elevationMeters)
    {
    }

    const Wgs84Ring& GetPolygon() const { return polygon_; }
    double GetElevationMeters() const { return elevationMeters_; }

  protected:
    Wgs84Ring polygon_;
    double elevationMeters_ = 0.;
};
} // namespace DownTool
