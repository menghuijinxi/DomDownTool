#pragma once

#include <memory>
#include <vector>

#include "include/core/SkTextBlob.h"

#include "AmapVectorPngDownloader.h"
#include "GisSpatialIndex.h"
#include "PointSymbol.h"

class SkCanvas;

namespace DownTool
{
// 字形在准备矢量数据时生成，预览重绘和多线程瓦片导出只读共享。
class PointLabel
{
  public:
    static std::shared_ptr<const PointLabel> Create(const AmapVectorFeature& feature);
    GisSpatialBounds Bounds(double x, double y) const;
    void Draw(SkCanvas& canvas, float x, float y, AmapVectorColor color) const;
    auto Priority() const
    {
        return std::pair{size_, rank_};
    }

  private:
    PointSymbol symbol_ = PointSymbol::Dot;
    AmapVectorColor categoryColor_{};
    float size_ = 14.F;
    uint32_t rank_ = 0;
    sk_sp<SkTextBlob> text_;
    float textX_ = 0.F;
    float baseline_ = 0.F;
    SkRect bounds_{};
};

class PositionedPointLabel
{
  public:
    PositionedPointLabel(std::shared_ptr<const PointLabel> label, double x, double y,
                         AmapVectorColor color);
    const PointLabel& GetLabel() const { return *label_; }
    AmapVectorColor GetColor() const { return color_; }
    double GetX() const { return x_; }
    double GetY() const { return y_; }
    const GisSpatialBounds& GetBounds() const
    {
        return bounds_;
    }
    auto Priority() const
    {
        return label_->Priority();
    }
    void Draw(SkCanvas& canvas, double originX, double originY) const;

  private:
    std::shared_ptr<const PointLabel> label_;
    double x_;
    double y_;
    AmapVectorColor color_;
    GisSpatialBounds bounds_;
};

std::vector<PositionedPointLabel> PlacePointLabels(std::vector<PositionedPointLabel> candidates,
                                                   const std::function<bool()>& cancelled = {});
} // namespace DownTool
