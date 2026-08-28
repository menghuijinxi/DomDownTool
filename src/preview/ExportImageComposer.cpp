#include "preview/ExportImageComposer.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

namespace
{
bool StopIfCanceled(
    const DomDownToolPreview::ExportCancellationCallback& cancellationRequested,
    std::string& error)
{
    if (!cancellationRequested || !cancellationRequested())
    {
        return false;
    }
    error = "导出任务已取消。";
    return true;
}

enum class ImageEncodingPolicy
{
    FileExtension,
    XyzTile
};

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

cv::Mat ReadBgraImage(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {};
    }
    std::vector<unsigned char> encoded;
    encoded.assign(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (encoded.empty())
    {
        return {};
    }

    cv::Mat image;
    try
    {
        image = cv::imdecode(encoded, cv::IMREAD_UNCHANGED);
    }
    catch (const cv::Exception&)
    {
        return {};
    }
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

bool HasTransparentPixels(const cv::Mat& image)
{
    if (image.channels() != 4)
    {
        return false;
    }
    if (image.depth() != CV_8U)
    {
        return true;
    }

    cv::Mat alphaChannel;
    cv::extractChannel(image, alphaChannel, 3);
    double minimumAlpha = 255.;
    cv::minMaxLoc(alphaChannel, &minimumAlpha, nullptr);
    return minimumAlpha < 255.;
}

bool WriteImage(const std::filesystem::path& path, const cv::Mat& image,
                ImageEncodingPolicy encodingPolicy, std::string& error)
{
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError)
    {
        error = "无法创建图像输出目录：" + directoryError.message();
        return false;
    }
    std::string extension = encodingPolicy == ImageEncodingPolicy::XyzTile ?
        (HasTransparentPixels(image) ? ".png" : ".jpg") :
        PathToUtf8(path.extension());
    if (extension.empty())
    {
        extension = ".png";
    }

    cv::Mat convertedImage;
    const cv::Mat* imageToEncode = &image;
    if ((extension == ".jpg" || extension == ".jpeg") && image.channels() == 4)
    {
        cv::cvtColor(image, convertedImage, cv::COLOR_BGRA2BGR);
        imageToEncode = &convertedImage;
    }

    std::vector<unsigned char> encoded;
    try
    {
        if (!cv::imencode(extension, *imageToEncode, encoded))
        {
            error = "无法编码合成图像：" + PathToUtf8(path);
            return false;
        }
    }
    catch (const cv::Exception& exception)
    {
        error = "编码合成图像失败：" + std::string(exception.what());
        return false;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
    {
        error = "无法打开合成图像输出：" + PathToUtf8(path);
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<std::streamsize>(encoded.size()));
    if (!output.good())
    {
        error = "无法写入合成图像：" + PathToUtf8(path);
        return false;
    }
    return true;
}
} // namespace

bool DomDownToolPreview::ExportImageComposer::CopyDirectory(
    const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error, const ExportCancellationCallback& cancellationRequested)
{
    std::error_code iteratorError;
    for (std::filesystem::recursive_directory_iterator iterator(source, iteratorError), end;
         iterator != end && !iteratorError; iterator.increment(iteratorError))
    {
        if (StopIfCanceled(cancellationRequested, error))
        {
            return false;
        }
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
    return !StopIfCanceled(cancellationRequested, error);
}

namespace
{

bool CompositeImageWithEncoding(
    const std::filesystem::path& basePath,
    const std::filesystem::path& overlayPath,
    const std::filesystem::path& outputPath,
    ImageEncodingPolicy encodingPolicy,
    std::string& error,
    const DomDownToolPreview::ExportCancellationCallback& cancellationRequested)
{
    if (StopIfCanceled(cancellationRequested, error))
    {
        return false;
    }
    cv::Mat base = ReadBgraImage(basePath);
    cv::Mat overlay = ReadBgraImage(overlayPath);
    if (base.empty())
    {
        error = "无法读取卫星底图：" + PathToUtf8(basePath);
        return false;
    }
    if (overlay.empty())
    {
        error = "无法读取 GIS 叠加图：" + PathToUtf8(overlayPath);
        return false;
    }
    if (overlay.size() != base.size())
    {
        cv::resize(overlay, overlay, base.size(), 0., 0., cv::INTER_LINEAR);
    }

    cv::Mat output(base.size(), CV_8UC4);
    for (int row = 0; row < base.rows; ++row)
    {
        if (StopIfCanceled(cancellationRequested, error))
        {
            return false;
        }
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
    return !StopIfCanceled(cancellationRequested, error) &&
        WriteImage(outputPath, output, encodingPolicy, error);
}

bool CompositeTileTreeWithEncoding(
    const std::filesystem::path& baseRoot,
    const std::filesystem::path& overlayRoot,
    const std::filesystem::path& outputRoot,
    ImageEncodingPolicy encodingPolicy,
    std::string& error,
    const DomDownToolPreview::ExportCancellationCallback& cancellationRequested)
{
    std::error_code iteratorError;
    size_t compositeCount = 0;
    for (std::filesystem::recursive_directory_iterator iterator(overlayRoot, iteratorError), end;
         iterator != end && !iteratorError; iterator.increment(iteratorError))
    {
        if (StopIfCanceled(cancellationRequested, error))
        {
            return false;
        }
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
        std::filesystem::path baseRelativePath = relativePath;
        std::filesystem::path basePath = baseRoot / baseRelativePath;
        if (!std::filesystem::exists(basePath) && relativePath.extension() == ".png")
        {
            baseRelativePath.replace_extension(".jpg");
            basePath = baseRoot / baseRelativePath;
        }
        if (std::filesystem::exists(basePath) &&
            !CompositeImageWithEncoding(
                basePath,
                iterator->path(),
                outputRoot / baseRelativePath,
                encodingPolicy,
                error,
                cancellationRequested))
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
    return !StopIfCanceled(cancellationRequested, error);
}
} // namespace

bool DomDownToolPreview::ExportImageComposer::CompositeImage(
    const std::filesystem::path& basePath, const std::filesystem::path& overlayPath,
    const std::filesystem::path& outputPath, std::string& error,
    const ExportCancellationCallback& cancellationRequested)
{
    return CompositeImageWithEncoding(
        basePath,
        overlayPath,
        outputPath,
        ImageEncodingPolicy::FileExtension,
        error,
        cancellationRequested);
}

bool DomDownToolPreview::ExportImageComposer::CompositeTileTree(
    const std::filesystem::path& baseRoot, const std::filesystem::path& overlayRoot,
    const std::filesystem::path& outputRoot, std::string& error,
    const ExportCancellationCallback& cancellationRequested)
{
    return CompositeTileTreeWithEncoding(
        baseRoot,
        overlayRoot,
        outputRoot,
        ImageEncodingPolicy::FileExtension,
        error,
        cancellationRequested);
}

bool DomDownToolPreview::ExportImageComposer::CompositeXyzTileTree(
    const std::filesystem::path& baseRoot, const std::filesystem::path& overlayRoot,
    const std::filesystem::path& outputRoot, std::string& error,
    const ExportCancellationCallback& cancellationRequested)
{
    return CompositeTileTreeWithEncoding(
        baseRoot,
        overlayRoot,
        outputRoot,
        ImageEncodingPolicy::XyzTile,
        error,
        cancellationRequested);
}
