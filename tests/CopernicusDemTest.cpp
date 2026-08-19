#include "CopernicusDem.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/core/mat.hpp"
#include "PostProcessTestSupport.h"

namespace
{
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

    std::cout << "CopernicusDemTest passed\n";
    return true;
}
}

int main()
{
    return postprocess_test::RunGuarded(RunTest);
}
