#include "preview/PreviewController.h"

#include <algorithm>
#include <string>
#include <utility>

#include "skui_runtime.h"

void DomDownToolPreview::PreviewController::RefreshWaybackCatalog(skui::Runtime& runtime)
{
    if (waybackCatalog_.Refresh())
    {
        waybackCatalogRequested_ = true;
        waybackStatus_ = "正在加载发布版本...";
    }
    UpdateWaybackUi(runtime, renderer_.activeSource());
    requestRedraw();
}

void DomDownToolPreview::PreviewController::ProcessWaybackCatalog()
{
    const auto result = waybackCatalog_.TakeCompleted();
    if (!result)
    {
        return;
    }
    if (!result->GetError().empty())
    {
        waybackStatus_ = "加载失败，可重试：" + result->GetError();
        return;
    }
    waybackReleases_ = result->GetReleases();
    const auto selected =
        std::find_if(waybackReleases_.begin(), waybackReleases_.end(), [this](const auto& release) {
            return release.GetNumber() == waybackSelectedNumber_;
        });
    const auto& release = selected != waybackReleases_.end() ? *selected : waybackReleases_.front();
    if (renderer_.SetWaybackRelease(release))
    {
        waybackSelectedNumber_ = release.GetNumber();
        waybackStatus_ = std::to_string(waybackReleases_.size()) + " 个发布版本";
    }
}

void DomDownToolPreview::PreviewController::SelectWaybackRelease(skui::Runtime& runtime,
                                                                 std::string_view number)
{
    const auto selected =
        std::find_if(waybackReleases_.begin(), waybackReleases_.end(),
                     [number](const auto& release) { return release.GetNumber() == number; });
    if (selected != waybackReleases_.end() && renderer_.SetWaybackRelease(*selected))
    {
        waybackSelectedNumber_ = selected->GetNumber();
        updateUi(runtime);
        requestRedraw();
    }
}

void DomDownToolPreview::PreviewController::UpdateWaybackUi(skui::Runtime& runtime,
                                                            const MapSourceSummary& source)
{
    const bool visible = source.id == "arcgis.wayback";
    runtime.setVisibleById("wayback-panel", visible);
    if (!visible)
    {
        return;
    }
    const bool loading = waybackCatalog_.IsLoading();
    std::string html = "<select id=\"wayback-release\" aria-label=\"Wayback 发布日期\"";
    if (waybackReleases_.empty())
    {
        html += " disabled><option value=\"\">暂无版本</option>";
    }
    else
    {
        html += ">";
        for (const auto& release : waybackReleases_)
        {
            html +=
                "<option value=\"" + release.GetNumber() + "\">" + release.GetDate() + "</option>";
        }
    }
    html += "</select>";
    // 仅目录变化时重建，常规瓦片状态刷新不能打断展开的日期菜单。
    if (html != lastWaybackSelectHtml_)
    {
        runtime.replaceHtmlById("wayback-release", html);
        lastWaybackSelectHtml_ = std::move(html);
        lastWaybackSelectedNumber_.clear();
    }
    if (waybackSelectedNumber_ != lastWaybackSelectedNumber_)
    {
        runtime.setValueById("wayback-release", waybackSelectedNumber_);
        lastWaybackSelectedNumber_ = waybackSelectedNumber_;
    }
    runtime.setTextById("wayback-status", waybackStatus_);
    if (loading)
    {
        runtime.setAttributeById("wayback-refresh", "disabled", "disabled");
    }
    else
    {
        runtime.removeAttributeById("wayback-refresh", "disabled");
    }
}
