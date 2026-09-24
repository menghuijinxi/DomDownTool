#include "AmapVectorPngDownloader.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

#include "AmapVectorDecoder.h"
#include "AmapBuildingFbxExporter.h"
#include "GCJ02GoogleDownBase.h"

namespace
{
    using DownTool::AmapVectorColor;
    using DownTool::AmapVectorDecodedData;
    using DownTool::AmapVectorFeature;
    using DownTool::AmapVectorGeometryType;
    using DownTool::AmapVectorRenderCategory;
    using DownTool::AmapVectorRenderOptions;
    using DownTool::Tile;

    constexpr uint32_t kRenderScale = 2;

    std::vector<DownTool::AmapVectorTile> SelectSourceTilesForPngOutput(
        const DownTool::AmapVectorDownloadOptions& options)
    {
        if (!std::isfinite(options.centerWgs84.lon) ||
            !std::isfinite(options.centerWgs84.lat) ||
            options.centerWgs84.lon < -180. || options.centerWgs84.lon > 180. ||
            options.centerWgs84.lat < -90. || options.centerWgs84.lat > 90.)
        {
            throw std::invalid_argument("高德矢量瓦片坐标必须是有效的经纬度。");
        }
        if (!std::isfinite(options.radiusMeters) || options.radiusMeters < 0.)
        {
            throw std::invalid_argument("高德矢量下载半径必须是非负有限米数。");
        }

        const uint32_t dataZoom =
            DownTool::AmapVectorTileGrid::ResolveDataZoom(options.zoom);
        DownTool::GoogleDownBase outputGrid;
        outputGrid.SetCenterLonLat(options.centerWgs84);
        outputGrid.SetRadius(options.radiusMeters);
        outputGrid.SetZoom(options.zoom);
        outputGrid.CalculateDownRange();

        const Tile& minimumTile = outputGrid.GetMinTile();
        const Tile& maximumTile = outputGrid.GetMaxTile();
        const DownTool::LonLat northWestWgs84 =
            DownTool::GoogleDownBase::TileToLonLat(minimumTile);
        const DownTool::LonLat southEastWgs84 =
            DownTool::GoogleDownBase::TileToLonLat({
                maximumTile.X + 1,
                maximumTile.Y + 1,
                maximumTile.Zoom
            });
        return DownTool::AmapVectorTileGrid::SelectTilesForWgs84Bounds(
            northWestWgs84,
            southEastWgs84,
            dataZoom);
    }

    struct ProjectedFeature
    {
        const AmapVectorFeature* source = nullptr;
        AmapVectorRenderCategory category = AmapVectorRenderCategory::Other;
        AmapVectorColor color;
        std::vector<std::vector<cv::Point2d>> geometryParts;
        double minimumX = 0.;
        double maximumX = 0.;
        double minimumY = 0.;
        double maximumY = 0.;
    };

    size_t GetRenderWorkerCount(uint32_t configuredCount, size_t tileCount)
    {
        if (tileCount == 0)
        {
            return 0;
        }
        const uint32_t hardwareCount = std::max(1u, std::thread::hardware_concurrency());
        const uint32_t requestedCount = configuredCount == 0 ? hardwareCount :
            std::min(configuredCount, hardwareCount);
        return std::min<size_t>(tileCount, std::max(1u, requestedCount));
    }

    std::string PathToUtf8String(const std::filesystem::path& path)
    {
        const std::u8string utf8Path = path.u8string();
        std::string result;
        result.reserve(utf8Path.size());
        for (const char8_t character : utf8Path)
        {
            result.push_back(static_cast<char>(character));
        }
        return result;
    }

    bool IsAmapWaterFeatureCode(uint32_t mainKey, uint32_t subKey)
    {
        // 这些分类来自高德当前 JSAPI 的水系 styleMap；内部编码并非公开稳定协议。
        if (mainKey == 30001)
        {
            return subKey == 2 || subKey == 6 || subKey == 11 || subKey == 13;
        }
        if (mainKey == 20014)
        {
            return subKey == 1 || subKey == 2;
        }
        if (mainKey == 10002)
        {
            return subKey == 13 || subKey == 38;
        }
        return false;
    }

    bool IsAmapGreenFeatureCode(uint32_t mainKey, uint32_t subKey)
    {
        return mainKey == 30001 && subKey == 3;
    }

    bool HasFeatureNameContaining(const AmapVectorFeature& feature, std::string_view text)
    {
        return std::ranges::any_of(
            feature.names,
            [text](const DownTool::AmapVectorName& name)
            {
                return name.text.find(text) != std::string::npos;
            });
    }

    bool IsAmapHighwayFeature(const AmapVectorFeature& feature)
    {
        if (feature.layerName != "road")
        {
            return false;
        }
        if (feature.mainKey == 20001 || feature.mainKey == 20002)
        {
            return true;
        }
        return feature.mainKey == 20018 &&
            (HasFeatureNameContaining(feature, "高速") ||
             HasFeatureNameContaining(feature, "快速路"));
    }

    bool IsAmapBoundaryFeature(const AmapVectorFeature& feature)
    {
        return feature.layerName == "region-line" && feature.mainKey != 20010 &&
            feature.mainKey != 20014;
    }

    AmapVectorRenderCategory GetCategory(
        const AmapVectorFeature& feature,
        const AmapVectorRenderOptions& options)
    {
        const auto featureCodeIterator = options.featureCodeCategories.find(
            { feature.mainKey, feature.subKey });
        if (featureCodeIterator != options.featureCodeCategories.end())
        {
            return featureCodeIterator->second;
        }
        const auto mainKeyIterator = options.mainKeyCategories.find(feature.mainKey);
        if (mainKeyIterator != options.mainKeyCategories.end())
        {
            return mainKeyIterator->second;
        }
        const auto layerIterator = options.layerCategories.find(feature.layerName);
        if (layerIterator != options.layerCategories.end())
        {
            return layerIterator->second;
        }
        return ClassifyAmapVectorFeature(feature);
    }

    bool IsCategorySelected(
        AmapVectorRenderCategory category,
        const AmapVectorRenderOptions& options)
    {
        return options.categories.contains(AmapVectorRenderCategory::All) ||
            options.categories.contains(category);
    }

    AmapVectorColor GetBuildingHeightColor(uint64_t height)
    {
        // 137 与 360 互质，常见高度范围内每个整数高度会先落到不同色相。
        const uint64_t hueIndex = ((height % 360u) * 137u) % 360u;
        const uint64_t colorBand = height / 360u;
        const double hue = static_cast<double>(hueIndex);
        const double saturation = 0.72 + static_cast<double>(colorBand % 4u) * 0.07;
        const double value = 0.92 - static_cast<double>((colorBand / 4u) % 4u) * 0.05;
        const double chroma = value * saturation;
        const double hueSector = hue / 60.;
        const double secondary = chroma *
            (1. - std::abs(std::fmod(hueSector, 2.) - 1.));
        const int sector = static_cast<int>(hueSector);
        double red = 0.;
        double green = 0.;
        double blue = 0.;
        switch (sector)
        {
        case 0:
            red = chroma;
            green = secondary;
            break;
        case 1:
            red = secondary;
            green = chroma;
            break;
        case 2:
            green = chroma;
            blue = secondary;
            break;
        case 3:
            green = secondary;
            blue = chroma;
            break;
        case 4:
            red = secondary;
            blue = chroma;
            break;
        default:
            red = chroma;
            blue = secondary;
            break;
        }
        const double minimum = value - chroma;
        return {
            static_cast<uint8_t>(std::lround((red + minimum) * 255.)),
            static_cast<uint8_t>(std::lround((green + minimum) * 255.)),
            static_cast<uint8_t>(std::lround((blue + minimum) * 255.)),
            255};
    }

    AmapVectorColor GetColor(
        const AmapVectorFeature& feature,
        AmapVectorRenderCategory category,
        const AmapVectorRenderOptions& options)
    {
        const auto featureCodeIterator = options.featureCodeColors.find(
            { feature.mainKey, feature.subKey });
        if (featureCodeIterator != options.featureCodeColors.end())
        {
            return featureCodeIterator->second;
        }
        const auto mainKeyIterator = options.mainKeyColors.find(feature.mainKey);
        if (mainKeyIterator != options.mainKeyColors.end())
        {
            return mainKeyIterator->second;
        }
        if (category == AmapVectorRenderCategory::Building &&
            options.colorBuildingsByHeight && feature.hasHeight && feature.height != 0)
        {
            return GetBuildingHeightColor(feature.height);
        }
        const auto colorIterator = options.colors.find(category);
        return colorIterator == options.colors.end() ?
            GetDefaultAmapVectorRenderColor(category) : colorIterator->second;
    }

    int GetRenderPhase(const ProjectedFeature& feature)
    {
        if (feature.source->geometryType == AmapVectorGeometryType::Polygon)
        {
            return 0;
        }
        if (feature.source->geometryType == AmapVectorGeometryType::LineString)
        {
            return 1;
        }
        return 2;
    }

    double GetLineWidth(
        const ProjectedFeature& feature,
        const AmapVectorRenderOptions& options)
    {
        if (feature.category == AmapVectorRenderCategory::Road)
        {
            return options.roadLineWidth;
        }
        if (feature.category == AmapVectorRenderCategory::Subway)
        {
            return options.transitLineWidth;
        }
        return options.lineWidth;
    }

    cv::Scalar ToPremultipliedBgra(const AmapVectorColor& color)
    {
        const double alphaScale = static_cast<double>(color.alpha) / 255.;
        return cv::Scalar(
            color.blue * alphaScale,
            color.green * alphaScale,
            color.red * alphaScale,
            color.alpha);
    }

    void DownsampleRenderImage(
        const cv::Mat& renderImage,
        uint32_t tileSize,
        cv::Mat& outputImage)
    {
        cv::resize(
            renderImage,
            outputImage,
            cv::Size(static_cast<int>(tileSize), static_cast<int>(tileSize)),
            0.,
            0.,
            cv::INTER_AREA);
        for (int row = 0; row < outputImage.rows; ++row)
        {
            cv::Vec4b* pixels = outputImage.ptr<cv::Vec4b>(row);
            for (int column = 0; column < outputImage.cols; ++column)
            {
                cv::Vec4b& pixel = pixels[column];
                const uint32_t alpha = pixel[3];
                if (alpha == 0)
                {
                    pixel[0] = 0;
                    pixel[1] = 0;
                    pixel[2] = 0;
                    continue;
                }
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    pixel[channel] = static_cast<uint8_t>(std::min(
                        255u,
                        (static_cast<uint32_t>(pixel[channel]) * 255u + alpha / 2u) /
                            alpha));
                }
            }
        }
    }

    bool WriteTileImage(
        const std::filesystem::path& path,
        const cv::Mat& image,
        std::string& error)
    {
        std::error_code createError;
        std::filesystem::create_directories(path.parent_path(), createError);
        if (createError)
        {
            error = "无法创建高德 GIS PNG 缓存目录：" + createError.message();
            return false;
        }
        std::vector<unsigned char> encodedImage;
        try
        {
            if (!cv::imencode(".png", image, encodedImage) || encodedImage.empty())
            {
                error = "无法编码高德 GIS PNG 瓦片。";
                return false;
            }
        }
        catch (const std::exception& exception)
        {
            error = "无法编码高德 GIS PNG 瓦片：" + std::string(exception.what());
            return false;
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            error = "无法写入高德 GIS PNG 瓦片。";
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(encodedImage.data()),
            static_cast<std::streamsize>(encodedImage.size()));
        if (!output)
        {
            error = "高德 GIS PNG 瓦片写入不完整。";
            return false;
        }
        return true;
    }

    bool IsFeatureVisibleAtZoom(
        const AmapVectorFeature& feature,
        uint32_t zoom,
        bool ignoreFeatureZoomRange)
    {
        if (ignoreFeatureZoomRange)
        {
            return true;
        }
        const bool meetsMinimumZoom = feature.minZoom == 0 || zoom >= feature.minZoom;
        return meetsMinimumZoom &&
            (feature.maxZoom == 0 || zoom <= feature.maxZoom);
    }

    AmapVectorDecodedData SelectBuildingFeaturesForFbx(
        const AmapVectorDecodedData& decodedData,
        uint32_t zoom,
        const AmapVectorRenderOptions& options)
    {
        AmapVectorDecodedData buildingData;
        buildingData.version = decodedData.version;
        buildingData.status = decodedData.status;
        buildingData.packetCount = decodedData.packetCount;
        for (const AmapVectorFeature& feature : decodedData.features)
        {
            const AmapVectorRenderCategory category = GetCategory(feature, options);
            if (category != AmapVectorRenderCategory::Building ||
                !IsCategorySelected(category, options) ||
                !IsFeatureVisibleAtZoom(feature, zoom, options.ignoreFeatureZoomRange) ||
                feature.geometryType != AmapVectorGeometryType::Polygon ||
                !feature.hasHeight || feature.height == 0)
            {
                continue;
            }
            buildingData.features.push_back(feature);
        }
        return buildingData;
    }

    bool BuildProjectedFeature(
        const AmapVectorFeature& feature,
        const AmapVectorRenderOptions& options,
        DownTool::GoogleDownBase& projection,
        ProjectedFeature& projectedFeature)
    {
        const AmapVectorRenderCategory category = GetCategory(feature, options);
        if (!IsCategorySelected(category, options))
        {
            return false;
        }

        projectedFeature.source = &feature;
        projectedFeature.category = category;
        projectedFeature.color = GetColor(feature, category, options);
        bool hasCoordinate = false;
        for (const std::vector<DownTool::AmapVectorCoordinate>& sourcePart : feature.geometryParts)
        {
            std::vector<cv::Point2d> projectedPart;
            projectedPart.reserve(sourcePart.size());
            for (const DownTool::AmapVectorCoordinate& coordinate : sourcePart)
            {
                const DownTool::LonLat wgs84Coordinate =
                    DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(
                        { coordinate.longitude, coordinate.latitude });
                Tile tile;
                double pixelX = 0.;
                double pixelY = 0.;
                if (!projection.TryGetTilePixelForLonLat(
                    wgs84Coordinate,
                    projection.GetZoom(),
                    tile,
                    pixelX,
                    pixelY))
                {
                    return false;
                }
                const double globalX =
                    static_cast<double>(tile.X) * options.tileSize + pixelX;
                const double globalY =
                    static_cast<double>(tile.Y) * options.tileSize + pixelY;
                projectedPart.emplace_back(globalX, globalY);
                if (!hasCoordinate)
                {
                    projectedFeature.minimumX = globalX;
                    projectedFeature.maximumX = globalX;
                    projectedFeature.minimumY = globalY;
                    projectedFeature.maximumY = globalY;
                    hasCoordinate = true;
                }
                else
                {
                    projectedFeature.minimumX = std::min(projectedFeature.minimumX, globalX);
                    projectedFeature.maximumX = std::max(projectedFeature.maximumX, globalX);
                    projectedFeature.minimumY = std::min(projectedFeature.minimumY, globalY);
                    projectedFeature.maximumY = std::max(projectedFeature.maximumY, globalY);
                }
            }
            if (!projectedPart.empty())
            {
                projectedFeature.geometryParts.push_back(std::move(projectedPart));
            }
        }
        return hasCoordinate;
    }

    void DrawFeature(
        const ProjectedFeature& feature,
        const Tile& tile,
        uint32_t tileSize,
        uint32_t renderScale,
        const AmapVectorRenderOptions& options,
        cv::Mat& image)
    {
        const double tileOriginX = static_cast<double>(tile.X) * tileSize;
        const double tileOriginY = static_cast<double>(tile.Y) * tileSize;
        std::vector<std::vector<cv::Point>> parts;
        parts.reserve(feature.geometryParts.size());
        for (const std::vector<cv::Point2d>& projectedPart : feature.geometryParts)
        {
            std::vector<cv::Point> localPart;
            localPart.reserve(projectedPart.size());
            for (const cv::Point2d& projectedPoint : projectedPart)
            {
                const double localX =
                    (projectedPoint.x - tileOriginX) * renderScale;
                const double localY =
                    (projectedPoint.y - tileOriginY) * renderScale;
                if (localX < static_cast<double>(std::numeric_limits<int>::min()) ||
                    localX > static_cast<double>(std::numeric_limits<int>::max()) ||
                    localY < static_cast<double>(std::numeric_limits<int>::min()) ||
                    localY > static_cast<double>(std::numeric_limits<int>::max()))
                {
                    return;
                }
                localPart.emplace_back(
                    static_cast<int>(std::lround(localX)),
                    static_cast<int>(std::lround(localY)));
            }
            if (!localPart.empty())
            {
                parts.push_back(std::move(localPart));
            }
        }

        const cv::Scalar color = ToPremultipliedBgra(feature.color);
        if (feature.source->geometryType == AmapVectorGeometryType::Polygon)
        {
            cv::fillPoly(image, parts, color, cv::LINE_AA);
            return;
        }
        if (feature.source->geometryType == AmapVectorGeometryType::LineString)
        {
            const int lineWidth = std::max(
                1,
                static_cast<int>(std::lround(
                    GetLineWidth(feature, options) * renderScale)));
            for (const std::vector<cv::Point>& part : parts)
            {
                if (part.size() >= 2)
                {
                    cv::polylines(image, part, false, color, lineWidth, cv::LINE_AA);
                }
            }
            return;
        }

        for (const std::vector<cv::Point>& part : parts)
        {
            if (!part.empty())
            {
                cv::circle(
                    image,
                    part.front(),
                    options.pointRadius * static_cast<int>(renderScale),
                    color,
                    cv::FILLED,
                    cv::LINE_AA);
            }
        }
    }
}

DownTool::AmapVectorRenderCategory DownTool::ClassifyAmapVectorFeature(
    const AmapVectorFeature& feature)
{
    if (IsAmapWaterFeatureCode(feature.mainKey, feature.subKey))
    {
        return AmapVectorRenderCategory::Water;
    }
    if (IsAmapGreenFeatureCode(feature.mainKey, feature.subKey))
    {
        return AmapVectorRenderCategory::Green;
    }
    if (IsAmapHighwayFeature(feature))
    {
        return AmapVectorRenderCategory::Highway;
    }
    if (feature.layerName == "road")
    {
        return AmapVectorRenderCategory::Road;
    }
    if (feature.layerName == "building")
    {
        return AmapVectorRenderCategory::Building;
    }
    if (IsAmapBoundaryFeature(feature))
    {
        return AmapVectorRenderCategory::Boundary;
    }
    if (feature.layerName.starts_with("transit-"))
    {
        return AmapVectorRenderCategory::Subway;
    }
    if (feature.layerName == "poi" || feature.geometryType == AmapVectorGeometryType::Point)
    {
        return AmapVectorRenderCategory::Point;
    }
    return AmapVectorRenderCategory::Other;
}

DownTool::AmapVectorColor DownTool::GetDefaultAmapVectorRenderColor(
    AmapVectorRenderCategory category)
{
    switch (category)
    {
    case AmapVectorRenderCategory::Road:
        return {255, 218, 113, 255};
    case AmapVectorRenderCategory::Water:
        return {106, 142, 202, 255};
    case AmapVectorRenderCategory::Green:
        return {83, 133, 78, 255};
    case AmapVectorRenderCategory::Highway:
        return {255, 111, 56, 255};
    case AmapVectorRenderCategory::Subway:
        return {210, 75, 220, 255};
    case AmapVectorRenderCategory::Building:
        return {255, 255, 255, 255};
    case AmapVectorRenderCategory::Point:
        return {229, 57, 53, 255};
    case AmapVectorRenderCategory::Boundary:
        return {54, 55, 56, 255};
    case AmapVectorRenderCategory::Other:
        return {96, 125, 139, 255};
    case AmapVectorRenderCategory::All:
        return {0, 0, 0, 0};
    }
    return {0, 0, 0, 0};
}

DownTool::AmapVectorPngRenderResult DownTool::AmapVectorPngDownloader::Render(
    const AmapVectorDecodedData& decodedData,
    const AmapVectorDownloadOptions& vectorOptions,
    const AmapVectorRenderOptions& renderOptions)
{
    AmapVectorPngRenderResult result;
    const auto isCancellationRequested = [&renderOptions]
    {
        return renderOptions.cancellationRequested &&
            renderOptions.cancellationRequested();
    };
    if (isCancellationRequested())
    {
        result.error = "高德 GIS PNG 渲染已取消。";
        return result;
    }
    if (renderOptions.categories.empty())
    {
        result.error = "高德 GIS PNG 输出至少需要选择一个类别。";
        return result;
    }
    if (renderOptions.tileSize == 0 ||
        renderOptions.tileSize > static_cast<uint32_t>(INT_MAX) / kRenderScale)
    {
        result.error = "高德 GIS PNG 瓦片大小超出高质量渲染支持范围。";
        return result;
    }
    if (renderOptions.roadLineWidth <= 0 || renderOptions.lineWidth <= 0 ||
        renderOptions.transitLineWidth <= 0 || renderOptions.pointRadius <= 0)
    {
        result.error = "高德 GIS PNG 线宽和点半径必须大于零。";
        return result;
    }
    try
    {
        (void)AmapVectorTileGrid::SelectTilesForWgs84Radius(
            vectorOptions.centerWgs84,
            vectorOptions.radiusMeters,
            vectorOptions.zoom);
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
        return result;
    }

    const std::filesystem::path temporaryTilePath = renderOptions.pngTileCachePath;
    result.pngTileCachePath = temporaryTilePath;

    SetCenterLonLat(vectorOptions.centerWgs84);
    SetRadius(vectorOptions.radiusMeters);
    SetZoom(vectorOptions.zoom);
    SetTileSize(renderOptions.tileSize);
    SetCachePath(PathToUtf8String(temporaryTilePath));
    CalculateDownRange();
    constexpr double kMaximumWebMercatorLatitude = 85.0511287798066;
    if (GetMinLonLat().lat < -kMaximumWebMercatorLatitude ||
        GetMaxLonLat().lat > kMaximumWebMercatorLatitude)
    {
        result.error = "高德 GIS PNG 输出范围超出 Web Mercator 纬度范围。";
        return result;
    }

    std::vector<Tile> targetTiles;
    BuildRequestedTiles(targetTiles);
    if (targetTiles.empty())
    {
        result.error = "高德 GIS PNG 输出范围没有包含可渲染瓦片。";
        return result;
    }

    std::vector<ProjectedFeature> projectedFeatures;
    projectedFeatures.reserve(decodedData.features.size());
    for (const AmapVectorFeature& feature : decodedData.features)
    {
        if (isCancellationRequested())
        {
            result.error = "高德 GIS PNG 渲染已取消。";
            return result;
        }
        if (!IsFeatureVisibleAtZoom(
            feature,
            vectorOptions.zoom,
            renderOptions.ignoreFeatureZoomRange))
        {
            continue;
        }
        ProjectedFeature projectedFeature;
        if (BuildProjectedFeature(feature, renderOptions, *this, projectedFeature))
        {
            projectedFeatures.push_back(std::move(projectedFeature));
        }
    }

    std::sort(
        projectedFeatures.begin(),
        projectedFeatures.end(),
        [](const ProjectedFeature& left, const ProjectedFeature& right)
        {
            const int leftPhase = GetRenderPhase(left);
            const int rightPhase = GetRenderPhase(right);
            if (leftPhase != rightPhase)
            {
                return leftPhase < rightPhase;
            }
            if (left.source->drawOrder != right.source->drawOrder)
            {
                return left.source->drawOrder < right.source->drawOrder;
            }
            if (left.source->rank != right.source->rank)
            {
                return left.source->rank < right.source->rank;
            }
            return left.source->uid < right.source->uid;
        });
    if (isCancellationRequested())
    {
        result.error = "高德 GIS PNG 渲染已取消。";
        return result;
    }

    std::map<std::pair<int32_t, int32_t>, std::vector<size_t>> tileFeatureIndexes;
    const int32_t minimumTileX = GetMinTile().X;
    const int32_t maximumTileX = GetMaxTile().X;
    const int32_t minimumTileY = GetMinTile().Y;
    const int32_t maximumTileY = GetMaxTile().Y;
    for (size_t featureIndex = 0; featureIndex < projectedFeatures.size(); ++featureIndex)
    {
        if (isCancellationRequested())
        {
            result.error = "高德 GIS PNG 渲染已取消。";
            return result;
        }
        const ProjectedFeature& feature = projectedFeatures[featureIndex];
        const int margin = feature.source->geometryType == AmapVectorGeometryType::Point ?
            renderOptions.pointRadius :
            (feature.source->geometryType == AmapVectorGeometryType::LineString ?
                static_cast<int>(std::ceil(GetLineWidth(
                    feature,
                    renderOptions))) : 0);
        const int64_t featureMinimumTileX = static_cast<int64_t>(std::floor(
            (feature.minimumX - margin) / renderOptions.tileSize));
        const int64_t featureMaximumTileX = static_cast<int64_t>(std::floor(
            (feature.maximumX + margin) / renderOptions.tileSize));
        const int64_t featureMinimumTileY = static_cast<int64_t>(std::floor(
            (feature.minimumY - margin) / renderOptions.tileSize));
        const int64_t featureMaximumTileY = static_cast<int64_t>(std::floor(
            (feature.maximumY + margin) / renderOptions.tileSize));
        const int32_t clampedMinimumX = static_cast<int32_t>(std::max<int64_t>(
            minimumTileX, featureMinimumTileX));
        const int32_t clampedMaximumX = static_cast<int32_t>(std::min<int64_t>(
            maximumTileX, featureMaximumTileX));
        const int32_t clampedMinimumY = static_cast<int32_t>(std::max<int64_t>(
            minimumTileY, featureMinimumTileY));
        const int32_t clampedMaximumY = static_cast<int32_t>(std::min<int64_t>(
            maximumTileY, featureMaximumTileY));
        if (clampedMinimumX > clampedMaximumX || clampedMinimumY > clampedMaximumY)
        {
            continue;
        }
        for (int32_t x = clampedMinimumX; x <= clampedMaximumX; ++x)
        {
            if (isCancellationRequested())
            {
                result.error = "高德 GIS PNG 渲染已取消。";
                return result;
            }
            for (int32_t y = clampedMinimumY; y <= clampedMaximumY; ++y)
            {
                tileFeatureIndexes[{ x, y }].push_back(featureIndex);
            }
        }
    }

    std::atomic_size_t nextTileIndex = 0;
    std::atomic_uint32_t renderedTileCount = 0;
    std::atomic_bool cancellationRequested = false;
    std::mutex errorMutex;
    std::mutex progressMutex;
    size_t completedTileCount = 0;
    size_t failedTileCount = 0;
    const auto setRenderError = [&](std::string error)
    {
        cancellationRequested.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> errorLock(errorMutex);
        if (result.error.empty())
        {
            result.error = std::move(error);
        }
    };
    const auto reportProgress = [&](bool failed)
    {
        std::lock_guard<std::mutex> lock(progressMutex);
        ++completedTileCount;
        failedTileCount += failed ? 1 : 0;
        if (renderOptions.progressCallback)
        {
            try
            {
                renderOptions.progressCallback({
                    completedTileCount,
                    targetTiles.size(),
                    failedTileCount });
            }
            catch (const std::exception& exception)
            {
                setRenderError(
                    "高德 GIS PNG 进度回调异常：" + std::string(exception.what()));
            }
            catch (...)
            {
                setRenderError("高德 GIS PNG 进度回调发生未知异常。");
            }
        }
    };

    if (renderOptions.progressCallback)
    {
        try
        {
            renderOptions.progressCallback({ 0, targetTiles.size(), 0 });
        }
        catch (const std::exception& exception)
        {
            setRenderError("高德 GIS PNG 进度回调异常：" + std::string(exception.what()));
        }
        catch (...)
        {
            setRenderError("高德 GIS PNG 进度回调发生未知异常。");
        }
    }

    if (cancellationRequested.load(std::memory_order_relaxed))
    {
        return result;
    }

    const std::map<std::pair<int32_t, int32_t>, std::vector<size_t>>&
        indexedTileFeatures = tileFeatureIndexes;
    const size_t workerCount =
        GetRenderWorkerCount(renderOptions.maxConcurrency, targetTiles.size());
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    try
    {
        for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
        {
            workers.emplace_back(
                [&]()
                {
                    while (!cancellationRequested.load(std::memory_order_relaxed) &&
                           (!renderOptions.cancellationRequested ||
                            !renderOptions.cancellationRequested()))
                    {
                        const size_t tileIndex =
                            nextTileIndex.fetch_add(1, std::memory_order_relaxed);
                        if (tileIndex >= targetTiles.size())
                        {
                            return;
                        }

                        const Tile& tile = targetTiles[tileIndex];
                        const std::filesystem::path tilePath = GetTileFilePath(tile);
                        const uint32_t renderTileSize =
                            renderOptions.tileSize * kRenderScale;
                        cv::Mat renderImage(
                            static_cast<int>(renderTileSize),
                            static_cast<int>(renderTileSize),
                            CV_8UC4,
                            cv::Scalar(0, 0, 0, 0));
                        const auto indexIterator =
                            indexedTileFeatures.find({ tile.X, tile.Y });
                        if (indexIterator != indexedTileFeatures.end())
                        {
                            for (const size_t featureIndex : indexIterator->second)
                            {
                                if (isCancellationRequested())
                                {
                                    return;
                                }
                                DrawFeature(
                                    projectedFeatures[featureIndex],
                                    tile,
                                    renderOptions.tileSize,
                                    kRenderScale,
                                    renderOptions,
                                    renderImage);
                            }
                        }

                        cv::Mat image;
                        DownsampleRenderImage(
                            renderImage,
                            renderOptions.tileSize,
                            image);

                        std::string tileError;
                        const bool success = WriteTileImage(tilePath, image, tileError);
                        if (success)
                        {
                            renderedTileCount.fetch_add(1, std::memory_order_relaxed);
                        }
                        else
                        {
                            setRenderError(tileError);
                        }
                        reportProgress(!success);
                    }
                });
        }
    }
    catch (const std::exception& exception)
    {
        setRenderError("无法创建高德 GIS PNG 线程：" + std::string(exception.what()));
    }
    catch (...)
    {
        setRenderError("无法创建高德 GIS PNG 线程。");
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    result.renderedTileCount = renderedTileCount.load(std::memory_order_relaxed);
    if (renderOptions.cancellationRequested && renderOptions.cancellationRequested())
    {
        result.error = "高德 GIS PNG 渲染已取消。";
        return result;
    }
    if (cancellationRequested.load(std::memory_order_relaxed))
    {
        return result;
    }

    if (GetPostProcessEnabled() || GetPostProcessMergedImageEnabled() || GetPostProcessFbxEnabled())
    {
        RunPostProcess();
    }

    if (GetPostProcessFbxEnabled() && renderOptions.exportBuildingsToFbx)
    {
        const AmapVectorDecodedData buildingData = SelectBuildingFeaturesForFbx(
            decodedData,
            vectorOptions.zoom,
            renderOptions);
        if (!buildingData.features.empty())
        {
            AmapBuildingFbxExportOptions buildingFbxOptions;
            buildingFbxOptions.outputPath = GetFbxRootPath() / "buildings.fbx";
            buildingFbxOptions.centerWgs84 = vectorOptions.centerWgs84;
            buildingFbxOptions.shape = GetFbxShape();
            buildingFbxOptions.wallTexturePath = renderOptions.buildingWallTexturePath;
            buildingFbxOptions.roofTexturePath = renderOptions.buildingRoofTexturePath;
            buildingFbxOptions.wallTextureMode = renderOptions.buildingFbxWallTextureMode;
            buildingFbxOptions.roofTextureMode = renderOptions.buildingFbxRoofTextureMode;
            buildingFbxOptions.atlasImageFormat =
                renderOptions.buildingFbxAtlasImageFormat;
            buildingFbxOptions.satelliteRoofTextures =
                renderOptions.buildingFbxSatelliteRoofTextures;
            buildingFbxOptions.layerHeightCentimeters =
                renderOptions.buildingLayerHeightCentimeters;
            buildingFbxOptions.textureUnitLayer = renderOptions.buildingTextureUnitLayer;
            buildingFbxOptions.textureUnitPixelSize =
                renderOptions.buildingTextureUnitPixelSize;
            buildingFbxOptions.maxWallMaterialsPerBucket =
                renderOptions.buildingMaxWallMaterialsPerBucket;
            buildingFbxOptions.randomSeed = renderOptions.buildingTextureRandomSeed;
            buildingFbxOptions.terrainHeightEnabled = GetFbxTerrainHeightEnabled();
            buildingFbxOptions.elevationReferenceMeters = GetFbxElevationReferenceMeters();
            buildingFbxOptions.elevationSampler = GetFbxElevationSampler();
            buildingFbxOptions.cancellationRequested = isCancellationRequested;
            const AmapBuildingFbxExportResult buildingFbxResult =
                ExportAmapBuildingsToFbx(buildingData, buildingFbxOptions);
            if (!buildingFbxResult.success)
            {
                result.error = buildingFbxResult.error;
                return result;
            }
        }
    }

    if (isCancellationRequested() || IsStopRequested())
    {
        result.error = "高德 GIS PNG 渲染已取消。";
        return result;
    }

    result.success = true;
    return result;
}

DownTool::AmapVectorPngDownloadResult DownTool::AmapVectorPngDownloader::DownloadPngTiles(
    const AmapVectorPngDownloadOptions& options)
{
    AmapVectorPngDownloadResult result;
    std::vector<AmapVectorTile> sourceTiles;
    try
    {
        sourceTiles = SelectSourceTilesForPngOutput(options.vectorDownload);
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
        return result;
    }

    AmapVectorDownloader vectorDownloader;
    result.vectorDownload = vectorDownloader.DownloadTiles(
        options.vectorDownload,
        std::move(sourceTiles));
    if (!result.vectorDownload.success)
    {
        result.error = result.vectorDownload.error;
        return result;
    }

    if (!options.vectorDownload.geoJsonOutputPath.empty() &&
        !AmapVectorGeoJsonWriter::WriteFile(
            result.vectorDownload.decodedData,
            options.vectorDownload.geoJsonOutputPath,
            result.error,
            options.vectorDownload.cancellationRequested))
    {
        return result;
    }

    result.render = Render(
        result.vectorDownload.decodedData,
        options.vectorDownload,
        options.render);
    if (!result.render.success)
    {
        result.error = result.render.error;
        return result;
    }

    result.success = true;
    return result;
}
