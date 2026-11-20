#include "PointLabel.h"

#include <algorithm>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkPaint.h"
#include "include/ports/SkTypeface_win.h"

namespace DownTool
{
namespace
{
SkFont labelFont(float size)
{
    static const auto face = [] {
        const auto manager = SkFontMgr_New_DirectWrite();
        return manager->matchFamilyStyle("Microsoft YaHei", SkFontStyle::Normal());
    }();
    SkFont font(face, size);
    font.setSubpixel(true);
    font.setEdging(SkFont::Edging::kAntiAlias);
    return font;
}

std::string displayName(const AmapVectorFeature& feature)
{
    std::string name;
    for (const auto& entry : feature.names)
    {
        if (entry.text.find_first_not_of(" \r\n\t") == std::string::npos)
        {
            continue;
        }
        if (entry.language.starts_with("zh"))
        {
            name = entry.text;
            break;
        }
        if (name.empty())
        {
            name = entry.text;
        }
    }
    // 百度样式文本用双反斜杠指定换行；地图标注在这里排为单行。
    if (feature.GetDataSource() == VectorDataSource::Baidu)
    {
        for (auto pos = name.find("\\\\"); pos != name.npos; pos = name.find("\\\\", pos + 1))
        {
            name.replace(pos, 2, " ");
        }
    }
    std::ranges::replace_if(
        name, [](char ch) { return ch == '\r' || ch == '\n' || ch == '\t'; }, ' ');
    return name;
}
} // namespace

std::shared_ptr<const PointLabel> PointLabel::Create(const AmapVectorFeature& feature)
{
    if (feature.geometryType != AmapVectorGeometryType::Point)
    {
        return {};
    }
    const std::string name = displayName(feature);
    const PointSymbol symbol = classifyPointSymbol(feature);
    const bool hasIcon = hasPointIcon(symbol);
    if (name.empty() && !hasIcon)
    {
        return {};
    }
    auto label = std::make_shared<PointLabel>();
    label->symbol_ = symbol;
    label->rank_ = feature.rank;
    label->categoryColor_ = GetDefaultAmapVectorRenderColor(ClassifyAmapVectorFeature(feature));
    label->size_ = symbol == PointSymbol::City ? 21.F : symbol == PointSymbol::District ? 16.F : 14.F;
    const bool textOnly = symbol == PointSymbol::District || symbol == PointSymbol::Address;
    if (symbol == PointSymbol::Address)
    {
        label->size_ = 12.F;
    }
    const SkFont font = labelFont(label->size_);
    SkRect ink = SkRect::MakeEmpty();
    const float width = font.measureText(name.data(), name.size(), SkTextEncoding::kUTF8, &ink);
    label->textX_ = textOnly ? -width / 2.F : hasIcon ? 13.F : 7.F;
    label->baseline_ = -ink.centerY();
    ink.offset(label->textX_, label->baseline_);
    label->bounds_ = ink;
    if (!textOnly)
    {
        const float r = hasIcon ? 10.F : 4.F;
        label->bounds_.join(SkRect::MakeLTRB(-r, -r, r, r));
    }
    label->bounds_.outset(2.5F, 2.5F);
    label->text_ = SkTextBlob::MakeFromString(name.c_str(), font);
    return label;
}

GisSpatialBounds PointLabel::Bounds(double x, double y) const
{
    return {x + bounds_.left(), y + bounds_.top(), x + bounds_.right(), y + bounds_.bottom()};
}

void PointLabel::Draw(SkCanvas& canvas, float x, float y, AmapVectorColor color) const
{
    SkPaint paint;
    paint.setAntiAlias(true);
    const auto setColor = [&](SkColor rgb) { paint.setColor(SkColorSetA(rgb, color.alpha)); };
    const SkColor marker = SkColorSetRGB(color.red, color.green, color.blue);
    if (hasPointIcon(symbol_))
    {
        const bool usePalette = color.red == categoryColor_.red &&
            color.green == categoryColor_.green && color.blue == categoryColor_.blue;
        SkAutoCanvasRestore restore(&canvas, true);
        canvas.translate(x, y);
        drawPointIcon(canvas, symbol_, color, usePalette);
    }
    else if (symbol_ == PointSymbol::Dot || symbol_ == PointSymbol::City)
    {
        setColor(SK_ColorWHITE);
        canvas.drawCircle(x, y, 3.8F, paint);
        setColor(marker);
        canvas.drawCircle(x, y, 2.5F, paint);
    }
    if (text_)
    {
        setColor(SkColorSetRGB(35, 39, 43));
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(3.F);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        canvas.drawTextBlob(text_, x + textX_, y + baseline_, paint);
        setColor(SK_ColorWHITE);
        paint.setStyle(SkPaint::kFill_Style);
        canvas.drawTextBlob(text_, x + textX_, y + baseline_, paint);
    }
}

PositionedPointLabel::PositionedPointLabel(std::shared_ptr<const PointLabel> label, double x,
                                           double y, AmapVectorColor color)
    : label_(std::move(label)), x_(x), y_(y), color_(color), bounds_(label_->Bounds(x, y))
{
}

void PositionedPointLabel::Draw(SkCanvas& canvas, double originX, double originY) const
{
    label_->Draw(canvas, static_cast<float>(x_ - originX), static_cast<float>(y_ - originY),
                 color_);
}

std::vector<PositionedPointLabel> PlacePointLabels(std::vector<PositionedPointLabel> candidates,
                                                   const std::function<bool()>& cancelled)
{
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto& a, const auto& b) { return a.Priority() > b.Priority(); });
    std::vector<GisSpatialBounds> bounds;
    bounds.reserve(candidates.size());
    for (const auto& item : candidates)
    {
        bounds.push_back(item.GetBounds());
    }
    GisSpatialIndex index;
    if (!index.Build(std::move(bounds), cancelled))
    {
        return {};
    }
    std::vector<bool> hidden(candidates.size());
    std::vector<size_t> overlaps;
    std::vector<PositionedPointLabel> visible;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (!hidden[i])
        {
            if (!index.Query(index.GetBounds(i), overlaps, cancelled))
            {
                return {};
            }
            for (const size_t other : overlaps)
            {
                hidden[other] = true;
            }
            visible.push_back(std::move(candidates[i]));
        }
    }
    return visible;
}
} // namespace DownTool
