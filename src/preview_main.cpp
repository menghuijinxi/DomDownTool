#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "skui_win32_app.h"

#include "preview/PreviewController.h"
#include "preview/SearchTrace.h"

namespace
{
std::filesystem::path executableDirectory()
{
    std::vector<wchar_t> pathBuffer(32768);
    const DWORD pathLength =
        GetModuleFileNameW(nullptr, pathBuffer.data(), static_cast<DWORD>(pathBuffer.size()));
    if (pathLength == 0 || pathLength >= pathBuffer.size())
    {
        return {};
    }

    return std::filesystem::path(std::wstring(pathBuffer.data(), pathLength)).parent_path();
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const std::filesystem::path applicationRoot = executableDirectory();
    if (applicationRoot.empty())
    {
        MessageBoxW(nullptr, L"无法确定程序运行目录。", L"DomDownTool", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }

    std::error_code pathError;
    std::filesystem::current_path(applicationRoot, pathError);
    if (pathError)
    {
        MessageBoxW(nullptr, L"无法确定程序运行目录。", L"DomDownTool", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }

    DomDownToolPreview::initializeSearchTrace(applicationRoot / "logs" /
                                              "amap-search.log");
    DomDownToolPreview::writeSearchTrace("App", "preview_starting");

    const std::filesystem::path documentPath =
        applicationRoot / "resources" / "preview" / "index.html";

    DomDownToolPreview::PreviewController controller;
    skui::win32::WindowOptions options;
    options.title = L"DomDownTool Tile Preview";
    options.logicalWidth = 1440;
    options.logicalHeight = 900;
    options.clearColor = RGB(31, 34, 33);
    options.documentPath = pathToUtf8(documentPath);
    options.runtime.assetRoot = pathToUtf8(documentPath.parent_path());
    options.onRuntimeReady = [&controller](skui::Runtime& runtime)
    { controller.attachRuntime(runtime); };
    options.onRuntimeResize = [&controller](skui::Runtime& runtime)
    { controller.resizeRuntime(runtime); };
    options.onAfterUiRender =
        [&controller](SkCanvas& canvas, int width, int height, float dpiScale)
    { controller.draw(canvas, width, height, dpiScale); };
    options.onWindowMessage = [&controller](HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                            skui::Runtime& runtime)
    { return controller.handleWindowMessage(window, message, wParam, lParam, runtime); };

    skui::win32::Dx12WindowApp application(std::move(options));
    application.setFrameRateLimit(120.);
    return application.run(instance, showCommand);
}
