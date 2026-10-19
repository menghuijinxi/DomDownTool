#include <stdexcept>

#include "preview/AmapWebViewSearch.h"
#include "preview/TianDiTuMultidateWebViewSession.h"

// 范围编辑回归使用生产控制器，但不启动与该功能无关的登录窗口和网络线程。
namespace DomDownToolPreview
{
AmapWebViewSearch::AmapWebViewSearch() = default;
AmapWebViewSearch::~AmapWebViewSearch() = default;

PlaceSearchHttpResponse AmapWebViewSearch::get(const std::string&)
{
    throw std::logic_error("范围编辑测试意外发起地点查询。");
}

void AmapWebViewSearch::cancelCurrent() {}

TianDiTuMultidateWebViewSession::TianDiTuMultidateWebViewSession() = default;
TianDiTuMultidateWebViewSession::~TianDiTuMultidateWebViewSession() = default;

TianDiTuMultidateCookieResponse TianDiTuMultidateWebViewSession::requestSessionCookie()
{
    throw std::logic_error("范围编辑测试意外发起登录。");
}

TianDiTuMultidateFetchResponse TianDiTuMultidateWebViewSession::fetchUrl(std::string_view)
{
    throw std::logic_error("范围编辑测试意外请求时间线。");
}

TianDiTuMultidateFetchResponse TianDiTuMultidateWebViewSession::fetchBinaryUrl(std::string_view)
{
    throw std::logic_error("范围编辑测试意外请求地图网络瓦片。");
}

void TianDiTuMultidateWebViewSession::cancelCurrent() {}
} // namespace DomDownToolPreview
