#pragma once

#include <string>
#include <string_view>

namespace DownTool::detail
{
    std::string SelectSystemHttpsProxy(std::string_view proxyServers);

    std::string GetSystemHttpsProxy();
} // namespace DownTool::detail
