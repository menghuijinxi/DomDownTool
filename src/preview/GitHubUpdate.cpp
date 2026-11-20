#include "preview/GitHubUpdate.h"

#include <array>
#include <filesystem>
#include <string>

#include "preview/PreviewResourceIds.h"

namespace
{
std::wstring embeddedUpdateCommand()
{
    HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(DOMDOWNTOOL_PREVIEW_UPDATE_RESOURCE_ID), RT_RCDATA);
    if (!resource)
    {
        return {};
    }
    const auto* bytes = static_cast<const char*>(LockResource(LoadResource(module, resource)));
    const int size = static_cast<int>(SizeofResource(module, resource));
    if (!bytes || size == 0)
    {
        return {};
    }
    std::wstring script(size, L'\0');
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size,
                                           script.data(), size);
    script.resize(length);
    return script;
}

BOOL CALLBACK focusUpdateWindow(HWND window, LPARAM processId)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(processId) && IsWindowVisible(window))
    {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return FALSE;
    }
    return TRUE;
}
}

DomDownToolPreview::GitHubUpdate::~GitHubUpdate()
{
    if (process_)
    {
        CloseHandle(process_);
    }
}

void DomDownToolPreview::GitHubUpdate::Open(HWND owner)
{
    if (process_)
    {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
        {
            EnumWindows(focusUpdateWindow, GetProcessId(process_));
            return;
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
    const auto script = embeddedUpdateCommand();
    std::array<wchar_t, MAX_PATH> systemDirectory{};
    const UINT length = GetSystemDirectoryW(systemDirectory.data(), MAX_PATH);
    if (script.empty() || length == 0 || length >= systemDirectory.size())
    {
        MessageBoxW(owner, L"无法加载更新组件。", L"检查更新", MB_ICONERROR);
        return;
    }
    const auto shell = std::filesystem::path(systemDirectory.data()) /
        L"WindowsPowerShell/v1.0/powershell.exe";
    // 命令仅包含内置脚本和数字进程 ID。路径、标签等外部数据均在脚本内按值处理。
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -STA -Command \"& {\n";
    command += script;
    command += L"\n} " + std::to_wstring(GetCurrentProcessId()) + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, systemDirectory.data(), &startup, &child))
    {
        MessageBoxW(owner, L"无法启动 Windows PowerShell 更新组件。", L"检查更新", MB_ICONERROR);
        return;
    }
    CloseHandle(child.hThread);
    process_ = child.hProcess;
    AllowSetForegroundWindow(child.dwProcessId);
}
