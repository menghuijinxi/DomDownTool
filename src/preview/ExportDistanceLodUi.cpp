#include "preview/ExportDistanceLodUi.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "fmt/format.h"

namespace
{
std::optional<uint32_t> ParseRuleIndex(std::string_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    uint32_t result = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc() || end != value.data() + value.size())
    {
        return std::nullopt;
    }
    return result;
}

std::optional<double> ParseRuleRadius(std::string_view value)
{
    if (value.empty())
    {
        return std::nullopt;
    }
    double result = 0.;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc() || end != value.data() + value.size() ||
        !std::isfinite(result) || result < 0.)
    {
        return std::nullopt;
    }
    return result;
}

std::string RulesHtml(
    const DomDownToolPreview::ExportComponent& component,
    const std::vector<DownTool::FbxDistanceLodRule>& rules, bool texture)
{
    const std::string mode = texture ? "texture" : "triangles";
    const bool enabled = texture || component.exportElevation;
    const std::string disabled = enabled ? "" : " disabled";
    std::string html = fmt::format(
        "<div class=\"export-lod-section\"><div class=\"export-lod-head\">"
        "<span>{}</span><button id=\"export-act-lod-{}-add__{}\" "
        "class=\"export-icon-button\" type=\"button\" data-action=\"export-composer\" "
        "title=\"添加距离档位\" aria-label=\"添加距离档位\"{}>&#43;</button></div>"
        "<div class=\"export-lod-columns\"><span>半径（米）</span><span>{}</span>"
        "<span></span></div>",
        texture ? "贴图距离档位" : "面数距离档位", mode, component.id, disabled,
        texture ? "贴图尺寸" : "三角面上限（0不限）");
    for (size_t index = 0; index < rules.size(); ++index)
    {
        const auto& rule = rules[index];
        html += fmt::format(
            "<div class=\"export-lod-row\">"
            "<input id=\"export-act-lod-{}-radius__{}__{}\" type=\"number\" "
            "min=\"0\" step=\"1\" value=\"{}\" aria-label=\"半径（米）\"{}>",
            mode, component.id, index, rule.GetRadiusMeters(), disabled);
        if (texture)
        {
            html += fmt::format(
                "<select id=\"export-act-lod-texture-value__{}__{}\" "
                "class=\"export-select\" aria-label=\"贴图尺寸\">", component.id, index);
            for (uint32_t size : {128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u})
            {
                html += fmt::format("<option value=\"{}\"{}>{}</option>",
                    size, size == rule.GetValue() ? " selected" : "", size);
            }
            html += "</select>";
        }
        else
        {
            html += fmt::format(
                "<input id=\"export-act-lod-triangles-value__{}__{}\" type=\"number\" "
                "min=\"0\" max=\"4294967295\" step=\"1\" value=\"{}\" "
                "aria-label=\"三角面上限\"{}>",
                component.id, index, rule.GetValue(), disabled);
        }
        html += fmt::format(
            "<button id=\"export-act-lod-{}-remove__{}__{}\" class=\"export-icon-button\" "
            "type=\"button\" data-action=\"export-composer\" "
            "title=\"删除距离档位\" aria-label=\"删除距离档位\"{}>&#215;</button></div>",
            mode, component.id, index, disabled);
    }
    html += "</div>";
    return html;
}
}

std::string DomDownToolPreview::ExportDistanceLodHtml(const ExportComponent& component)
{
    return RulesHtml(component, component.GetDistanceLod().GetTextureRules(), true) +
        RulesHtml(component, component.GetDistanceLod().GetTriangleRules(), false);
}

bool DomDownToolPreview::UpdateExportDistanceLod(
    ExportComponent& component, std::string_view verb,
    std::string_view rowIndex, std::string_view value)
{
    const bool texture = verb.starts_with("lod-texture-");
    const std::string_view prefix = texture ? "lod-texture-" : "lod-triangles-";
    if (!verb.starts_with(prefix))
    {
        return false;
    }
    const std::string_view command = verb.substr(prefix.size());
    auto settings = component.GetDistanceLod();
    auto rules = texture ? settings.GetTextureRules() : settings.GetTriangleRules();
    if (command == "add")
    {
        double radius = 0.;
        for (const auto& rule : rules)
        {
            radius = std::max(radius, rule.GetRadiusMeters());
        }
        rules.emplace_back(radius + 10000.,
            texture ? std::min(component.textureSize, 2048u) : 10000u);
    }
    else
    {
        const auto index = ParseRuleIndex(rowIndex);
        if (!index || *index >= rules.size())
        {
            return true;
        }
        if (command == "remove")
        {
            rules.erase(rules.begin() + *index);
        }
        else if (command == "radius")
        {
            if (const auto radius = ParseRuleRadius(value))
            {
                rules[*index].SetRadiusMeters(*radius);
            }
        }
        else if (command == "value")
        {
            if (const auto number = ParseRuleIndex(value))
            {
                rules[*index].SetValue(*number);
            }
        }
        else
        {
            return false;
        }
    }
    if (texture)
    {
        settings.SetTextureRules(rules);
    }
    else
    {
        settings.SetTriangleRules(rules);
    }
    component.SetDistanceLod(settings);
    return true;
}
