#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

#include "skui_win32_app.h"

#include "preview/PreviewController.h"
#include "preview/GuiStandardStreams.h"
#include "preview/PreviewResourceIds.h"
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

std::optional<std::string> loadEmbeddedPreviewDocument(HINSTANCE instance)
{
    const HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(DOMDOWNTOOL_PREVIEW_HTML_RESOURCE_ID), RT_RCDATA);
    if (resource == nullptr)
    {
        return std::nullopt;
    }

    const DWORD resourceSize = SizeofResource(instance, resource);
    const HGLOBAL resourceHandle = LoadResource(instance, resource);
    if (resourceSize == 0 || resourceHandle == nullptr)
    {
        return std::nullopt;
    }

    const void* resourceBytes = LockResource(resourceHandle);
    if (resourceBytes == nullptr)
    {
        return std::nullopt;
    }

    return std::string(static_cast<const char*>(resourceBytes), resourceSize);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    if (!DomDownToolPreview::initializeGuiStandardStreams())
    {
        MessageBoxW(nullptr, L"Failed to initialize application output streams.",
                    L"DomDownTool", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }

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

    const std::optional<std::string> embeddedDocument =
        loadEmbeddedPreviewDocument(instance);
    if (!embeddedDocument.has_value())
    {
        DomDownToolPreview::writeSearchTrace("App", "embedded_preview_document_missing");
        MessageBoxW(nullptr, L"Failed to load the embedded preview UI.", L"DomDownTool",
                    MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }

    DomDownToolPreview::PreviewController controller;
    skui::win32::WindowOptions options;
    options.title = L"DomDownTool Tile Preview";
    options.logicalWidth = 1440;
    options.logicalHeight = 900;
    options.clearColor = RGB(31, 34, 33);
    options.onRuntimeReady = [&controller, &embeddedDocument](skui::Runtime& runtime)
    {
        if (!runtime.loadDocumentFromString(*embeddedDocument))
        {
            DomDownToolPreview::writeSearchTrace(
                "App", "embedded_preview_document_load_failed", runtime.lastError());
            throw std::runtime_error("Failed to parse the embedded preview UI.");
        }
        controller.attachRuntime(runtime);
    };
    options.onRuntimeResize = [&controller](skui::Runtime& runtime)
    { controller.resizeRuntime(runtime); };
    options.onAfterUiRender =
        [&controller](SkCanvas& canvas, int width, int height, float dpiScale)
    { controller.draw(canvas, width, height, dpiScale); };
    options.onWindowMessage = [&controller](HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                            skui::Runtime& runtime)
    { return controller.handleWindowMessage(window, message, wParam, lParam, runtime); };

    try
    {
        skui::win32::Dx12WindowApp application(std::move(options));
        application.setFrameRateLimit(120.);
        return application.run(instance, showCommand);
    }
    catch (const std::exception& exception)
    {
        DomDownToolPreview::writeSearchTrace("App", "preview_startup_failed", exception.what());
        MessageBoxW(nullptr, L"Failed to start the embedded preview UI.", L"DomDownTool",
                    MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
}
