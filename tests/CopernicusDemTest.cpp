#include "CopernicusDem.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/core/mat.hpp"
#include "PostProcessTestSupport.h"

namespace
{
DownTool::Wgs84Ring Rectangle(double west, double south, double east, double north)
{
    return {{west, south}, {east, south}, {east, north}, {west, north}};
}

bool TestFlattenedRegions(const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    DownTool::CopernicusDemOptions options;
    options.centerWgs84 = {116.5, 39.5};
    options.radiusMeters = 1000.;
    options.cachePath = testRoot / "flatten-cache";
    options.downloadCallback = [](std::string_view, const std::filesystem::path& path,
                                  const DownTool::CopernicusDemCancellationCallback&,
                                  std::string&) {
        cv::Mat image(8, 8, CV_32FC1);
        for (int y = 0; y < image.rows; ++y)
        {
            for (int x = 0; x < image.cols; ++x)
            {
                image.at<float>(y, x) = x * 10.F - 40.F;
            }
        }
        std::vector<unsigned char> encoded;
        cv::imencode(".tif", image, encoded);
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        return output.good();
    };
    std::string error;
    const auto raw = DownTool::CopernicusDemData::Prepare(options, error);
    options.SetFlattenPolygonsWgs84({Rectangle(116.25, 39.2, 116.75, 39.8)});
    const auto flattened = DownTool::CopernicusDemData::Prepare(options, error);
    const auto equals = [](const std::optional<double>& actual, double expected) {
        return actual && std::abs(*actual - expected) < 1.e-8;
    };
    if (!Require(raw && flattened, "平整区域 DEM 准备失败：" + error) ||
        !Require(equals(flattened->Sample({116.3, 39.3}), -5.) &&
                     equals(flattened->Sample({116.7, 39.7}), -5.) &&
                     equals(flattened->Sample({116.25, 39.5}), -5.) &&
                     flattened->Sample({116.9, 39.5}) == raw->Sample({116.9, 39.5}) &&
                     raw->Sample({116.3, 39.3}) != raw->Sample({116.7, 39.7}),
                 "区域内没有保持同一负海拔，或区域外的原始高程被修改"))
    {
        return false;
    }
    options.SetFlattenPolygonsWgs84(
        {Rectangle(116.25, 39.2, 116.75, 39.8), Rectangle(116.5, 39.2, 116.875, 39.8)});
    const auto overlapping = DownTool::CopernicusDemData::Prepare(options, error);
    if (!Require(overlapping && equals(overlapping->Sample({116.3, 39.5}), -5.) &&
                     equals(overlapping->Sample({116.6, 39.5}), 10.),
                 "重叠平整区没有独立使用原始 DEM 平均值并按列表顺序覆盖"))
    {
        return false;
    }

    const DownTool::Wgs84Ring concave = {{116.25, 39.25}, {116.75, 39.25}, {116.75, 39.5},
                                         {116.5, 39.5},   {116.5, 39.75},  {116.25, 39.75}};
    options.SetFlattenPolygonsWgs84({concave});
    const auto concaveData = DownTool::CopernicusDemData::Prepare(options, error);
    double total = 0.;
    double weight = 0.;
    for (int y = 2; y <= 5; ++y)
    {
        const double rowWeight = std::cos((40. - (y + 0.5) / 8.) * std::numbers::pi / 180.);
        total += (y < 4 ? -30. : -20.) * rowWeight;
        weight += (y < 4 ? 2. : 4.) * rowWeight;
    }
    if (!Require(concaveData && equals(concaveData->Sample({116.3, 39.6}), total / weight) &&
                     concaveData->Sample({116.6, 39.6}) == raw->Sample({116.6, 39.6}),
                 "凹多边形使用了外接矩形平均值，或凹口被错误平整"))
    {
        return false;
    }

    const auto tiny = Rectangle(116.501, 39.501, 116.502, 39.502);
    options.SetFlattenPolygonsWgs84({tiny});
    const auto tinyData = DownTool::CopernicusDemData::Prepare(options, error);
    double tinyMean = 0.;
    for (const auto& point : tiny)
    {
        tinyMean += *raw->Sample(point) / tiny.size();
    }
    if (!Require(tinyData && equals(tinyData->Sample({116.5015, 39.5015}), tinyMean),
                 "小于原始 DEM 像元的平整区不能采样"))
    {
        return false;
    }
    options.SetFlattenPolygonsWgs84({{{116.2, 39.2}, {116.8, 39.8}, {116.2, 39.8}, {116.8, 39.2}}});
    if (!Require(!DownTool::CopernicusDemData::Prepare(options, error), "接受了自交平整区域"))
    {
        return false;
    }
    bool canceled = false;
    options.SetFlattenPolygonsWgs84({Rectangle(116.25, 39.2, 116.75, 39.8)});
    options.progressCallback = [&canceled](const DownTool::CopernicusDemProgress& progress) {
        if (progress.stage == "计算平整区域平均海拔")
        {
            canceled = true;
        }
    };
    options.cancellationRequested = [&canceled] { return canceled; };
    if (!Require(!DownTool::CopernicusDemData::Prepare(options, error) && canceled &&
                     error.find("取消") != std::string::npos,
                 "平整区域统计没有响应取消"))
    {
        return false;
    }
    options.cancellationRequested = {};
    options.progressCallback = {};
    options.SetFlattenPolygonsWgs84({});
    const auto restored = DownTool::CopernicusDemData::Prepare(options, error);
    if (!Require(restored && restored->Sample({116.3, 39.3}) == raw->Sample({116.3, 39.3}),
                 "移除平整区域后缓存仍保留被平整的高程"))
    {
        return false;
    }
    options.SetFlattenPolygonsWgs84({Rectangle(116.25, 39.25, 116.75, 39.75)});
    for (bool valid : {false, true})
    {
        options.cachePath = testRoot / (valid ? "mixed-valid-cache" : "no-data-cache");
        options.downloadCallback = [valid](std::string_view, const std::filesystem::path& path,
                                           const DownTool::CopernicusDemCancellationCallback&,
                                           std::string&) {
            cv::Mat image(8, 8, CV_32FC1, cv::Scalar(-32768.F));
            image.at<float>(0, 0) = 200.F;
            if (valid)
            {
                image.at<float>(4, 4) = -20.F;
            }
            std::vector<unsigned char> encoded;
            cv::imencode(".tif", image, encoded);
            std::ofstream output(path, std::ios::binary);
            output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
            return output.good();
        };
        options.radiusMeters = 60000.;
        const auto mixed = DownTool::CopernicusDemData::Prepare(options, error);
        if (!Require(valid ? mixed && equals(mixed->Sample({116.5, 39.5}), -20.)
                           : !mixed && error.find("平整区域内没有有效") != std::string::npos,
                     "平整区域没有忽略 NoData，或全部无效时没有明确报错：" + error))
        {
            return false;
        }
    }
    return true;
}

bool TestFlattenAcrossDemTiles(const std::filesystem::path& testRoot)
{
    using postprocess_test::Require;
    DownTool::CopernicusDemOptions options;
    options.centerWgs84 = {116.9, 39.5};
    options.radiusMeters = 1000.;
    options.cachePath = testRoot / "flatten-cross-tile-cache";
    options.SetFlattenPolygonsWgs84({Rectangle(116.75, 39.3, 117.25, 39.7)});
    options.downloadCallback = [](std::string_view url, const std::filesystem::path& path,
                                  const DownTool::CopernicusDemCancellationCallback&,
                                  std::string&) {
        const bool east = url.find("E117_00") != std::string_view::npos;
        cv::Mat image(8, east ? 16 : 8, CV_32FC1, cv::Scalar(east ? 200.F : 100.F));
        std::vector<unsigned char> encoded;
        cv::imencode(".tif", image, encoded);
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        return output.good();
    };
    std::string error;
    const auto data = DownTool::CopernicusDemData::Prepare(options, error);
    return Require(data && data->GetSourceFiles().size() == 2 && data->GetBounds().east < 117. &&
                       std::abs(data->Sample({116.8, 39.5}).value_or(-1.) - 150.) < 1.e-8 &&
                       std::abs(data->Sample({117.2, 39.5}).value_or(-1.) - 150.) < 1.e-8,
                   "跨 DEM 分幅没有共用完整区域平均值，或不同分辨率的像元权重错误：" + error);
}

bool RunTest()
{
    const std::filesystem::path testRoot =
        postprocess_test::MakeTestRoot("copernicus-dem");
    if (!postprocess_test::ResetDirectory(testRoot))
    {
        return false;
    }

    std::atomic_uint32_t downloadCount = 0;
    DownTool::CopernicusDemOptions options;
    options.centerWgs84 = DownTool::LonLat(116.5, 39.5);
    options.radiusMeters = 1000.;
    options.cachePath = testRoot / "cache";
    options.downloadCallback =
        [&downloadCount](
            std::string_view url,
            const std::filesystem::path& outputPath,
            const DownTool::CopernicusDemCancellationCallback&,
            std::string& error)
        {
            if (url.find("N39_00_E116_00") == std::string_view::npos)
            {
                error = "测试 URL 的分幅名称不正确。";
                return false;
            }
            downloadCount.fetch_add(1, std::memory_order_relaxed);
            cv::Mat image(4, 4, CV_32FC1, cv::Scalar(100.F));
            image.at<float>(2, 2) = 50.F;
            std::vector<unsigned char> encoded;
            if (!cv::imencode(".tif", image, encoded))
            {
                error = "无法写入测试高程 TIFF。";
                return false;
            }
            std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(encoded.data()),
                         static_cast<std::streamsize>(encoded.size()));
            if (!output.good())
            {
                error = "无法保存测试高程 TIFF。";
                return false;
            }
            return true;
        };

    std::string error;
    const std::shared_ptr<DownTool::CopernicusDemData> data =
        DownTool::CopernicusDemData::Prepare(options, error);
    if (!postprocess_test::Require(data != nullptr, "DEM 准备失败：" + error) ||
        !postprocess_test::Require(
            downloadCount.load(std::memory_order_relaxed) == 1,
            "DEM 首次准备没有下载目标分幅") ||
        !postprocess_test::Require(
            data->GetMinimumElevationMeters() == 50.,
            "DEM 范围最低高程计算不正确") ||
        !postprocess_test::Require(
            data->Sample(DownTool::LonLat(116.5, 39.5)).has_value(),
            "DEM 中心高程采样失败"))
    {
        return false;
    }

    const std::optional<double> centerElevation =
        data->Sample(DownTool::LonLat(116.5, 39.5));
    if (!postprocess_test::Require(
            centerElevation && *centerElevation > 80. && *centerElevation < 90.,
            "DEM 双线性采样结果不正确"))
    {
        return false;
    }

    const std::shared_ptr<DownTool::CopernicusDemData> cachedData =
        DownTool::CopernicusDemData::Prepare(options, error);
    if (!postprocess_test::Require(cachedData != nullptr, "DEM 缓存读取失败：" + error) ||
        !postprocess_test::Require(
            downloadCount.load(std::memory_order_relaxed) == 1,
            "DEM 缓存命中后仍重复下载"))
    {
        return false;
    }

    if (std::getenv("DOMDOWNTOOL_RUN_REAL_DEM_TESTS") != nullptr)
    {
        DownTool::CopernicusDemOptions realOptions;
        realOptions.centerWgs84 = DownTool::LonLat(116.3974, 39.9093);
        realOptions.radiusMeters = 500.;
        realOptions.cachePath = testRoot / "real-cache";
        const std::shared_ptr<DownTool::CopernicusDemData> realData =
            DownTool::CopernicusDemData::Prepare(realOptions, error);
        if (!postprocess_test::Require(
                realData != nullptr, "Copernicus DEM 实际下载失败：" + error) ||
            !postprocess_test::Require(
                realData->Sample(realOptions.centerWgs84).has_value(),
                "Copernicus DEM 实际数据采样失败"))
        {
            return false;
        }
    }

    return TestFlattenedRegions(testRoot) && TestFlattenAcrossDemTiles(testRoot);
}
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
