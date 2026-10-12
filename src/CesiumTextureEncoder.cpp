#include "CesiumTextureEncoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

#include "ktx.h"
#include "opencv2/imgproc.hpp"

#include "CesiumExportSupport.h"

namespace
{
constexpr uint32_t kRgba8Srgb = 43; // Vulkan VK_FORMAT_R8G8B8A8_SRGB。
constexpr uint32_t kEtc1sQuality = 128;

struct TextureDeleter
{
    void operator()(ktxTexture2* texture) const
    {
        ktxTexture_Destroy(ktxTexture(texture));
    }
};

bool checkKtx(KTX_error_code result, std::string_view operation, std::string& error)
{
    if (result == KTX_SUCCESS)
    {
        return true;
    }
    error = std::string(operation) + "：" + ktxErrorString(result);
    return false;
}

float decodeSrgb(float value)
{
    return value <= 0.04045f ? value / 12.92f :
        std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float encodeSrgb(float value)
{
    return value <= 0.0031308f ? value * 12.92f :
        1.055f * std::pow(value, 1.f / 2.4f) - 0.055f;
}

cv::Mat linearPremultiplied(const cv::Mat& rgba)
{
    cv::Mat linear;
    rgba.convertTo(linear, CV_32FC4, 1. / 255.);
    for (int row = 0; row < linear.rows; ++row)
    {
        auto* pixels = linear.ptr<cv::Vec4f>(row);
        for (int column = 0; column < linear.cols; ++column)
        {
            auto& pixel = pixels[column];
            for (int channel = 0; channel < 3; ++channel)
            {
                pixel[channel] = decodeSrgb(pixel[channel]) * pixel[3];
            }
        }
    }
    return linear;
}

cv::Mat srgbUnpremultiplied(const cv::Mat& linear)
{
    cv::Mat rgba(linear.size(), CV_8UC4);
    for (int row = 0; row < linear.rows; ++row)
    {
        const auto* input = linear.ptr<cv::Vec4f>(row);
        auto* output = rgba.ptr<cv::Vec4b>(row);
        for (int column = 0; column < linear.cols; ++column)
        {
            const auto& pixel = input[column];
            for (int channel = 0; channel < 3; ++channel)
            {
                const float value = pixel[3] > 0.f ? pixel[channel] / pixel[3] : 0.f;
                output[column][channel] = cv::saturate_cast<unsigned char>(
                    encodeSrgb(std::clamp(value, 0.f, 1.f)) * 255.f);
            }
            output[column][3] = cv::saturate_cast<unsigned char>(pixel[3] * 255.f);
        }
    }
    return rgba;
}
} // namespace

DownTool::cesium::TextureEncoder::TextureEncoder(
    bool mipmaps, std::function<bool()> cancellation)
    : mipmaps_(mipmaps), cancellation_(std::move(cancellation))
{
}

bool DownTool::cesium::TextureEncoder::write(
    const cv::Mat& image, const std::filesystem::path& path, std::string& error) const
{
    const auto canceled = [&]
    {
        if (cancellation_ && cancellation_())
        {
            error = "建筑 KTX2 贴图编码已取消。";
            return true;
        }
        return false;
    };
    if (canceled())
    {
        return false;
    }
    if (image.empty() || image.depth() != CV_8U ||
        (image.channels() != 1 && image.channels() != 3 && image.channels() != 4))
    {
        error = "建筑 KTX2 编码需要有效的 8 位灰度、RGB 或 RGBA 图片。";
        return false;
    }
    try
    {
        cv::Mat rgba;
        const int conversion = image.channels() == 4 ? cv::COLOR_BGRA2RGBA :
            image.channels() == 3 ? cv::COLOR_BGR2RGBA : cv::COLOR_GRAY2RGBA;
        cv::cvtColor(image, rgba, conversion);
        // KHR_texture_basisu 要求基础层边长为 4 的倍数，不做额外分辨率封顶。
        const cv::Size aligned((rgba.cols + 3) / 4 * 4, (rgba.rows + 3) / 4 * 4);
        if (aligned != rgba.size())
        {
            cv::resize(rgba, rgba, aligned, 0., 0., cv::INTER_LINEAR);
        }
        ktxTextureCreateInfo info{};
        info.vkFormat = kRgba8Srgb;
        info.baseWidth = uint32_t(rgba.cols);
        info.baseHeight = uint32_t(rgba.rows);
        info.baseDepth = 1;
        info.numDimensions = 2;
        info.numLevels = mipmaps_ ?
            uint32_t(std::floor(std::log2(std::max(rgba.cols, rgba.rows)))) + 1 : 1;
        info.numLayers = 1;
        info.numFaces = 1;
        // 压缩贴图不能依赖 WebGL 现场生成 mipmap，必须在编码前写入各层。
        info.generateMipmaps = KTX_FALSE;
        ktxTexture2* created = nullptr;
        if (!checkKtx(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &created),
                "创建 KTX2 贴图失败", error))
        {
            return false;
        }
        const std::unique_ptr<ktxTexture2, TextureDeleter> texture(created);
        cv::Mat linear;
        if (mipmaps_)
        {
            linear = linearPremultiplied(rgba);
        }
        for (uint32_t level = 0; level < info.numLevels; ++level)
        {
            if (canceled() ||
                !checkKtx(ktxTexture_SetImageFromMemory(ktxTexture(texture.get()),
                    level, 0, 0, rgba.data, rgba.total() * rgba.elemSize()),
                    "写入 KTX2 像素失败", error))
            {
                return false;
            }
            if (level + 1 < info.numLevels)
            {
                cv::resize(linear, linear,
                    cv::Size(std::max(1, linear.cols / 2), std::max(1, linear.rows / 2)),
                    0., 0., cv::INTER_AREA);
                rgba = srgbUnpremultiplied(linear);
            }
        }
        constexpr char orientation[] = "rd";
        if (!checkKtx(ktxHashList_AddKVPair(&texture->kvDataHead, KTX_ORIENTATION_KEY,
                sizeof(orientation), orientation), "设置 KTX2 方向失败", error))
        {
            return false;
        }
        ktxBasisParams parameters{};
        parameters.structSize = sizeof(parameters);
        parameters.compressionLevel = KTX_ETC1S_DEFAULT_COMPRESSION_LEVEL;
        parameters.qualityLevel = kEtc1sQuality;
        parameters.threadCount = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
        if (!checkKtx(ktxTexture2_CompressBasisEx(texture.get(), &parameters),
                "ETC1S 压缩失败", error) || canceled())
        {
            return false;
        }
        ktx_uint8_t* allocated = nullptr;
        ktx_size_t size = 0;
        if (!checkKtx(ktxTexture_WriteToMemory(ktxTexture(texture.get()), &allocated, &size),
                "KTX2 序列化失败", error))
        {
            return false;
        }
        const std::unique_ptr<ktx_uint8_t, decltype(&std::free)> bytes(allocated, &std::free);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.get()), std::streamsize(size));
        stream.flush();
        if (!stream.good())
        {
            error = "建筑 KTX2 贴图写入失败：" + pathUtf8(path);
            return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = "建筑 KTX2 贴图编码失败：" + std::string(exception.what());
        return false;
    }
}
