#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

#include "AmapVectorPngDownloader.h"
#include "GCJ02GoogleDownBase.h"

namespace
{
    bool IsNearColor(uint8_t actual, uint8_t expected)
    {
        return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 2;
    }

    bool Require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    }

    bool HasColor(
        const cv::Mat& image,
        uint8_t red,
        uint8_t green,
        uint8_t blue)
    {
        for (int row = 0; row < image.rows; ++row)
        {
            for (int column = 0; column < image.cols; ++column)
            {
                const cv::Vec4b pixel = image.at<cv::Vec4b>(row, column);
                if (IsNearColor(pixel[0], blue) &&
                    IsNearColor(pixel[1], green) &&
                    IsNearColor(pixel[2], red) && pixel[3] != 0)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool TryGetPixelAtGcj02(
        const cv::Mat& image,
        const DownTool::LonLat& coordinate,
        uint32_t zoom,
        const DownTool::Tile& expectedTile,
        cv::Vec4b& pixel)
    {
        const DownTool::LonLat wgs84Coordinate =
            DownTool::GCJ02GoogleDownBase::GCJ02ToWGS84(coordinate);
        DownTool::GoogleDownBase projection;
        DownTool::Tile tile;
        double pixelX = 0.;
        double pixelY = 0.;
        if (!projection.TryGetTilePixelForLonLat(
                wgs84Coordinate, zoom, tile, pixelX, pixelY) ||
            tile.X != expectedTile.X || tile.Y != expectedTile.Y ||
            pixelX < 0. || pixelY < 0. ||
            pixelX >= static_cast<double>(image.cols) ||
            pixelY >= static_cast<double>(image.rows))
        {
            return false;
        }
        pixel = image.at<cv::Vec4b>(
            static_cast<int>(pixelY), static_cast<int>(pixelX));
        return pixel[3] != 0;
    }

    bool SameRgb(const cv::Vec4b& first, const cv::Vec4b& second)
    {
        return first[0] == second[0] && first[1] == second[1] &&
            first[2] == second[2];
    }

    DownTool::AmapVectorFeature MakePolygon(
        const std::string& layerName,
        const DownTool::LonLat& center,
        double halfWidth,
        double halfHeight,
        uint32_t mainKey)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::Polygon;
        feature.layerName = layerName;
        feature.mainKey = mainKey;
        feature.minZoom = 1;
        feature.maxZoom = 20;
        feature.geometryParts.push_back({
            { center.lon - halfWidth, center.lat - halfHeight },
            { center.lon + halfWidth, center.lat - halfHeight },
            { center.lon + halfWidth, center.lat + halfHeight },
            { center.lon - halfWidth, center.lat + halfHeight },
            { center.lon - halfWidth, center.lat - halfHeight }});
        return feature;
    }

    DownTool::AmapVectorFeature MakeHeightBuilding(
        const DownTool::LonLat& center,
        uint64_t height,
        bool hasHeight,
        uint32_t subKey)
    {
        DownTool::AmapVectorFeature feature = MakePolygon(
            "building", center, 0.0007, 0.00035, 200);
        feature.subKey = subKey;
        feature.hasHeight = hasHeight;
        feature.height = height;
        return feature;
    }

    DownTool::AmapVectorFeature MakeLine(
        const DownTool::LonLat& center,
        double halfWidth,
        uint32_t mainKey)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::LineString;
        feature.layerName = "road";
        feature.mainKey = mainKey;
        feature.minZoom = 1;
        feature.maxZoom = 20;
        feature.geometryParts.push_back({
            { center.lon - halfWidth, center.lat },
            { center.lon + halfWidth, center.lat }});
        return feature;
    }

    DownTool::AmapVectorFeature MakeWaterLine(
        const DownTool::LonLat& center,
        double latitudeOffset,
        uint32_t subKey,
        uint32_t minimumZoom)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::LineString;
        feature.layerName = "region-line";
        feature.mainKey = 20014;
        feature.subKey = subKey;
        feature.minZoom = minimumZoom;
        feature.maxZoom = 30;
        feature.geometryParts.push_back({
            { center.lon - 0.0015, center.lat + latitudeOffset },
            { center.lon, center.lat + latitudeOffset + 0.0002 },
            { center.lon + 0.0015, center.lat + latitudeOffset }});
        return feature;
    }

    DownTool::AmapVectorFeature MakePoint(const DownTool::LonLat& center)
    {
        DownTool::AmapVectorFeature feature;
        feature.geometryType = DownTool::AmapVectorGeometryType::Point;
        feature.layerName = "poi";
        feature.minZoom = 1;
        feature.maxZoom = 20;
        feature.geometryParts.push_back({ { center.lon, center.lat } });
        return feature;
    }

    bool RunTest()
    {
        const DownTool::AmapVectorDownloadOptions defaultVectorOptions;
        const DownTool::AmapVectorRenderOptions defaultRenderOptions;
        if (!Require(
            defaultVectorOptions.rawTileCachePath ==
                std::filesystem::u8path("cache/高德地图/gis数据/pbf缓存"),
            "默认 PBF 缓存目录错误") ||
            !Require(
                defaultRenderOptions.pngTileCachePath ==
                    std::filesystem::u8path("cache/高德地图/gis数据/临时"),
                "默认 PNG 临时目录错误"))
        {
            return false;
        }

        const std::filesystem::path outputRoot =
            std::filesystem::u8path(DOMDOWNTOOL_SOURCE_DIR) / "tmp" / "amap-vector-png-tests";
        std::error_code removeError;
        std::filesystem::remove_all(outputRoot, removeError);
        if (!Require(!removeError, "无法清理高德 GIS PNG 测试目录"))
        {
            return false;
        }

        const DownTool::LonLat centerWgs84(116.0500, 39.9760);
        const DownTool::LonLat centerGcj02 =
            DownTool::GCJ02GoogleDownBase::WGS84ToGCJ02(centerWgs84);
        DownTool::AmapVectorDecodedData decodedData;
        decodedData.status = true;
        DownTool::AmapVectorFeature waterFeature = MakePolygon(
            "region", centerGcj02, 0.002, 0.001, 30001);
        waterFeature.subKey = 2;
        waterFeature.minZoom = 15;
        decodedData.features.push_back(std::move(waterFeature));
        DownTool::AmapVectorFeature greenFeature = MakePolygon(
            "region",
            { centerGcj02.lon + 0.005, centerGcj02.lat },
            0.001,
            0.0005,
            30001);
        greenFeature.subKey = 3;
        decodedData.features.push_back(std::move(greenFeature));
        DownTool::AmapVectorFeature buildingFeature = MakePolygon(
            "building",
            { centerGcj02.lon, centerGcj02.lat - 0.002 },
            0.0004,
            0.00025,
            200);
        buildingFeature.minZoom = 15;
        decodedData.features.push_back(std::move(buildingFeature));
        decodedData.features.push_back(MakeLine(centerGcj02, 0.0015, 300));
        decodedData.features.push_back(MakeWaterLine(centerGcj02, 0.0006, 2, 10));
        decodedData.features.push_back(MakeWaterLine(centerGcj02, -0.0006, 1, 14));
        decodedData.features.push_back(MakePoint(centerGcj02));

        DownTool::AmapVectorPngDownloader downloader;
        DownTool::AmapVectorDownloadOptions vectorOptions;
        vectorOptions.centerWgs84 = centerWgs84;
        vectorOptions.radiusMeters = 100.;
        vectorOptions.zoom = 14;
        const DownTool::Tile tile =
            DownTool::GoogleDownBase::LonLatToTile(centerWgs84, vectorOptions.zoom);

        DownTool::AmapVectorRenderOptions renderOptions;
        renderOptions.pngTileCachePath = outputRoot / "png-cache";
        std::vector<DownTool::AmapVectorRenderProgress> renderProgress;
        renderOptions.progressCallback =
            [&](const DownTool::AmapVectorRenderProgress& progress)
            {
                renderProgress.push_back(progress);
            };

        DownTool::AmapVectorRenderOptions unicodeRenderOptions = renderOptions;
        unicodeRenderOptions.pngTileCachePath =
            outputRoot / std::filesystem::u8path("高德地图/临时");
        unicodeRenderOptions.progressCallback = {};
        const DownTool::AmapVectorPngRenderResult unicodeRenderResult =
            downloader.Render(decodedData, vectorOptions, unicodeRenderOptions);
        const std::filesystem::path unicodeTilePath =
            unicodeRenderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        if (!Require(
            unicodeRenderResult.success && std::filesystem::exists(unicodeTilePath),
            "中文临时目录无法写入高德 GIS PNG：" + unicodeRenderResult.error))
        {
            return false;
        }

        DownTool::AmapVectorPngRenderResult renderResult =
            downloader.Render(decodedData, vectorOptions, renderOptions);
        if (!Require(renderResult.success, "高德 GIS PNG 渲染失败：" + renderResult.error) ||
            !Require(renderResult.renderedTileCount == 1, "测试范围应渲染一张 PNG 瓦片") ||
            !Require(
                renderProgress.size() == 2 &&
                    renderProgress.front().completedTileCount == 0 &&
                    renderProgress.back().completedTileCount == 1 &&
                    renderProgress.back().failedTileCount == 0,
                "PNG 渲染进度错误"))
        {
            return false;
        }

        const std::filesystem::path tilePath =
            renderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat image = cv::imread(tilePath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(!image.empty() && image.channels() == 4, "PNG 瓦片不可读取") ||
            !Require(HasColor(image, 106, 142, 202), "缺少水体默认蓝色") ||
            !Require(HasColor(image, 83, 133, 78), "缺少绿地默认绿色") ||
            !Require(HasColor(image, 255, 218, 113), "缺少道路默认黄色"))
        {
            return false;
        }

        DownTool::AmapVectorDecodedData heightData;
        heightData.status = true;
        const DownTool::LonLat sameHeightCenterA =
            { centerGcj02.lon - 0.004, centerGcj02.lat - 0.001 };
        const DownTool::LonLat sameHeightCenterB =
            { centerGcj02.lon + 0.004, centerGcj02.lat - 0.001 };
        const DownTool::LonLat differentHeightCenter =
            { centerGcj02.lon, centerGcj02.lat - 0.003 };
        const DownTool::LonLat zeroHeightCenter =
            { centerGcj02.lon - 0.003, centerGcj02.lat - 0.005 };
        const DownTool::LonLat missingHeightCenter =
            { centerGcj02.lon + 0.003, centerGcj02.lat - 0.005 };
        heightData.features.push_back(
            MakeHeightBuilding(sameHeightCenterA, 18, true, 1));
        heightData.features.push_back(
            MakeHeightBuilding(sameHeightCenterB, 18, true, 2));
        heightData.features.push_back(
            MakeHeightBuilding(differentHeightCenter, 30, true, 3));
        heightData.features.push_back(
            MakeHeightBuilding(zeroHeightCenter, 0, true, 4));
        heightData.features.push_back(
            MakeHeightBuilding(missingHeightCenter, 30, false, 5));

        DownTool::AmapVectorRenderOptions heightRenderOptions;
        heightRenderOptions.categories = { DownTool::AmapVectorRenderCategory::Building };
        heightRenderOptions.ignoreFeatureZoomRange = true;
        heightRenderOptions.colorBuildingsByHeight = true;
        heightRenderOptions.pngTileCachePath = outputRoot / "height-colors";
        const DownTool::AmapVectorPngRenderResult heightRenderResult =
            downloader.Render(heightData, vectorOptions, heightRenderOptions);
        const std::filesystem::path heightTilePath =
            heightRenderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat heightImage = cv::imread(heightTilePath.string(), cv::IMREAD_UNCHANGED);
        cv::Vec4b sameHeightPixelA;
        cv::Vec4b sameHeightPixelB;
        cv::Vec4b differentHeightPixel;
        cv::Vec4b zeroHeightPixel;
        cv::Vec4b missingHeightPixel;
        if (!Require(heightRenderResult.success, "建筑高度颜色渲染失败：" + heightRenderResult.error) ||
            !Require(!heightImage.empty() && heightImage.channels() == 4,
                "建筑高度颜色 PNG 不可读取") ||
            !Require(TryGetPixelAtGcj02(
                heightImage, sameHeightCenterA, vectorOptions.zoom, tile, sameHeightPixelA),
                "无法读取第一个同高建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage, sameHeightCenterB, vectorOptions.zoom, tile, sameHeightPixelB),
                "无法读取第二个同高建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage,
                differentHeightCenter,
                vectorOptions.zoom,
                tile,
                differentHeightPixel),
                "无法读取异高建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage, zeroHeightCenter, vectorOptions.zoom, tile, zeroHeightPixel),
                "无法读取零高度建筑像素") ||
            !Require(TryGetPixelAtGcj02(
                heightImage,
                missingHeightCenter,
                vectorOptions.zoom,
                tile,
                missingHeightPixel),
                "无法读取无高度属性建筑像素") ||
            !Require(SameRgb(sameHeightPixelA, sameHeightPixelB),
                "相同高度建筑没有保持相同颜色") ||
            !Require(!SameRgb(sameHeightPixelA, differentHeightPixel),
                "不同高度建筑没有使用不同颜色") ||
            !Require(
                zeroHeightPixel[0] == 255 && zeroHeightPixel[1] == 255 &&
                    zeroHeightPixel[2] == 255,
                "零高度建筑没有沿用普通建筑颜色") ||
            !Require(
                missingHeightPixel[0] == 255 && missingHeightPixel[1] == 255 &&
                    missingHeightPixel[2] == 255,
                "无高度属性建筑没有沿用普通建筑颜色"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Water };
        renderOptions.colors[DownTool::AmapVectorRenderCategory::Water] = { 10, 20, 30, 255 };
        renderOptions.featureCodeColors[{ 30001, 3 }] = { 250, 10, 240, 255 };
        renderOptions.featureCodeColors[{ 20014, 2 }] = { 70, 80, 90, 255 };
        renderOptions.featureCodeColors[{ 20014, 1 }] = { 100, 110, 120, 255 };
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const cv::Mat waterOnly = cv::imread(tilePath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success, "水体类别渲染失败：" + renderResult.error) ||
            !Require(!HasColor(waterOnly, 10, 20, 30),
                "z14 rendered water whose minZoom is 15") ||
            !Require(HasColor(waterOnly, 70, 80, 90), "missing visible water line") ||
            !Require(HasColor(waterOnly, 100, 110, 120), "missing visible water line") ||
            !Require(!HasColor(waterOnly, 250, 10, 240), "水体筛选后仍包含绿地"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Building };
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        if (!Require(renderResult.success, "建筑类别渲染失败：" + renderResult.error))
        {
            return false;
        }
        const std::filesystem::path buildingOnlyPath =
            renderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat buildingOnly =
            cv::imread(buildingOnlyPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(!HasColor(buildingOnly, 255, 255, 255),
            "z14 rendered a building whose minZoom is 15"))
        {
            return false;
        }

        renderOptions.ignoreFeatureZoomRange = true;
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const cv::Mat forcedBuilding =
            cv::imread(buildingOnlyPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success, "forcing all feature zooms failed") ||
            !Require(HasColor(forcedBuilding, 255, 255, 255),
                "forcing all feature zooms did not render the building") ||
            !Require(!HasColor(forcedBuilding, 106, 142, 202),
                "the building category rendered water"))
        {
            return false;
        }

        cv::Mat staleImage(256, 256, CV_8UC4, cv::Scalar(13, 17, 251, 255));
        if (!Require(cv::imwrite(buildingOnlyPath.string(), staleImage),
            "无法写入临时 PNG 测试数据"))
        {
            return false;
        }

        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const cv::Mat rebuiltBuildingOnly =
            cv::imread(buildingOnlyPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success && renderResult.renderedTileCount == 1,
            "临时 PNG 没有重新渲染") ||
            !Require(HasColor(rebuiltBuildingOnly, 255, 255, 255),
                "临时 PNG 仍然保留旧内容"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Road };
        renderOptions.featureCodeColors[{ 300, 0 }] = { 10, 20, 30, 255 };
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        const std::filesystem::path customRoadPath =
            renderResult.pngTileCachePath / std::to_string(tile.Zoom) /
            std::to_string(tile.X) / (std::to_string(tile.Y) + ".png");
        const cv::Mat customRoad = cv::imread(customRoadPath.string(), cv::IMREAD_UNCHANGED);
        if (!Require(renderResult.success, "道路自定义颜色渲染失败：" + renderResult.error) ||
            !Require(HasColor(customRoad, 10, 20, 30), "主/子分类自定义颜色没有生效"))
        {
            return false;
        }

        DownTool::AmapVectorDownloadOptions invalidVectorOptions = vectorOptions;
        invalidVectorOptions.radiusMeters = -1.;
        const DownTool::AmapVectorPngRenderResult invalidRender =
            downloader.Render(decodedData, invalidVectorOptions, renderOptions);
        if (!Require(!invalidRender.success, "负数半径必须被 PNG 渲染入口拒绝"))
        {
            return false;
        }

        renderOptions.categories = { DownTool::AmapVectorRenderCategory::Building };
        renderOptions.featureCodeColors.clear();

        const std::filesystem::path postProcessRoot = outputRoot / "postprocess";
        const std::filesystem::path xyzRoot = postProcessRoot / "XYZ";
        const std::filesystem::path fbxRoot = postProcessRoot / "FBX";
        downloader.SetPostProcessOutputPath(xyzRoot.string());
        downloader.SetPostProcessEnabled(true);
        downloader.SetPostProcessMergedImageEnabled(true);
        downloader.SetPostProcessMergedImageResolution(256);
        downloader.SetPostProcessMergedImageOutputPath(
            (postProcessRoot / "merged.png").string());
        downloader.SetPostProcessFbxEnabled(true);
        downloader.SetFbxOutputPath(fbxRoot.string());
        downloader.SetFbxTextureSize(256);
        downloader.SetFbxShape(DownTool::FbxPlanetShape::Plane);
        downloader.SetFbxMeshSubdivision(2);
        renderResult = downloader.Render(decodedData, vectorOptions, renderOptions);
        if (!Require(renderResult.success, "PNG 后处理渲染失败：" + renderResult.error) ||
            !Require(std::filesystem::exists(
                xyzRoot / std::to_string(tile.Zoom) / std::to_string(tile.X) /
                (std::to_string(tile.Y) + ".png")),
                "后处理金字塔缺少最高层级瓦片") ||
            !Require(std::filesystem::exists(postProcessRoot / "merged.png"),
                "后处理缺少合并大图") ||
            !Require(std::filesystem::exists(
                fbxRoot / "plane.fbx"),
                "后处理缺少 FBX"))
        {
            return false;
        }

        std::cout << "AmapVectorPngTileTest passed\n";
        return true;
    }
}

int main()
{
    try
    {
        return RunTest() ? 0 : 1;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "unexpected exception: " << exception.what() << '\n';
        return 1;
    }
}
