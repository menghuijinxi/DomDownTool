#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "webview/webview.h"

#include "UiBridge.h"

#ifndef DOMDOWNTOOL_UI_DIR
#define DOMDOWNTOOL_UI_DIR "."
#endif

namespace
{
    bool ShouldEscapeFileUrlByte(unsigned char character)
    {
        if (std::isalnum(character) != 0)
        {
            return false;
        }

        switch (character)
        {
        case '/':
        case ':':
        case '-':
        case '_':
        case '.':
        case '~':
            return false;
        default:
            return true;
        }
    }

    std::string BuildFileUrl(const std::filesystem::path& path)
    {
        const std::filesystem::path absolutePath = std::filesystem::absolute(path);
        const std::u8string pathText = absolutePath.generic_u8string();

        std::ostringstream encodedPath;
        encodedPath << std::uppercase << std::hex << std::setfill('0');
        for (char8_t pathByte : pathText)
        {
            const unsigned char character = static_cast<unsigned char>(pathByte);
            if (ShouldEscapeFileUrlByte(character))
            {
                encodedPath << '%' << std::setw(2) << static_cast<int>(character);
                continue;
            }

            encodedPath << static_cast<char>(character);
        }

#if defined(_WIN32)
        return "file:///" + encodedPath.str();
#else
        return "file://" + encodedPath.str();
#endif
    }

    std::filesystem::path GetUiIndexPath()
    {
        return std::filesystem::path(DOMDOWNTOOL_UI_DIR) / "index.html";
    }

    int RunUi()
    {
        const std::filesystem::path indexPath = GetUiIndexPath();
        if (!std::filesystem::exists(indexPath))
        {
            throw std::runtime_error("UI 资源不存在：" + indexPath.string());
        }

        webview::webview window(false, nullptr);
        DomDownToolUi::UiBridge bridge;
        window.set_title("DomDownTool");
        window.set_size(1440, 900, WEBVIEW_HINT_NONE);
        window.bind("domDownToolGetMapSources",
                    [&bridge](const std::string&) { return bridge.GetMapSourcesJson(); });
        window.bind("domDownToolGetQueueSnapshot",
                    [&bridge](const std::string&) { return bridge.GetQueueSnapshotJson(); });
        window.bind("domDownToolEstimatePreview", [&bridge](const std::string& requestJson)
                    { return bridge.EstimatePreviewJson(requestJson); });
        window.bind("domDownToolAddDownloadJob", [&bridge](const std::string& requestJson)
                    { return bridge.AddDownloadJobJson(requestJson); });
        window.navigate(BuildFileUrl(indexPath));
        window.run();
        return 0;
    }

    int ReportStartupError(const std::string& message)
    {
#if defined(_WIN32)
        MessageBoxA(nullptr, message.c_str(), "DomDownTool UI", MB_ICONERROR | MB_OK);
#else
        std::fprintf(stderr, "%s\n", message.c_str());
#endif
        return 1;
    }
} // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main()
#endif
{
    try
    {
        return RunUi();
    }
    catch (const std::exception& exception)
    {
        return ReportStartupError(exception.what());
    }
}
