#include "preview/ExportImageComposer.h"

#include <cstdint>
#include <system_error>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

namespace
{
std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

cv::Mat ReadBgraImage(const std::filesystem::path& path)
{
    cv::Mat image = cv::imread(PathToUtf8(path), cv::IMREAD_UNCHANGED);
    if (image.empty())
    {
        return {};
    }
    if (image.channels() == 4)
    {
        return image;
    }

    cv::Mat bgra;
    if (image.channels() == 3)
    {
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    }
    else if (image.channels() == 1)
    {
        cv::cvtColor(image, bgra, cv::COLOR_GRAY2BGRA);
    }
    return bgra;
}

bool WriteImage(const std::filesystem::path& path, const cv::Mat& image,
                std::string& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError)
    {
        error = "无法创建图像输出目录：" + directoryError.message();
        return false;
    }
    try
    {
        if (!cv::imwrite(PathToUtf8(path), image))
        {
            error = "无法写入合成图像：" + PathToUtf8(path);
            return false;
        }
    }
    catch (const cv::Exception& exception)
    {
        error = "写入合成图像失败：" + std::string(exception.what());
        return false;
    }
    return true;
}
} // namespace

bool DomDownToolPreview::ExportImageComposer::CopyDirectory(
    const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error)
{
    std::error_code iteratorError;
    for (std::filesystem::recursive_directory_iterator iterator(source, iteratorError), end;
         iterator != end && !iteratorError; iterator.increment(iteratorError))
    {
        if (!iterator->is_regular_file())
        {
            continue;
        }
        const std::filesystem::path relativePath =
            std::filesystem::relative(iterator->path(), source, iteratorError);
        if (iteratorError)
        {
            break;
        }

        const std::filesystem::path outputPath = destination / relativePath;
        std::error_code copyError;
        std::filesystem::create_directories(outputPath.parent_path(), copyError);
        if (!copyError)
        {
            std::filesystem::copy_file(
                iterator->path(), outputPath,
                std::filesystem::copy_options::overwrite_existing, copyError);
        }
        if (copyError)
        {
            error = "复制卫星图输出失败：" + copyError.message();
            return false;
        }
    }
    if (iteratorError)
    {
        error = "遍历卫星图输出失败：" + iteratorError.message();
        return false;
    }
    return true;
}

bool DomDownToolPreview::ExportImageComposer::CompositeImage(
    const std::filesystem::path& basePath, const std::filesystem::path& overlayPath,
    const std::filesystem::path& outputPath, std::string& error)
{
    cv::Mat base = ReadBgraImage(basePath);
    cv::Mat overlay = ReadBgraImage(overlayPath);
    if (base.empty() || overlay.empty())
    {
        error = "无法读取待合成图像。";
        return false;
    }
    if (overlay.size() != base.size())
    {
        cv::resize(overlay, overlay, base.size(), 0., 0., cv::INTER_LINEAR);
    }

    cv::Mat output(base.size(), CV_8UC4);
    for (int row = 0; row < base.rows; ++row)
    {
        const cv::Vec4b* basePixels = base.ptr<cv::Vec4b>(row);
        const cv::Vec4b* overlayPixels = overlay.ptr<cv::Vec4b>(row);
        cv::Vec4b* outputPixels = output.ptr<cv::Vec4b>(row);
        for (int column = 0; column < base.cols; ++column)
        {
            const uint32_t sourceAlpha = overlayPixels[column][3];
            const uint32_t baseAlpha = basePixels[column][3];
            const uint32_t inverseSourceAlpha = 255u - sourceAlpha;
            const uint32_t outputAlphaScale =
                sourceAlpha * 255u + baseAlpha * inverseSourceAlpha;
            if (outputAlphaScale == 0)
            {
                outputPixels[column] = cv::Vec4b(0, 0, 0, 0);
                continue;
            }
            for (size_t channel = 0; channel < 3; ++channel)
            {
                outputPixels[column][channel] = static_cast<uint8_t>(
                    (static_cast<uint32_t>(overlayPixels[column][channel]) *
                         sourceAlpha * 255u +
                     static_cast<uint32_t>(basePixels[column][channel]) *
                         baseAlpha * inverseSourceAlpha + outputAlphaScale / 2u) /
                    outputAlphaScale);
            }
            outputPixels[column][3] = static_cast<uint8_t>(
                (outputAlphaScale + 127u) / 255u);
        }
    }
    return WriteImage(outputPath, output, error);
}

bool DomDownToolPreview::ExportImageComposer::CompositeTileTree(
    const std::filesystem::path& baseRoot, const std::filesystem::path& overlayRoot,
    const std::filesystem::path& outputRoot, std::string& error)
{
    std::error_code iteratorError;
    size_t compositeCount = 0;
    for (std::filesystem::recursive_directory_iterator iterator(overlayRoot, iteratorError), end;
         iterator != end && !iteratorError; iterator.increment(iteratorError))
    {
        if (!iterator->is_regular_file() || iterator->path().extension() != ".png")
        {
            continue;
        }
        const std::filesystem::path relativePath =
            std::filesystem::relative(iterator->path(), overlayRoot, iteratorError);
        if (iteratorError)
        {
            break;
        }
        const std::filesystem::path basePath = baseRoot / relativePath;
        if (std::filesystem::exists(basePath) &&
            !CompositeImage(basePath, iterator->path(), outputRoot / relativePath, error))
        {
            return false;
        }
        if (std::filesystem::exists(basePath))
        {
            ++compositeCount;
        }
    }
    if (iteratorError)
    {
        error = "遍历 GIS 瓦片失败：" + iteratorError.message();
        return false;
    }
    if (compositeCount == 0)
    {
        error = "GIS 叠加没有找到匹配的卫星图瓦片。";
        return false;
    }
    return true;
}
