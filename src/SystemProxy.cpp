#include "SystemProxy.h"

#include <algorithm>
#include <cctype>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace
{
    bool isHttpsScheme(std::string_view scheme)
    {
        constexpr std::string_view kHttps = "https";
        return scheme.size() == kHttps.size() &&
               std::equal(scheme.begin(), scheme.end(), kHttps.begin(),
                          [](unsigned char left, unsigned char right) {
                              return std::tolower(left) == right;
                          });
    }

    std::string httpProxyAddress(std::string_view address)
    {
        if (address.empty() || address.find("://") != std::string_view::npos)
        {
            return std::string(address);
        }
        return "http://" + std::string(address);
    }
} // namespace

std::string DownTool::detail::SelectSystemHttpsProxy(std::string_view proxyServers)
{
    std::string_view defaultProxy;
    while (!proxyServers.empty())
    {
        const size_t begin = proxyServers.find_first_not_of(" \t\r\n;");
        if (begin == std::string_view::npos)
        {
            break;
        }
        proxyServers.remove_prefix(begin);
        const size_t end = proxyServers.find_first_of(" \t\r\n;");
        const std::string_view entry = proxyServers.substr(0, end);
        const size_t separator = entry.find('=');
        if (separator == std::string_view::npos)
        {
            if (defaultProxy.empty())
            {
                defaultProxy = entry;
            }
        }
        else if (isHttpsScheme(entry.substr(0, separator)))
        {
            return httpProxyAddress(entry.substr(separator + 1));
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        proxyServers.remove_prefix(end);
    }
    return httpProxyAddress(defaultProxy);
}

std::string DownTool::detail::GetSystemHttpsProxy()
{
#ifdef _WIN32
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG configuration{};
    const bool loaded = WinHttpGetIEProxyConfigForCurrentUser(&configuration) != FALSE;
    const std::unique_ptr<void, decltype(&GlobalFree)> proxy(configuration.lpszProxy, &GlobalFree);
    const std::unique_ptr<void, decltype(&GlobalFree)> bypass(configuration.lpszProxyBypass,
                                                              &GlobalFree);
    const std::unique_ptr<void, decltype(&GlobalFree)> autoConfig(configuration.lpszAutoConfigUrl,
                                                                  &GlobalFree);
    // 系统关闭手动代理时不会返回代理地址，不使用残留配置或写死的本机端口。
    if (!loaded || !proxy)
    {
        return {};
    }

    const int byteCount = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, configuration.lpszProxy, -1, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 1)
    {
        return {};
    }
    std::string proxyServers(static_cast<size_t>(byteCount), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, configuration.lpszProxy, -1,
                            proxyServers.data(), byteCount, nullptr, nullptr) == 0)
    {
        return {};
    }
    proxyServers.pop_back();
    return SelectSystemHttpsProxy(proxyServers);
#else
    return {};
#endif
}
