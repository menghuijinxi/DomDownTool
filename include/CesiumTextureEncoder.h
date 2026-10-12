#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace cv
{
class Mat;
}

namespace DownTool::cesium
{
class TextureEncoder
{
public:
    explicit TextureEncoder(bool mipmaps, std::function<bool()> cancellation = {});
    [[nodiscard]] bool write(
        const cv::Mat& image, const std::filesystem::path& path, std::string& error) const;

protected:
    bool mipmaps_ = false;
    std::function<bool()> cancellation_;
};
} // namespace DownTool::cesium
