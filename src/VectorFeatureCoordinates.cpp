#include "AmapVectorDecoder.h"

#include "GCJ02GoogleDownBase.h"

DownTool::LonLat DownTool::VectorCoordinateWgs84(const AmapVectorFeature& feature,
                                                 const AmapVectorCoordinate& coordinate)
{
    const LonLat position{coordinate.longitude, coordinate.latitude};
    return feature.GetCoordinateSystem() == VectorCoordinateSystem::Wgs84
               ? position
               : GCJ02GoogleDownBase::GCJ02ToWGS84(position);
}

double DownTool::VectorBuildingHeightCentimeters(const AmapVectorFeature& feature,
                                                 double layerHeightCentimeters)
{
    return feature.GetHeightMeters() ? *feature.GetHeightMeters() * 100.
                                     : double(feature.height) * layerHeightCentimeters;
}

bool DownTool::HasVectorBuildingHeight(const AmapVectorFeature& feature)
{
    return feature.GetHeightMeters() ? *feature.GetHeightMeters() > 0.
                                     : feature.hasHeight && feature.height != 0;
}
