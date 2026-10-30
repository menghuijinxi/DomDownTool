#include "DomDownLegacyPlaneCoordinates.h"

#include <cmath>

#include "Misc/FileHelper.h"
#include "Serialization/MemoryReader.h"

namespace
{
    constexpr double EarthRadiusMeters = 6378137.;
    constexpr double DegreesToRadians = UE_DOUBLE_PI / 180.;
    constexpr double MaximumMercatorLatitude = 85.0511287798066;
    constexpr double CellSizeCm = 25000.;
    constexpr uint32 SurfaceMagic = 0x314c5044;
    constexpr int64 HeaderBytes = 36;
    constexpr int64 TriangleBytes = 36;

    double MercatorY(double LatitudeDegrees)
    {
        return EarthRadiusMeters *
               std::log(std::tan(UE_DOUBLE_PI * .25 + LatitudeDegrees * DegreesToRadians * .5));
    }

    double Cross2D(const FVector2D& First, const FVector2D& Second)
    {
        return First.X * Second.Y - First.Y * Second.X;
    }
} // namespace

bool FDomDownLegacyPlaneCoordinates::TryLongitudeLatitudeToUEXY(double LongitudeDegrees,
                                                                double LatitudeDegrees,
                                                                FVector2D& OutPosition) const
{
    if (!FMath::IsFinite(LongitudeDegrees) || !FMath::IsFinite(LatitudeDegrees) ||
        LongitudeDegrees < -180. || LongitudeDegrees > 180. ||
        FMath::Abs(LatitudeDegrees) > MaximumMercatorLatitude)
    {
        return false;
    }
    // 导入器已经翻转 FBX 的 Y；UE 中东为 +X、南为 +Y，单位为厘米。
    OutPosition = FVector2D(EarthRadiusMeters * (LongitudeDegrees - OriginLongitude_) *
                                DegreesToRadians * 100.,
                            (MercatorY(OriginLatitude_) - MercatorY(LatitudeDegrees)) * 100.);
    return true;
}

bool FDomDownLegacyPlaneCoordinates::TryLongitudeLatitudeAltitudeToUE(
    const FVector3d& LongitudeLatitudeAltitudeMeters, FVector& OutLocation) const
{
    FVector2D Horizontal;
    if (!FMath::IsFinite(LongitudeLatitudeAltitudeMeters.Z) ||
        !TryLongitudeLatitudeToUEXY(LongitudeLatitudeAltitudeMeters.X,
                                    LongitudeLatitudeAltitudeMeters.Y, Horizontal))
    {
        return false;
    }
    OutLocation = FVector(
        Horizontal.X, Horizontal.Y,
        FMath::Max(0., LongitudeLatitudeAltitudeMeters.Z - ReferenceElevationMeters_) * 100.);
    return true;
}

bool FDomDownLegacyPlaneCoordinates::LoadSurface(const FString& Filename, FString& OutError)
{
    OutError.Reset();
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename) || Bytes.Num() < HeaderBytes)
    {
        OutError = TEXT("Cannot read the legacy plane surface file.");
        return false;
    }
    FMemoryReader Reader(Bytes, true);
    uint32 Magic = 0;
    uint32 Version = 0;
    uint32 TriangleCount = 0;
    FDomDownLegacyPlaneCoordinates Loaded;
    Reader << Magic << Version;
    Reader << Loaded.OriginLongitude_ << Loaded.OriginLatitude_;
    Reader << Loaded.ReferenceElevationMeters_ << TriangleCount;
    if (Magic != SurfaceMagic || Version != 1 || TriangleCount == 0 ||
        int64(Bytes.Num()) != HeaderBytes + TriangleBytes * TriangleCount ||
        !FMath::IsFinite(Loaded.ReferenceElevationMeters_))
    {
        OutError = TEXT("Invalid legacy plane surface header or length.");
        return false;
    }
    FVector2D OriginCheck;
    if (!Loaded.TryLongitudeLatitudeToUEXY(Loaded.OriginLongitude_, Loaded.OriginLatitude_,
                                           OriginCheck))
    {
        OutError = TEXT("Invalid legacy plane surface origin.");
        return false;
    }
    Loaded.TriangleVertices_.SetNumUninitialized(int32(TriangleCount * 3));
    for (FVector3f& Position : Loaded.TriangleVertices_)
    {
        Reader << Position.X << Position.Y << Position.Z;
        if (!FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y) ||
            !FMath::IsFinite(Position.Z))
        {
            OutError = TEXT("Non-finite vertex in the legacy plane surface.");
            return false;
        }
        Loaded.SurfaceBounds_ += FVector2D(Position.X, Position.Y);
    }
    const FVector2D Extent = Loaded.SurfaceBounds_.GetSize();
    if (Extent.X <= 0. || Extent.Y <= 0. || Extent.X > 50000000. || Extent.Y > 50000000.)
    {
        OutError = TEXT("Unsupported legacy plane surface extent.");
        return false;
    }
    Loaded.BuildSpatialIndex();
    *this = MoveTemp(Loaded);
    return true;
}

FIntPoint FDomDownLegacyPlaneCoordinates::GetCell(const FVector2D& Position) const
{
    const FVector2D Relative = (Position - SurfaceBounds_.Min) / CellSizeCm;
    return FIntPoint(FMath::Clamp(FMath::FloorToInt(Relative.X), 0, CellCounts_.X - 1),
                     FMath::Clamp(FMath::FloorToInt(Relative.Y), 0, CellCounts_.Y - 1));
}

void FDomDownLegacyPlaneCoordinates::BuildSpatialIndex()
{
    const FVector2D Extent = SurfaceBounds_.GetSize();
    CellCounts_ = FIntPoint(FMath::Max(1, FMath::CeilToInt(Extent.X / CellSizeCm)),
                            FMath::Max(1, FMath::CeilToInt(Extent.Y / CellSizeCm)));
    CellTriangles_.SetNum(CellCounts_.X * CellCounts_.Y);
    for (int32 Triangle = 0; Triangle < TriangleVertices_.Num() / 3; ++Triangle)
    {
        FBox2D Bounds(ForceInit);
        for (int32 Corner = 0; Corner < 3; ++Corner)
        {
            const FVector3f& Position = TriangleVertices_[Triangle * 3 + Corner];
            Bounds += FVector2D(Position.X, Position.Y);
        }
        const FIntPoint First = GetCell(Bounds.Min);
        const FIntPoint Last = GetCell(Bounds.Max);
        for (int32 Y = First.Y; Y <= Last.Y; ++Y)
        {
            for (int32 X = First.X; X <= Last.X; ++X)
            {
                CellTriangles_[Y * CellCounts_.X + X].Add(Triangle);
            }
        }
    }
}

bool FDomDownLegacyPlaneCoordinates::TrySurfaceHeight(const FVector2D& Position,
                                                      double& OutHeightCm) const
{
    // 只容忍双精度投影往返带来的亚毫米边界舍入，不跨孔洞寻找远处地面。
    constexpr double BoundaryToleranceCm = .001;
    if (Position.X < SurfaceBounds_.Min.X - BoundaryToleranceCm ||
        Position.X > SurfaceBounds_.Max.X + BoundaryToleranceCm ||
        Position.Y < SurfaceBounds_.Min.Y - BoundaryToleranceCm ||
        Position.Y > SurfaceBounds_.Max.Y + BoundaryToleranceCm)
    {
        return false;
    }
    const FIntPoint Cell = GetCell(Position);
    for (const int32 Triangle : CellTriangles_[Cell.Y * CellCounts_.X + Cell.X])
    {
        const FVector3f& A = TriangleVertices_[Triangle * 3];
        const FVector3f& B = TriangleVertices_[Triangle * 3 + 1];
        const FVector3f& C = TriangleVertices_[Triangle * 3 + 2];
        const FVector2D AB(double(B.X) - A.X, double(B.Y) - A.Y);
        const FVector2D AC(double(C.X) - A.X, double(C.Y) - A.Y);
        const double Determinant = Cross2D(AB, AC);
        if (FMath::Abs(Determinant) < 1.e-12)
        {
            continue;
        }
        const FVector2D AP(Position.X - A.X, Position.Y - A.Y);
        const double WeightB = Cross2D(AP, AC) / Determinant;
        const double WeightC = Cross2D(AB, AP) / Determinant;
        const double WeightA = 1. - WeightB - WeightC;
        constexpr double WeightTolerance = 1.e-7;
        if (WeightA >= -WeightTolerance && WeightB >= -WeightTolerance &&
            WeightC >= -WeightTolerance)
        {
            OutHeightCm = WeightA * A.Z + WeightB * B.Z + WeightC * C.Z;
            return true;
        }
    }
    return false;
}

bool FDomDownLegacyPlaneCoordinates::TryLongitudeLatitudeToUE(double LongitudeDegrees,
                                                              double LatitudeDegrees,
                                                              FVector& OutLocation) const
{
    FVector2D Horizontal;
    double HeightCm = 0.;
    if (!IsSurfaceLoaded() ||
        !TryLongitudeLatitudeToUEXY(LongitudeDegrees, LatitudeDegrees, Horizontal) ||
        !TrySurfaceHeight(Horizontal, HeightCm))
    {
        return false;
    }
    OutLocation = FVector(Horizontal.X, Horizontal.Y, HeightCm);
    return true;
}
