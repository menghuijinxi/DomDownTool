#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "AmapVectorDecoder.h"
#include "GeographicPolygon.h"

namespace DownTool
{
class ShapefileImportOptions
{
  public:
    const std::filesystem::path& GetPath() const { return path_; }
    void SetPath(const std::filesystem::path& value) { path_ = value; }
    const std::string& GetLayerName() const { return layerName_; }
    void SetLayerName(const std::string& value) { layerName_ = value; }
    const std::string& GetHeightField() const { return heightField_; }
    void SetHeightField(const std::string& value) { heightField_ = value; }

  protected:
    std::filesystem::path path_;
    std::string layerName_;
    // 空字符串自动识别米制高度字段；"-" 表示不使用高度字段。
    std::string heightField_;
};

class ShapefileInfo
{
  public:
    AmapVectorGeometryType GetGeometryType() const { return geometryType_; }
    void SetGeometryType(AmapVectorGeometryType value) { geometryType_ = value; }
    const GeographicBounds& GetBounds() const { return bounds_; }
    void SetBounds(const GeographicBounds& value) { bounds_ = value; }
    const std::vector<std::string>& GetNumericFields() const { return numericFields_; }
    void SetNumericFields(const std::vector<std::string>& value) { numericFields_ = value; }
    int GetFeatureCount() const { return featureCount_; }
    void SetFeatureCount(int value) { featureCount_ = value; }

  protected:
    AmapVectorGeometryType geometryType_ = AmapVectorGeometryType::Point;
    GeographicBounds bounds_;
    std::vector<std::string> numericFields_;
    int featureCount_ = 0;
};

class Wgs84ShapefileReader
{
  public:
    static bool Inspect(const std::filesystem::path& path, ShapefileInfo& info, std::string& error);
    static bool Read(const ShapefileImportOptions& options, AmapVectorDecodedData& decoded,
                     std::string& error, const std::function<bool()>& cancellationRequested = {},
                     const std::function<void(size_t, size_t)>& progressCallback = {});
};
} // namespace DownTool
