#pragma once

#include "CoreMinimal.h"

// 兼容任务 1790567037646-0 的旧平面网格；返回网格共同原点下的厘米坐标。
class FDomDownLegacyPlaneCoordinates
{
public:
    // 仅投影到导入网格的局部 XY（厘米），供调用方通过射线获取 Z；无需加载数据。
    bool TryLongitudeLatitudeToUEXY(double LongitudeDegrees, double LatitudeDegrees,
                                    FVector2D& OutPosition) const;

    bool LoadSurface(const FString& Filename, FString& OutError);

    // 只有经纬度时，从原三角面插值得到 Z；范围外、网格孔洞或未加载时返回 false。
    bool TryLongitudeLatitudeToUE(double LongitudeDegrees, double LatitudeDegrees,
                                  FVector& OutLocation) const;

    // 已知海拔时保留旧导出的最低点归零与负值截平逻辑，不进行地面吸附。
    bool TryLongitudeLatitudeAltitudeToUE(const FVector3d& LongitudeLatitudeAltitudeMeters,
                                          FVector& OutLocation) const;

    double GetOriginLongitude() const
    {
        return OriginLongitude_;
    }
    double GetOriginLatitude() const
    {
        return OriginLatitude_;
    }
    double GetReferenceElevationMeters() const
    {
        return ReferenceElevationMeters_;
    }
    bool IsSurfaceLoaded() const
    {
        return !TriangleVertices_.IsEmpty();
    }

protected:
    void BuildSpatialIndex();
    FIntPoint GetCell(const FVector2D& Position) const;
    bool TrySurfaceHeight(const FVector2D& Position, double& OutHeightCm) const;

    double OriginLongitude_ = 113.144184;
    double OriginLatitude_ = 22.186737;
    double ReferenceElevationMeters_ = -46.816097259521484;
    TArray<FVector3f> TriangleVertices_;
    FBox2D SurfaceBounds_{ForceInit};
    FIntPoint CellCounts_ = FIntPoint::ZeroValue;
    TArray<TArray<int32>> CellTriangles_;
};
