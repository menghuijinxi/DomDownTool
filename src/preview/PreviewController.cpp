#include "preview/PreviewController.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "fmt/format.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkRect.h"
#include "skui_runtime.h"
#include "skui_runtime_helpers.h"

#include "nlohmann/json.hpp"

#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "preview/PreviewInputRouting.h"
#include "preview/SearchTrace.h"

namespace
{
using DomDownToolPreview::ExportConfiguration;
using DomDownToolPreview::ExportTaskRecord;
using DomDownToolPreview::FbxPreviewInteractionSettings;
using DomDownToolPreview::GisExportMode;
using Json = nlohmann::json;

constexpr UINT kPreviewRedrawMessage = WM_APP + 0x642;
constexpr UINT kPreviewUiRefreshMessage = WM_APP + 0x643;
constexpr UINT kPreviewSearchRefreshMessage = WM_APP + 0x644;
constexpr UINT kPreviewMapUiRefreshMessage = WM_APP + 0x646;
constexpr UINT kPreviewExportTaskRefreshMessage = WM_APP + 0x647;
constexpr UINT kPreviewPageChangeMessage = WM_APP + 0x648;
constexpr UINT kPreviewImmediateMapRedrawMessage = WM_APP + 0x649;
constexpr UINT kPreviewImmediateFbxRedrawMessage = WM_APP + 0x64A;
constexpr UINT_PTR kPreviewEyedropperTimerId = 0x645;
constexpr UINT_PTR kPreviewFbxKeyboardPanTimerId = 0x64B;
constexpr UINT_PTR kPreviewFbxFrameTimerId = 0x64C;
constexpr UINT kPreviewEyedropperTimerIntervalMilliseconds = 16;
constexpr UINT kPreviewFbxKeyboardPanFrameIntervalMilliseconds = 4;
constexpr UINT kPreviewFbxFrameTimerIntervalMilliseconds = 16;
constexpr auto kTianDiTuMultidateYearPickerHold = std::chrono::seconds(20);
constexpr auto kFbxFrameRateUpdateInterval = std::chrono::milliseconds(500);
constexpr auto kFbxFrameRateIdleResetInterval = std::chrono::seconds(1);
constexpr double kSearchControlLeft = 192.;
constexpr double kSearchControlWidth = 300.;
constexpr double kSearchPopupWidth = 360.;
constexpr double kSearchPopupTop = 46.;
constexpr double kSearchPopupHeight = 374.;
constexpr double kGisOverlayPanelLeft = 735.;
constexpr double kGisOverlayPanelTop = 48.;
constexpr double kGisOverlayPanelWidth = 184.;
constexpr double kGisOverlayPanelHeight = 322.;
constexpr double kGisColorPanelLeft = 720.;
constexpr double kGisColorPanelTop = 48.;
constexpr double kGisColorPanelWidth = 342.;
constexpr double kGisColorPanelHeight = 562.;
constexpr double kGisColorFieldLeft = 730.;
constexpr double kGisColorFieldTop = 427.;
constexpr double kGisColorFieldWidth = 190.;
constexpr double kGisColorFieldHeight = 150.;
constexpr double kGisColorHueTop = kGisColorFieldTop;
constexpr double kGisColorHueHeight = kGisColorFieldHeight;
constexpr double kMinimumFbxInteractionSpeed = 0.1;
constexpr double kMaximumFbxInteractionSpeed = 3.0;
constexpr int kFbxPreviewHeaderHeight = 56;
constexpr int kFbxPreviewFooterHeight = 42;
constexpr int kFbxSpeedPanelRight = 18;
constexpr int kFbxSpeedPanelBottom = 60;
constexpr int kFbxSpeedPanelWidth = 260;
constexpr int kFbxSpeedPanelHeight = 174;
constexpr double kFbxKeyboardPanPixelsPerSecond = 720.0;
constexpr std::string_view kTaskOutputPathSeparator = "\xEF\xBC\x9B";

std::filesystem::path fbxInteractionSettingsPath()
{
    return std::filesystem::u8path("cache/fbx-preview-settings.json");
}

double clampFbxInteractionSpeed(double value)
{
    if (!std::isfinite(value))
    {
        return 1.0;
    }
    return std::clamp(value, kMinimumFbxInteractionSpeed, kMaximumFbxInteractionSpeed);
}

std::string fbxInteractionSpeedText(double value)
{
    return fmt::format("{:.1f}x", value);
}

std::string buildingFbxTextureModeValue(DownTool::AmapBuildingFbxTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxTextureMode::Scattered:
        return "scattered";
    case DownTool::AmapBuildingFbxTextureMode::MergedAtlas:
        return "merged-atlas";
    }
    return "scattered";
}

DownTool::AmapBuildingFbxTextureMode buildingFbxTextureModeFromValue(
    std::string_view value)
{
    if (value == "merged-atlas" || value == "合并贴图")
    {
        return DownTool::AmapBuildingFbxTextureMode::MergedAtlas;
    }
    return DownTool::AmapBuildingFbxTextureMode::Scattered;
}

std::string buildingFbxTextureModeHint(std::string_view value)
{
    if (value == "satellite")
    {
        return "屋顶卫星图模式会把 buildings.fbx 写到对应卫星 FBX 目录，并复用其中贴图。";
    }
    return value == "merged-atlas" ?
        "合并贴图模式会只把本次随机用到的贴图合成 atlas，减少材质和 draw call。" :
        "散图模式会额外生成 buildings.fbx，贴图通过 FBX 材质槽引用。";
}

std::string buildingFbxRoofTextureModeValue(
    DownTool::AmapBuildingFbxRoofTextureMode mode)
{
    switch (mode)
    {
    case DownTool::AmapBuildingFbxRoofTextureMode::Scattered:
        return "scattered";
    case DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas:
        return "merged-atlas";
    case DownTool::AmapBuildingFbxRoofTextureMode::Satellite:
        return "satellite";
    }
    return "scattered";
}

DownTool::AmapBuildingFbxRoofTextureMode buildingFbxRoofTextureModeFromValue(
    std::string_view value)
{
    if (value == "merged-atlas" || value == "合并贴图")
    {
        return DownTool::AmapBuildingFbxRoofTextureMode::MergedAtlas;
    }
    if (value == "satellite" || value == "卫星图")
    {
        return DownTool::AmapBuildingFbxRoofTextureMode::Satellite;
    }
    return DownTool::AmapBuildingFbxRoofTextureMode::Scattered;
}

std::string buildingFbxAtlasImageFormatValue(
    DownTool::AmapBuildingFbxAtlasImageFormat format)
{
    switch (format)
    {
    case DownTool::AmapBuildingFbxAtlasImageFormat::Png:
        return "png";
    case DownTool::AmapBuildingFbxAtlasImageFormat::Jpg:
        return "jpg";
    }
    return "png";
}

DownTool::AmapBuildingFbxAtlasImageFormat buildingFbxAtlasImageFormatFromValue(
    std::string_view value)
{
    if (value == "jpg" || value == "jpeg" || value == "JPG" || value == "JPEG")
    {
        return DownTool::AmapBuildingFbxAtlasImageFormat::Jpg;
    }
    return DownTool::AmapBuildingFbxAtlasImageFormat::Png;
}

std::string imageFormatValue(std::string_view value)
{
    if (value == "jpg" || value == "jpeg" || value == "JPG" || value == "JPEG")
    {
        return "jpg";
    }
    return "png";
}

FbxPreviewInteractionSettings loadFbxInteractionSettings()
{
    FbxPreviewInteractionSettings settings;
    std::ifstream input(fbxInteractionSettingsPath(), std::ios::binary);
    if (!input.is_open())
    {
        return settings;
    }
    const Json root = Json::parse(input, nullptr, false);
    if (!root.is_object())
    {
        return settings;
    }
    settings.rotateSpeed = clampFbxInteractionSpeed(root.value("rotateSpeed", settings.rotateSpeed));
    settings.zoomSpeed = clampFbxInteractionSpeed(root.value("zoomSpeed", settings.zoomSpeed));
    settings.panSpeed = clampFbxInteractionSpeed(root.value("panSpeed", settings.panSpeed));
    settings.moveSpeed = clampFbxInteractionSpeed(root.value("moveSpeed", settings.moveSpeed));
    return settings;
}

bool saveFbxInteractionSettings(const FbxPreviewInteractionSettings& settings)
{
    const std::filesystem::path path = fbxInteractionSettingsPath();
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError)
    {
        return false;
    }
    Json root = Json::object();
    root["rotateSpeed"] = settings.rotateSpeed;
    root["zoomSpeed"] = settings.zoomSpeed;
    root["panSpeed"] = settings.panSpeed;
    root["moveSpeed"] = settings.moveSpeed;

    std::filesystem::path temporaryPath = path;
    temporaryPath += ".tmp";
    {
        std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            return false;
        }
        output << root.dump(2);
        if (!output.good())
        {
            return false;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(temporaryPath, path, renameError);
    if (!renameError)
    {
        return true;
    }
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    renameError.clear();
    std::filesystem::rename(temporaryPath, path, renameError);
    return !renameError;
}

bool isPointInside(double x, double y, double left, double top, double width, double height)
{
    return x >= left && x <= left + width && y >= top && y <= top + height;
}

POINT clientPoint(LPARAM lParam)
{
    return POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                 static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
}

bool isFbxKeyboardPanKey(WPARAM key)
{
    return key == 'W' || key == 'A' || key == 'S' || key == 'D' ||
        key == 'Q' || key == 'E';
}

bool isKeyDownAfterMessage(UINT message, WPARAM messageKey, WPARAM targetKey)
{
    if (messageKey == targetKey)
    {
        return message == WM_KEYDOWN;
    }
    return (GetAsyncKeyState(static_cast<int>(targetKey)) & 0x8000) != 0;
}

class ScreenDeviceContext
{
  public:
    ScreenDeviceContext() : handle_(GetDC(nullptr))
    {
    }

    ~ScreenDeviceContext()
    {
        if (handle_)
        {
            ReleaseDC(nullptr, handle_);
        }
    }

    ScreenDeviceContext(const ScreenDeviceContext&) = delete;
    ScreenDeviceContext& operator=(const ScreenDeviceContext&) = delete;

    [[nodiscard]] HDC get() const
    {
        return handle_;
    }

  private:
    HDC handle_ = nullptr;
};

std::optional<DownTool::AmapVectorColor> screenPixelColor(const POINT& screenPoint)
{
    const ScreenDeviceContext screen;
    if (!screen.get())
    {
        return std::nullopt;
    }

    const COLORREF color = GetPixel(screen.get(), screenPoint.x, screenPoint.y);
    if (color == CLR_INVALID)
    {
        return std::nullopt;
    }
    return DownTool::AmapVectorColor{GetRValue(color), GetGValue(color), GetBValue(color), 255};
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

std::wstring utf8ToWide(std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0)
    {
        return L"无法显示错误详情。";
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), length);
    return result;
}

struct CoTaskMemDeleter
{
    void operator()(wchar_t* value) const
    {
        CoTaskMemFree(value);
    }
};

class ScopedComInitialization
{
  public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                 COINIT_DISABLE_OLE1DDE))
    {
    }

    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result_))
        {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool available() const
    {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT result_ = E_FAIL;
};

std::optional<std::filesystem::path> selectOutputDirectory(HWND owner)
{
    const ScopedComInitialization comInitialization;
    if (!comInitialization.available())
    {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(dialog.GetAddressOf()))))
    {
        return std::nullopt;
    }

    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->GetOptions(&options)) ||
        FAILED(dialog->SetOptions(
            options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST)) ||
        FAILED(dialog->Show(owner)))
    {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IShellItem> selectedItem;
    if (FAILED(dialog->GetResult(selectedItem.GetAddressOf())))
    {
        return std::nullopt;
    }

    wchar_t* rawPath = nullptr;
    if (FAILED(selectedItem->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)))
    {
        return std::nullopt;
    }
    const std::unique_ptr<wchar_t, CoTaskMemDeleter> selectedPath(rawPath);
    return std::filesystem::path(selectedPath.get());
}

bool equalStats(const DomDownToolPreview::MapRenderStats& left,
                const DomDownToolPreview::MapRenderStats& right)
{
    return left.visibleTiles == right.visibleTiles && left.readyTiles == right.readyTiles &&
        left.cache.residentTiles == right.cache.residentTiles &&
        left.cache.pendingTiles == right.cache.pendingTiles &&
        left.cache.cpuBytes == right.cache.cpuBytes &&
        left.cache.gpuBytes == right.cache.gpuBytes &&
        left.downloads.queuedTiles == right.downloads.queuedTiles &&
        left.downloads.activeTiles == right.downloads.activeTiles &&
        left.downloads.failedTiles == right.downloads.failedTiles;
}

std::string escapeHtml(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '&':
            escaped += "&amp;";
            break;
        case '<':
            escaped += "&lt;";
            break;
        case '>':
            escaped += "&gt;";
            break;
        case '"':
            escaped += "&quot;";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

std::string responseBodyPreview(std::string_view value)
{
    constexpr size_t kPreviewLimit = 180;
    std::string preview(value.substr(0, std::min(value.size(), kPreviewLimit)));
    for (char& character : preview)
    {
        if (character == '\r' || character == '\n' || character == '\t')
        {
            character = ' ';
        }
    }
    return preview;
}

std::optional<size_t> searchResultIndex(std::string_view id)
{
    constexpr std::string_view kPrefix = "search-result-";
    if (!id.starts_with(kPrefix))
    {
        return std::nullopt;
    }

    size_t index = 0;
    const std::string_view indexText = id.substr(kPrefix.size());
    const auto parseResult =
        std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
    if (parseResult.ec != std::errc{} || parseResult.ptr != indexText.data() + indexText.size())
    {
        return std::nullopt;
    }
    return index;
}

std::optional<uint64_t> exportGisGroupId(std::string_view id)
{
    constexpr std::string_view kPrefix = "export-gis-group-";
    if (!id.starts_with(kPrefix))
    {
        return std::nullopt;
    }

    const size_t separator = id.find_last_of('-');
    const std::string_view value = id.substr(separator + 1);
    uint64_t groupId = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), groupId);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
    {
        return std::nullopt;
    }
    return groupId;
}

std::optional<std::string> taskIdFromCancelButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-cancel-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::string> taskIdFromOpenButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-open-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::string> taskIdFromPreviewButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-preview-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::string> taskIdFromConfigButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-config-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::string> taskIdFromResumeButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-resume-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::string> taskIdFromRemoveButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-remove-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::string> taskIdFromDeleteButton(std::string_view id)
{
    constexpr std::string_view kPrefix = "task-delete-";
    if (!id.starts_with(kPrefix) || id.size() == kPrefix.size())
    {
        return std::nullopt;
    }
    return std::string(id.substr(kPrefix.size()));
}

std::optional<std::filesystem::path> existingTaskOutputPath(std::string_view outputPath)
{
    const size_t separator = outputPath.find(kTaskOutputPathSeparator);
    const std::filesystem::path path = std::filesystem::u8path(
        std::string(outputPath.substr(0, separator)));
    std::error_code pathError;
    if (!std::filesystem::is_directory(path, pathError) || pathError)
    {
        return std::nullopt;
    }
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, pathError);
    return pathError ? std::optional<std::filesystem::path>(path) : absolutePath;
}

std::vector<std::filesystem::path> findTaskFbxFiles(const ExportTaskRecord& record)
{
    std::vector<std::filesystem::path> files;
    size_t start = 0;
    while (start <= record.outputPath.size())
    {
        const size_t separator = record.outputPath.find(kTaskOutputPathSeparator, start);
        const std::string_view pathText = std::string_view(record.outputPath).substr(
            start, separator == std::string_view::npos ? std::string_view::npos :
                                                          separator - start);
        if (!pathText.empty())
        {
            const std::filesystem::path root = std::filesystem::u8path(std::string(pathText));
            std::error_code iteratorError;
            if (std::filesystem::is_directory(root, iteratorError) && !iteratorError)
            {
                std::filesystem::recursive_directory_iterator iterator(
                    root, std::filesystem::directory_options::skip_permission_denied,
                    iteratorError);
                const std::filesystem::recursive_directory_iterator end;
                for (; iterator != end && files.size() < 32; iterator.increment(iteratorError))
                {
                    if (iteratorError)
                    {
                        iteratorError.clear();
                        continue;
                    }
                    if (!iterator->is_regular_file(iteratorError) || iteratorError)
                    {
                        iteratorError.clear();
                        continue;
                    }
                    std::string extension = iterator->path().extension().string();
                    std::ranges::transform(extension, extension.begin(),
                                           [](unsigned char character)
                                           { return static_cast<char>(std::tolower(character)); });
                    if (extension == ".fbx")
                    {
                        files.push_back(iterator->path());
                    }
                }
            }
        }
        if (separator == std::string_view::npos)
        {
            break;
        }
        start = separator + kTaskOutputPathSeparator.size();
    }
    std::ranges::sort(files);
    return files;
}

bool taskDeclaresFbxOutput(const ExportTaskRecord& record)
{
    if (record.configuration.has_value() && record.configuration->exportFbx)
    {
        return true;
    }

    // 旧任务记录可能没有保存配置，但详情文本仍能识别 FBX 导出，保留预览入口。
    return record.detail.find("FBX") != std::string::npos;
}

bool openTaskOutputDirectory(HWND owner, const std::filesystem::path& path)
{
    const HINSTANCE result = ShellExecuteW(
        owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::optional<double> parseFiniteDouble(std::string_view value)
{
    try
    {
        size_t parsedLength = 0;
        const double result = std::stod(std::string(value), &parsedLength);
        if (parsedLength != value.size() || !std::isfinite(result))
        {
            return std::nullopt;
        }
        return result;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

std::optional<DownTool::LonLat> parseCenterCoordinate(std::string_view value)
{
    const size_t separator = value.find(',');
    if (separator == std::string_view::npos ||
        value.find(',', separator + 1) != std::string_view::npos)
    {
        return std::nullopt;
    }

    const std::optional<double> longitude = parseFiniteDouble(value.substr(0, separator));
    const std::optional<double> latitude = parseFiniteDouble(value.substr(separator + 1));
    if (!longitude.has_value() || *longitude < -180. || *longitude > 180. ||
        !latitude.has_value() || *latitude < -85. || *latitude > 85.)
    {
        return std::nullopt;
    }
    return DownTool::LonLat{*longitude, *latitude};
}

std::optional<uint32_t> parseUint32(std::string_view value)
{
    uint32_t result = 0;
    const auto parseResult =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (parseResult.ec != std::errc{} ||
        parseResult.ptr != value.data() + value.size())
    {
        return std::nullopt;
    }
    return result;
}

std::string conversionRecordValue(const std::string& line, std::string_view prefix)
{
    return line.starts_with(prefix) ? line.substr(prefix.size()) : std::string{};
}

std::optional<ExportConfiguration> loadLegacyTaskConfiguration(
    const ExportTaskRecord& record)
{
    const size_t separator = record.outputPath.find(kTaskOutputPathSeparator);
    const std::filesystem::path outputRoot = std::filesystem::u8path(
        record.outputPath.substr(0, separator));
    const std::filesystem::path conversionRecord = outputRoot /
        std::filesystem::u8path("转换记录-" + record.id + ".txt");
    std::ifstream input(conversionRecord, std::ios::binary);
    if (!input.is_open())
    {
        return std::nullopt;
    }

    ExportConfiguration configuration;
    bool hasCenter = false;
    bool hasRadius = false;
    bool hasZoom = false;
    std::string line;
    while (std::getline(input, line))
    {
        const std::string centerValue = conversionRecordValue(
            line, "中心坐标 (WGS84): ");
        if (!centerValue.empty())
        {
            const std::optional<DownTool::LonLat> center = parseCenterCoordinate(centerValue);
            if (center.has_value())
            {
                configuration.centerWgs84 = *center;
                hasCenter = true;
            }
            continue;
        }
        const std::string radiusValue = conversionRecordValue(line, "半径 (米): ");
        if (!radiusValue.empty())
        {
            const std::optional<double> radius = parseFiniteDouble(radiusValue);
            if (radius.has_value())
            {
                configuration.radiusMeters = *radius;
                hasRadius = true;
            }
            continue;
        }
        const std::string zoomValue = conversionRecordValue(line, "层级: ");
        if (!zoomValue.empty())
        {
            const std::optional<uint32_t> zoom = parseUint32(zoomValue);
            if (zoom.has_value())
            {
                configuration.zoom = *zoom;
                hasZoom = true;
            }
            continue;
        }
        const std::string outputValue = conversionRecordValue(
            line, "用户指定输出根目录: ");
        if (!outputValue.empty())
        {
            if (!outputValue.starts_with("未指定"))
            {
                configuration.outputRoot = std::filesystem::u8path(outputValue);
            }
            continue;
        }
        const std::string sourceValue = conversionRecordValue(line, "卫星源: ");
        if (!sourceValue.empty())
        {
            const size_t openBracket = sourceValue.rfind(" [");
            const size_t closeBracket = sourceValue.rfind(']');
            if (openBracket != std::string::npos && closeBracket > openBracket)
            {
                configuration.satelliteSources.push_back({
                    sourceValue.substr(openBracket + 2, closeBracket - openBracket - 2),
                    sourceValue.substr(0, openBracket), {}});
            }
            continue;
        }
        const std::string cacheValue = conversionRecordValue(line, "  缓存目录: ");
        if (!cacheValue.empty() && !configuration.satelliteSources.empty())
        {
            configuration.satelliteSources.back().cachePath =
                std::filesystem::u8path(cacheValue);
            continue;
        }
        const std::string categoryValue = conversionRecordValue(line, "GIS 分类: ");
        if (!categoryValue.empty())
        {
            static constexpr std::array<std::pair<std::string_view,
                                                  DownTool::AmapVectorRenderCategory>, 10>
                categories = {{{"全部", DownTool::AmapVectorRenderCategory::All},
                               {"道路", DownTool::AmapVectorRenderCategory::Road},
                               {"水", DownTool::AmapVectorRenderCategory::Water},
                               {"绿地", DownTool::AmapVectorRenderCategory::Green},
                               {"高速", DownTool::AmapVectorRenderCategory::Highway},
                               {"地铁", DownTool::AmapVectorRenderCategory::Subway},
                               {"建筑", DownTool::AmapVectorRenderCategory::Building},
                               {"点", DownTool::AmapVectorRenderCategory::Point},
                               {"边界", DownTool::AmapVectorRenderCategory::Boundary},
                               {"其他", DownTool::AmapVectorRenderCategory::Other}}};
            size_t start = 0;
            while (start <= categoryValue.size())
            {
                const size_t end = categoryValue.find("、", start);
                const std::string_view category = std::string_view(categoryValue).substr(
                    start, end == std::string::npos ? std::string::npos : end - start);
                const auto match = std::ranges::find_if(
                    categories, [category](const auto& item) { return item.first == category; });
                if (match != categories.end())
                {
                    configuration.gisCategories.insert(match->second);
                }
                if (end == std::string::npos)
                {
                    break;
                }
                start = end + std::string_view("、").size();
            }
            continue;
        }
        const std::string modeValue = conversionRecordValue(line, "GIS 组合方式: ");
        if (!modeValue.empty())
        {
            if (modeValue.find("叠加卫星图") != std::string::npos)
            {
                configuration.gisModes.insert(GisExportMode::OverlayOnSatellite);
            }
            if (modeValue.find("GIS 合并") != std::string::npos)
            {
                configuration.gisModes.insert(GisExportMode::MergeSelectedLayers);
            }
            if (modeValue.find("GIS 单层") != std::string::npos)
            {
                configuration.gisModes.insert(GisExportMode::SeparateLayers);
            }
            continue;
        }
        const std::string exportValue = conversionRecordValue(line, "导出类型: ");
        if (!exportValue.empty())
        {
            configuration.exportTiles = exportValue.find("XYZ 切片") != std::string::npos;
            configuration.exportMergedImage = exportValue.find("合并大图") != std::string::npos;
            configuration.exportFbx = exportValue.find("FBX") != std::string::npos;
            configuration.exportGeoJson = exportValue.find("GeoJSON") != std::string::npos;
            continue;
        }
        const std::string mergedValue = conversionRecordValue(line, "合并大图: ");
        if (!mergedValue.empty())
        {
            const size_t separator = mergedValue.find("，");
            configuration.mergedImageFormat = separator == std::string::npos ? mergedValue :
                mergedValue.substr(0, separator);
            if (separator != std::string::npos)
            {
                constexpr std::string_view resolutionLabel = "最大边";
                const size_t resolutionLabelPosition = mergedValue.find(
                    resolutionLabel, separator + std::string("，").size());
                if (resolutionLabelPosition != std::string::npos)
                {
                    size_t resolutionPosition = resolutionLabelPosition + resolutionLabel.size();
                    while (resolutionPosition < mergedValue.size() &&
                           mergedValue[resolutionPosition] == ' ')
                    {
                        ++resolutionPosition;
                    }
                    const std::optional<uint32_t> resolution = parseUint32(
                        std::string_view(mergedValue).substr(resolutionPosition));
                    if (resolution.has_value())
                    {
                        configuration.mergedImageResolution = *resolution;
                    }
                }
            }
            continue;
        }
        const std::string fbxValue = conversionRecordValue(line, "FBX 贴图尺寸: ");
        if (!fbxValue.empty())
        {
            if (const std::optional<uint32_t> size = parseUint32(fbxValue))
            {
                configuration.fbxTextureSize = *size;
            }
            continue;
        }
        const std::string fbxTextureFormatValue = conversionRecordValue(
            line, "FBX 贴图格式: ");
        if (!fbxTextureFormatValue.empty())
        {
            configuration.fbxTextureFormat = imageFormatValue(fbxTextureFormatValue);
            continue;
        }
        const std::string terrainHeightValue = conversionRecordValue(
            line, "FBX 导出高度: ");
        if (!terrainHeightValue.empty())
        {
            configuration.exportTerrainHeight = terrainHeightValue == "是";
            continue;
        }
        const std::string terrainSampleRateValue = conversionRecordValue(
            line, "FBX 高度采样率 (米): ");
        if (!terrainSampleRateValue.empty())
        {
            if (const std::optional<double> sampleRate =
                    parseFiniteDouble(terrainSampleRateValue))
            {
                configuration.fbxTerrainSampleRateMeters = *sampleRate;
            }
            continue;
        }
        const std::string buildingValue = conversionRecordValue(line, "建筑按高度着色: ");
        if (!buildingValue.empty())
        {
            configuration.colorBuildingsByHeight = buildingValue == "是";
            continue;
        }
        const std::string buildingFbxValue = conversionRecordValue(line, "建筑散图 FBX: ");
        if (!buildingFbxValue.empty())
        {
            configuration.exportBuildingFbx = buildingFbxValue == "是";
            continue;
        }
        const std::string genericBuildingFbxValue = conversionRecordValue(line, "建筑 FBX: ");
        if (!genericBuildingFbxValue.empty())
        {
            configuration.exportBuildingFbx = genericBuildingFbxValue == "是";
            continue;
        }
        const std::string buildingFbxTextureModeValue = conversionRecordValue(
            line, "建筑 FBX 贴图模式: ");
        if (!buildingFbxTextureModeValue.empty())
        {
            configuration.buildingFbxWallTextureMode = buildingFbxTextureModeFromValue(
                buildingFbxTextureModeValue);
            configuration.buildingFbxRoofTextureMode = buildingFbxRoofTextureModeFromValue(
                buildingFbxTextureModeValue);
            continue;
        }
        const std::string buildingFbxWallTextureModeValue = conversionRecordValue(
            line, "建筑 FBX 墙体贴图模式: ");
        if (!buildingFbxWallTextureModeValue.empty())
        {
            configuration.buildingFbxWallTextureMode = buildingFbxTextureModeFromValue(
                buildingFbxWallTextureModeValue);
            continue;
        }
        const std::string buildingFbxRoofTextureModeValue = conversionRecordValue(
            line, "建筑 FBX 屋顶贴图模式: ");
        if (!buildingFbxRoofTextureModeValue.empty())
        {
            configuration.buildingFbxRoofTextureMode = buildingFbxRoofTextureModeFromValue(
                buildingFbxRoofTextureModeValue);
            continue;
        }
        const std::string buildingFbxAtlasFormatValue = conversionRecordValue(
            line, "建筑 FBX 合并贴图格式: ");
        if (!buildingFbxAtlasFormatValue.empty())
        {
            configuration.buildingFbxAtlasImageFormat = buildingFbxAtlasImageFormatFromValue(
                buildingFbxAtlasFormatValue);
            continue;
        }
        const std::string wallTexturePath = conversionRecordValue(
            line, "建筑墙体贴图目录: ");
        if (!wallTexturePath.empty())
        {
            configuration.buildingWallTexturePath = std::filesystem::u8path(wallTexturePath);
            continue;
        }
        const std::string roofTexturePath = conversionRecordValue(
            line, "建筑屋顶贴图目录: ");
        if (!roofTexturePath.empty())
        {
            configuration.buildingRoofTexturePath = std::filesystem::u8path(roofTexturePath);
            continue;
        }
        const std::string layerHeightValue = conversionRecordValue(
            line, "建筑楼层高度 (厘米): ");
        if (!layerHeightValue.empty())
        {
            if (const std::optional<double> layerHeight =
                    parseFiniteDouble(layerHeightValue))
            {
                configuration.buildingLayerHeightCentimeters = *layerHeight;
            }
            continue;
        }
        const std::string materialCountValue = conversionRecordValue(
            line, "建筑墙体材质上限: ");
        if (!materialCountValue.empty())
        {
            if (const std::optional<uint32_t> materialCount =
                    parseUint32(materialCountValue))
            {
                configuration.buildingMaxWallMaterialsPerBucket = *materialCount;
            }
            continue;
        }
        const std::string randomSeedValue = conversionRecordValue(
            line, "建筑贴图随机种子: ");
        if (!randomSeedValue.empty())
        {
            if (const std::optional<uint32_t> randomSeed = parseUint32(randomSeedValue))
            {
                configuration.buildingTextureRandomSeed = *randomSeed;
            }
        }
    }

    if (!hasCenter || !hasRadius || !hasZoom ||
        (configuration.satelliteSources.empty() && configuration.gisCategories.empty()))
    {
        return std::nullopt;
    }
    return configuration;
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisCategoryFromValue(
    std::string_view value)
{
    static const std::pair<std::string_view, DomDownToolPreview::GisOverlayCategory> categories[] = {
        {"none", DomDownToolPreview::GisOverlayCategory::None},
        {"all", DomDownToolPreview::GisOverlayCategory::All},
        {"road", DomDownToolPreview::GisOverlayCategory::Road},
        {"water", DomDownToolPreview::GisOverlayCategory::Water},
        {"green", DomDownToolPreview::GisOverlayCategory::Green},
        {"highway", DomDownToolPreview::GisOverlayCategory::Highway},
        {"subway", DomDownToolPreview::GisOverlayCategory::Subway},
        {"building", DomDownToolPreview::GisOverlayCategory::Building},
        {"point", DomDownToolPreview::GisOverlayCategory::Point},
        {"boundary", DomDownToolPreview::GisOverlayCategory::Boundary},
        {"line", DomDownToolPreview::GisOverlayCategory::Boundary},
        {"other", DomDownToolPreview::GisOverlayCategory::Other}};
    for (const auto& [name, category] : categories)
    {
        if (name == value)
        {
            return category;
        }
    }
    return std::nullopt;
}

std::string gisCategoryValue(DomDownToolPreview::GisOverlayCategory category)
{
    switch (category)
    {
    case DomDownToolPreview::GisOverlayCategory::None:
        return "none";
    case DomDownToolPreview::GisOverlayCategory::All:
        return "all";
    case DomDownToolPreview::GisOverlayCategory::Road:
        return "road";
    case DomDownToolPreview::GisOverlayCategory::Water:
        return "water";
    case DomDownToolPreview::GisOverlayCategory::Green:
        return "green";
    case DomDownToolPreview::GisOverlayCategory::Highway:
        return "highway";
    case DomDownToolPreview::GisOverlayCategory::Subway:
        return "subway";
    case DomDownToolPreview::GisOverlayCategory::Building:
        return "building";
    case DomDownToolPreview::GisOverlayCategory::Point:
        return "point";
    case DomDownToolPreview::GisOverlayCategory::Boundary:
        return "boundary";
    case DomDownToolPreview::GisOverlayCategory::Other:
        return "other";
    }
    return "none";
}

DownTool::AmapVectorRenderCategory exportCategory(
    DomDownToolPreview::GisOverlayCategory category)
{
    switch (category)
    {
    case DomDownToolPreview::GisOverlayCategory::Point:
        return DownTool::AmapVectorRenderCategory::Point;
    case DomDownToolPreview::GisOverlayCategory::Road:
        return DownTool::AmapVectorRenderCategory::Road;
    case DomDownToolPreview::GisOverlayCategory::Green:
        return DownTool::AmapVectorRenderCategory::Green;
    case DomDownToolPreview::GisOverlayCategory::Highway:
        return DownTool::AmapVectorRenderCategory::Highway;
    case DomDownToolPreview::GisOverlayCategory::Subway:
        return DownTool::AmapVectorRenderCategory::Subway;
    case DomDownToolPreview::GisOverlayCategory::Building:
        return DownTool::AmapVectorRenderCategory::Building;
    case DomDownToolPreview::GisOverlayCategory::Water:
        return DownTool::AmapVectorRenderCategory::Water;
    case DomDownToolPreview::GisOverlayCategory::Boundary:
        return DownTool::AmapVectorRenderCategory::Boundary;
    case DomDownToolPreview::GisOverlayCategory::None:
    case DomDownToolPreview::GisOverlayCategory::All:
    case DomDownToolPreview::GisOverlayCategory::Other:
        return DownTool::AmapVectorRenderCategory::Other;
    }
    return DownTool::AmapVectorRenderCategory::Other;
}

DomDownToolPreview::GisOverlayCategory previewCategory(
    DownTool::AmapVectorRenderCategory category)
{
    switch (category)
    {
    case DownTool::AmapVectorRenderCategory::Point:
        return DomDownToolPreview::GisOverlayCategory::Point;
    case DownTool::AmapVectorRenderCategory::Road:
        return DomDownToolPreview::GisOverlayCategory::Road;
    case DownTool::AmapVectorRenderCategory::Green:
        return DomDownToolPreview::GisOverlayCategory::Green;
    case DownTool::AmapVectorRenderCategory::Highway:
        return DomDownToolPreview::GisOverlayCategory::Highway;
    case DownTool::AmapVectorRenderCategory::Subway:
        return DomDownToolPreview::GisOverlayCategory::Subway;
    case DownTool::AmapVectorRenderCategory::Building:
        return DomDownToolPreview::GisOverlayCategory::Building;
    case DownTool::AmapVectorRenderCategory::Water:
        return DomDownToolPreview::GisOverlayCategory::Water;
    case DownTool::AmapVectorRenderCategory::Boundary:
        return DomDownToolPreview::GisOverlayCategory::Boundary;
    case DownTool::AmapVectorRenderCategory::All:
        return DomDownToolPreview::GisOverlayCategory::All;
    case DownTool::AmapVectorRenderCategory::Other:
        return DomDownToolPreview::GisOverlayCategory::Other;
    }
    return DomDownToolPreview::GisOverlayCategory::Other;
}

constexpr std::array kExportCategories = {
    DownTool::AmapVectorRenderCategory::Road,
    DownTool::AmapVectorRenderCategory::Building,
    DownTool::AmapVectorRenderCategory::Water,
    DownTool::AmapVectorRenderCategory::Point,
    DownTool::AmapVectorRenderCategory::Green,
    DownTool::AmapVectorRenderCategory::Boundary,
    DownTool::AmapVectorRenderCategory::Highway,
    DownTool::AmapVectorRenderCategory::Other,
    DownTool::AmapVectorRenderCategory::Subway};

constexpr std::array<std::pair<std::string_view, std::string_view>, 7>
    kExportProviderControls = {
    std::pair{"gaode.satellite", "export-provider-gaode"},
    std::pair{"tencent.satellite", "export-provider-tencent"},
    std::pair{"tianditu.satellite", "export-provider-tianditu"},
    std::pair{"baidu.satellite", "export-provider-baidu"},
    std::pair{"google.satellite", "export-provider-google"},
    std::pair{"bing.satellite", "export-provider-bing"},
    std::pair{"arcgis.satellite", "export-provider-arcgis"}};

bool isExportProviderSource(std::string_view sourceId)
{
    return std::ranges::any_of(
        kExportProviderControls,
        [sourceId](const auto& item) { return item.first == sourceId; });
}

DomDownToolPreview::GisOverlayCategories allGisCategories()
{
    return {DomDownToolPreview::GisOverlayCategory::Road,
            DomDownToolPreview::GisOverlayCategory::Water,
            DomDownToolPreview::GisOverlayCategory::Green,
            DomDownToolPreview::GisOverlayCategory::Highway,
            DomDownToolPreview::GisOverlayCategory::Subway,
            DomDownToolPreview::GisOverlayCategory::Building,
            DomDownToolPreview::GisOverlayCategory::Point,
            DomDownToolPreview::GisOverlayCategory::Boundary,
            DomDownToolPreview::GisOverlayCategory::Other};
}

struct GisCategoryControl
{
    std::string_view id;
    std::string_view value;
    std::string_view label;
    DomDownToolPreview::GisOverlayCategory category;
};

constexpr std::array kGisCategoryControls = {
    GisCategoryControl{"gis-overlay-road", "road", "道路",
                       DomDownToolPreview::GisOverlayCategory::Road},
    GisCategoryControl{"gis-overlay-water", "water", "水",
                       DomDownToolPreview::GisOverlayCategory::Water},
    GisCategoryControl{"gis-overlay-green", "green", "绿地",
                       DomDownToolPreview::GisOverlayCategory::Green},
    GisCategoryControl{"gis-overlay-highway", "highway", "高速",
                       DomDownToolPreview::GisOverlayCategory::Highway},
    GisCategoryControl{"gis-overlay-subway", "subway", "地铁",
                       DomDownToolPreview::GisOverlayCategory::Subway},
    GisCategoryControl{"gis-overlay-building", "building", "建筑",
                       DomDownToolPreview::GisOverlayCategory::Building},
    GisCategoryControl{"gis-overlay-point", "point", "点",
                       DomDownToolPreview::GisOverlayCategory::Point},
    GisCategoryControl{"gis-overlay-boundary", "boundary", "边界",
                       DomDownToolPreview::GisOverlayCategory::Boundary},
    GisCategoryControl{"gis-overlay-other", "other", "其他",
                       DomDownToolPreview::GisOverlayCategory::Other}};

std::vector<std::string> gisSelectionValues(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    if (categories.empty())
    {
        std::vector<std::string> values;
        values.emplace_back("none");
        return values;
    }
    if (categories == allGisCategories())
    {
        std::vector<std::string> values;
        values.emplace_back("all");
        return values;
    }

    std::vector<std::string> values;
    values.reserve(categories.size());
    for (const DomDownToolPreview::GisOverlayCategory category : categories)
    {
        values.push_back(gisCategoryValue(category));
    }
    return values;
}

std::string gisCategoriesValue(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    const std::vector<std::string> values = gisSelectionValues(categories);
    std::string result;
    for (const std::string& value : values)
    {
        if (!result.empty())
        {
            result += ',';
        }
        result += value;
    }
    return result;
}

std::string gisCategoriesSummary(
    const DomDownToolPreview::GisOverlayCategories& categories)
{
    if (categories.empty())
    {
        return "无";
    }
    if (categories == allGisCategories())
    {
        return "全部";
    }
    if (categories.size() > 2)
    {
        return fmt::format("已选 {} 项", categories.size());
    }

    std::string summary;
    for (const GisCategoryControl& control : kGisCategoryControls)
    {
        if (!categories.contains(control.category))
        {
            continue;
        }
        if (!summary.empty())
        {
            summary += "、";
        }
        summary += control.label;
    }
    return summary;
}

void setCheckboxChecked(skui::Runtime& runtime, std::string_view id, bool checked)
{
    if (checked)
    {
        runtime.setAttributeById(id, "checked", "");
        return;
    }
    runtime.removeAttributeById(id, "checked");
}

void setControlDisabled(skui::Runtime& runtime, std::string_view id, bool disabled)
{
    if (disabled)
    {
        runtime.setAttributeById(id, "disabled", "");
        return;
    }
    runtime.removeAttributeById(id, "disabled");
}

void syncGisCheckboxes(skui::Runtime& runtime,
                       const DomDownToolPreview::GisOverlayCategories& categories)
{
    setCheckboxChecked(runtime, "gis-overlay-none", categories.empty());
    setCheckboxChecked(runtime, "gis-overlay-all", categories == allGisCategories());
    for (const GisCategoryControl& control : kGisCategoryControls)
    {
        setCheckboxChecked(runtime, control.id, categories.contains(control.category));
    }
    runtime.setTextById("gis-overlay-summary", gisCategoriesSummary(categories));
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisColorCategoryFromId(
    std::string_view id)
{
    constexpr std::string_view prefix = "gis-color-";
    if (!id.starts_with(prefix))
    {
        return std::nullopt;
    }
    return gisCategoryFromValue(id.substr(prefix.size()));
}

std::optional<DomDownToolPreview::GisOverlayCategory> gisColorTargetCategoryFromId(
    std::string_view id)
{
    constexpr std::string_view prefix = "gis-color-target-";
    if (!id.starts_with(prefix))
    {
        return std::nullopt;
    }
    const std::optional<DomDownToolPreview::GisOverlayCategory> category =
        gisCategoryFromValue(id.substr(prefix.size()));
    if (!category.has_value() ||
        *category == DomDownToolPreview::GisOverlayCategory::None ||
        *category == DomDownToolPreview::GisOverlayCategory::All)
    {
        return std::nullopt;
    }
    return category;
}

std::optional<DownTool::AmapVectorColor> parseGisColor(std::string_view value)
{
    if (!value.empty() && value.front() == '#')
    {
        value.remove_prefix(1);
    }
    if (value.size() != 6)
    {
        return std::nullopt;
    }
    const auto parseByte = [](std::string_view text) -> std::optional<uint8_t>
    {
        uint32_t parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed > 255)
        {
            return std::nullopt;
        }
        return static_cast<uint8_t>(parsed);
    };
    const std::optional<uint8_t> red = parseByte(value.substr(0, 2));
    const std::optional<uint8_t> green = parseByte(value.substr(2, 2));
    const std::optional<uint8_t> blue = parseByte(value.substr(4, 2));
    if (!red || !green || !blue)
    {
        return std::nullopt;
    }
    return DownTool::AmapVectorColor{*red, *green, *blue, 255};
}

std::string gisColorHex(const DownTool::AmapVectorColor& color)
{
    return fmt::format("#{:02X}{:02X}{:02X}", static_cast<unsigned>(color.red),
                       static_cast<unsigned>(color.green),
                       static_cast<unsigned>(color.blue));
}

struct HsvColor
{
    double hue = 0.;
    double saturation = 0.;
    double brightness = 0.;
};

HsvColor toHsv(const DownTool::AmapVectorColor& color)
{
    const double red = static_cast<double>(color.red) / 255.;
    const double green = static_cast<double>(color.green) / 255.;
    const double blue = static_cast<double>(color.blue) / 255.;
    const double maximum = std::max({red, green, blue});
    const double minimum = std::min({red, green, blue});
    const double delta = maximum - minimum;

    double hue = 0.;
    if (delta > 0.)
    {
        if (maximum == red)
        {
            hue = 60. * std::fmod((green - blue) / delta, 6.);
        }
        else if (maximum == green)
        {
            hue = 60. * ((blue - red) / delta + 2.);
        }
        else
        {
            hue = 60. * ((red - green) / delta + 4.);
        }
        if (hue < 0.)
        {
            hue += 360.;
        }
    }

    return HsvColor{hue, maximum > 0. ? delta / maximum : 0., maximum};
}

DownTool::AmapVectorColor fromHsv(const HsvColor& color)
{
    const double hue = std::fmod(std::max(0., color.hue), 360.);
    const double saturation = std::clamp(color.saturation, 0., 1.);
    const double brightness = std::clamp(color.brightness, 0., 1.);
    const double chroma = brightness * saturation;
    const double intermediate =
        chroma * (1. - std::abs(std::fmod(hue / 60., 2.) - 1.));

    double red = 0.;
    double green = 0.;
    double blue = 0.;
    if (hue < 60.)
    {
        red = chroma;
        green = intermediate;
    }
    else if (hue < 120.)
    {
        red = intermediate;
        green = chroma;
    }
    else if (hue < 180.)
    {
        green = chroma;
        blue = intermediate;
    }
    else if (hue < 240.)
    {
        green = intermediate;
        blue = chroma;
    }
    else if (hue < 300.)
    {
        red = intermediate;
        blue = chroma;
    }
    else
    {
        red = chroma;
        blue = intermediate;
    }

    const double match = brightness - chroma;
    const auto channel = [match](double value)
    {
        return static_cast<uint8_t>(
            std::clamp(std::lround((value + match) * 255.), 0L, 255L));
    };
    return DownTool::AmapVectorColor{channel(red), channel(green), channel(blue), 255};
}

} // namespace

DomDownToolPreview::PreviewController::PreviewController()
    : tianDiTuMultidateSession_(),
      renderer_([this] { requestRedraw(); }),
      placeSearch_([this] { scheduleSearchRefresh(); },
                   [this](const std::string& url) { return webViewSearch_.get(url); },
                   [this] { webViewSearch_.cancelCurrent(); }),
      exportTaskManager_([this] { scheduleExportTaskRefresh(); }),
      dx12FbxRenderer_([this] { requestRedraw(); })
{
    renderer_.SetTianDiTuMultidateTileDownloader(
        [this](const DownTool::TileHttpRequest& request,
               const std::filesystem::path& partialPath)
        {
            return DownloadTianDiTuMultidateTile(request, partialPath);
        });
    applyFbxInteractionSettings(loadFbxInteractionSettings());
}

void DomDownToolPreview::PreviewController::attachRuntime(skui::Runtime& runtime)
{
    writeSearchTrace("Controller", "runtime_attached");
    runtime_ = &runtime;
    selectGisColorCategory(gisColorPaletteCategory_);
    runtime.setElementEventCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.id == "export-radius" &&
                event.type == skui::ElementEventType::Input)
            {
                previewRadiusValue_ = event.value;
                const std::optional<double> radius = parseFiniteDouble(event.value);
                if (radius.has_value() && *radius >= 0.)
                {
                    renderer_.SetExportRadiusMeters(*radius);
                    requestRedraw();
                }
                return;
            }

            if ((event.type == skui::ElementEventType::Input ||
                 event.type == skui::ElementEventType::Change) &&
                updateFbxInteractionSetting(runtime, event.id, event.value,
                                            event.type == skui::ElementEventType::Change))
            {
                return;
            }

            if (event.id == "tianditu-multidate-year" &&
                (event.type == skui::ElementEventType::MouseDown ||
                 event.type == skui::ElementEventType::Scroll ||
                 event.type == skui::ElementEventType::MouseWheel))
            {
                tianDiTuMultidateYearPickerActive_ = true;
                tianDiTuMultidateYearPickerReleaseAt_ =
                    std::chrono::steady_clock::now() + kTianDiTuMultidateYearPickerHold;
            }
            else if (tianDiTuMultidateYearPickerActive_ &&
                     event.id != "tianditu-multidate-year" &&
                     (event.type == skui::ElementEventType::MouseDown ||
                      event.type == skui::ElementEventType::Change ||
                      event.type == skui::ElementEventType::KeyDown))
            {
                tianDiTuMultidateYearPickerActive_ = false;
            }

            if (event.type == skui::ElementEventType::Input)
            {
                if (event.id == "center-coordinate")
                {
                    return;
                }
                if (event.id == "export-lon")
                {
                    exportLongitudeValue_ = event.value;
                    return;
                }
                if (event.id == "export-lat")
                {
                    exportLatitudeValue_ = event.value;
                    return;
                }
                if (event.id == "export-config-radius")
                {
                    exportRadiusValue_ = event.value;
                    return;
                }
                if (event.id == "export-zoom")
                {
                    exportZoomValue_ = event.value;
                    return;
                }
                if (event.id == "export-output")
                {
                    exportOutputValue_ = event.value;
                    return;
                }
                if (event.id == "export-building-wall-textures")
                {
                    exportBuildingWallTexturePathValue_ = event.value;
                    return;
                }
                if (event.id == "export-building-roof-textures")
                {
                    exportBuildingRoofTexturePathValue_ = event.value;
                    return;
                }
                if (event.id == "export-building-layer-height")
                {
                    exportBuildingLayerHeightValue_ = event.value;
                    return;
                }
                if (event.id == "export-building-max-wall-materials")
                {
                    exportBuildingMaxWallMaterialsValue_ = event.value;
                    return;
                }
                if (event.id == "export-building-texture-seed")
                {
                    exportBuildingTextureSeedValue_ = event.value;
                    return;
                }
                if (event.id == "export-merged-resolution")
                {
                    exportMergedResolutionValue_ = event.value;
                    return;
                }
            }

            if (event.type == skui::ElementEventType::Change)
            {
                if (event.id == "tianditu-multidate-year")
                {
                    tianDiTuMultidateYearPickerActive_ = false;
                    SelectTianDiTuMultidateTimeline(runtime, event.value);
                    return;
                }
                const auto providerControl = std::find_if(
                    kExportProviderControls.begin(), kExportProviderControls.end(),
                    [&event](const auto& item) { return item.second == event.id; });
                if (providerControl != kExportProviderControls.end())
                {
                    const std::string sourceId(providerControl->first);
                    if (event.checked)
                    {
                        exportMapSourceIds_.insert(sourceId);
                    }
                    else
                    {
                        exportMapSourceIds_.erase(sourceId);
                    }
                    if (exportMapSourceIds_.empty())
                    {
                        exportGisOverlayOnSatellite_ = false;
                    }
                    UpdateExportUi(runtime);
                    requestRedraw();
                    return;
                }
                if (event.id == "export-merged-format")
                {
                    exportMergedFormat_ = event.value;
                    return;
                }
                if (event.id == "export-fbx-size")
                {
                    exportFbxSizeValue_ = event.value;
                    return;
                }
                if (event.id == "export-fbx-texture-format")
                {
                    exportFbxTextureFormatValue_ = imageFormatValue(event.value);
                    return;
                }
                if (event.id == "export-terrain-sample-rate")
                {
                    exportTerrainSampleRateValue_ = event.value;
                    return;
                }
                if (event.id == "export-building-wall-texture-mode")
                {
                    exportBuildingWallTextureModeValue_ = event.value == "merged-atlas" ?
                        "merged-atlas" : "scattered";
                    UpdateExportUi(runtime);
                    return;
                }
                if (event.id == "export-building-roof-texture-mode")
                {
                    exportBuildingRoofTextureModeValue_ =
                        event.value == "merged-atlas" ? "merged-atlas" :
                        event.value == "satellite" ? "satellite" : "scattered";
                    UpdateExportUi(runtime);
                    return;
                }
                if (event.id == "export-building-atlas-format")
                {
                    exportBuildingAtlasFormatValue_ = event.value == "jpg" ? "jpg" : "png";
                    return;
                }
                if (event.id == "export-satellite-tiles")
                {
                    exportTiles_ = event.checked;
                    UpdateExportUi(runtime);
                    return;
                }
                if (event.id == "export-satellite-merged")
                {
                    exportMergedImage_ = event.checked;
                    UpdateExportUi(runtime);
                    return;
                }
                if (event.id == "export-gis-combine-overlay")
                {
                    exportGisOverlayOnSatellite_ = event.checked;
                    return;
                }
                if (event.id == "export-gis-combine-merged")
                {
                    exportGisLayersMerged_ = event.checked;
                    return;
                }
                if (event.id == "export-gis-combine-separate")
                {
                    exportGisLayersSeparate_ = event.checked;
                    return;
                }
                if (event.id == "export-fbx")
                {
                    exportFbx_ = event.checked;
                    if (!exportFbx_)
                    {
                        exportTerrainHeight_ = false;
                    }
                    UpdateExportUi(runtime);
                    requestRedraw();
                    return;
                }
                if (event.id == "export-terrain-height")
                {
                    exportTerrainHeight_ = exportFbx_ && event.checked;
                    UpdateExportUi(runtime);
                    return;
                }
                if (event.id == "export-geojson")
                {
                    exportGeoJson_ = event.checked;
                    UpdateExportUi(runtime);
                    return;
                }
                if (event.id == "export-building-height-colors")
                {
                    exportBuildingHeightColors_ = event.checked;
                    return;
                }
                if (event.id == "export-building-fbx")
                {
                    exportBuildingFbx_ = event.checked;
                    UpdateExportUi(runtime);
                    return;
                }
                if (const std::optional<uint64_t> groupId = exportGisGroupId(event.id))
                {
                    setExportGisGroupSelected(*groupId, event.checked);
                    UpdateExportUi(runtime);
                    requestRedraw();
                    return;
                }
            }

            if (event.type == skui::ElementEventType::MouseDown && gisOverlayPopupOpen_ &&
                event.id != "gis-overlay-toggle" &&
                !isPointInside(event.x, event.y, kGisOverlayPanelLeft, kGisOverlayPanelTop,
                               kGisOverlayPanelWidth, kGisOverlayPanelHeight))
            {
                gisOverlayPopupOpen_ = false;
                updateUi(runtime);
                requestRedraw();
            }

            if (event.id == "place-search-input" &&
                event.type == skui::ElementEventType::Input)
            {
                searchQuery_ = event.value;
                pendingLocationRequest_.reset();
                writeSearchTrace("Controller", "input_changed", "query=" + searchQuery_);
                if (searchQuery_.find_first_not_of(" \t\r\n") == std::string::npos)
                {
                    placeSearch_.cancelPending();
                    searchResults_.clear();
                    highlightedSearchResult_.reset();
                    searchState_ = SearchState::Idle;
                    searchPopupOpen_ = false;
                    updateSearchUi(runtime);
                }
                else
                {
                    searchState_ = SearchState::Loading;
                    searchMessage_.clear();
                    searchPopupOpen_ = true;
                    const uint64_t requestId =
                        placeSearch_.submitSuggestions(searchQuery_, renderer_.center());
                    writeSearchTrace("Controller", "suggestions_requested",
                                     fmt::format("id={}", requestId));
                    updateSearchUi(runtime);
                }
                requestRedraw();
                return;
            }

            if (event.id == "place-search-input" &&
                event.type == skui::ElementEventType::MouseDown)
            {
                writeSearchTrace("Controller", "input_mouse_down",
                                 fmt::format("x={} y={}", event.x, event.y));
                if (!searchResults_.empty() || searchState_ == SearchState::Error)
                {
                    searchPopupOpen_ = true;
                    updateSearchUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.id == "provider-select" &&
                event.type == skui::ElementEventType::MouseDown)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_mouse_down id={} value={} x={} y={}",
                                             event.id, event.value, event.x, event.y));
                return;
            }

            if (event.id == "provider-select" && event.type == skui::ElementEventType::Change)
            {
                writeSearchTrace("Controller",
                                 fmt::format("select_change id={} value={}", event.id,
                                             event.value));
                if (renderer_.selectSource(event.value))
                {
                    updateUi(runtime);
                    requestRedraw();
                }
                return;
            }

            if (event.type == skui::ElementEventType::Change &&
                event.id.starts_with("gis-overlay-"))
            {
                const std::optional<GisOverlayCategory> category =
                    gisCategoryFromValue(event.value);
                if (!category.has_value())
                {
                    return;
                }

                GisOverlayCategories categories = renderer_.GetGisOverlayCategories();
                if (*category == GisOverlayCategory::None)
                {
                    if (event.checked)
                    {
                        categories.clear();
                    }
                }
                else if (*category == GisOverlayCategory::All)
                {
                    categories = event.checked ? allGisCategories() : GisOverlayCategories{};
                }
                else if (event.checked)
                {
                    categories.insert(*category);
                }
                else
                {
                    categories.erase(*category);
                }

                writeSearchTrace("Controller",
                                 fmt::format("checkbox_change id={} checked={} values={}",
                                             event.id, event.checked,
                                             gisCategoriesValue(categories)));
                renderer_.SetGisOverlayCategories(categories);
                syncGisCheckboxes(runtime, categories);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.type == skui::ElementEventType::MouseDown)
            {
                if (const std::optional<GisOverlayCategory> target =
                        gisColorTargetCategoryFromId(event.id))
                {
                    selectGisColorCategory(*target);
                    updateUi(runtime);
                    requestRedraw();
                    return;
                }
                if (const std::optional<GisOverlayCategory> category =
                        gisColorCategoryFromId(event.id))
                {
                    selectGisColorCategory(*category);
                    updateUi(runtime);
                    requestRedraw();
                    return;
                }
            }

            const bool colorFieldEvent = event.id == "gis-color-picker-field" &&
                (event.type == skui::ElementEventType::MouseDown ||
                 event.type == skui::ElementEventType::MouseUp ||
                 (event.type == skui::ElementEventType::MouseMove &&
                  gisColorFieldDragging_));
            if (colorFieldEvent)
            {
                gisColorFieldDragging_ =
                    event.type != skui::ElementEventType::MouseUp;
                gisColorHueDragging_ = false;
                gisColorSaturation_ = std::clamp(
                    (static_cast<double>(event.x) - kGisColorFieldLeft) /
                        kGisColorFieldWidth,
                    0., 1.);
                gisColorBrightness_ = 1. - std::clamp(
                    (static_cast<double>(event.y) - kGisColorFieldTop) /
                        kGisColorFieldHeight,
                    0., 1.);
                applyGisColorPicker(runtime);
                return;
            }

            const bool colorHueEvent = event.id == "gis-color-picker-hue" &&
                (event.type == skui::ElementEventType::MouseDown ||
                 event.type == skui::ElementEventType::MouseUp ||
                 (event.type == skui::ElementEventType::MouseMove &&
                  gisColorHueDragging_));
            if (colorHueEvent)
            {
                gisColorHueDragging_ = event.type != skui::ElementEventType::MouseUp;
                gisColorFieldDragging_ = false;
                gisColorHue_ = 360. * std::clamp(
                    (static_cast<double>(event.y) - kGisColorHueTop) /
                        kGisColorHueHeight,
                    0., 1.);
                if (gisColorHue_ >= 360.)
                {
                    gisColorHue_ = 0.;
                }
                applyGisColorPicker(runtime);
                return;
            }

            if (event.type == skui::ElementEventType::Input)
            {
                const std::optional<GisOverlayCategory> category = gisColorCategoryFromId(event.id);
                if (category.has_value())
                {
                    const std::optional<DownTool::AmapVectorColor> color =
                        parseGisColor(event.value);
                    if (color.has_value())
                    {
                        renderer_.SetGisOverlayColor(*category, *color);
                        selectGisColorCategory(*category);
                        updateUi(runtime);
                        requestRedraw();
                    }
                    return;
                }
            }

            if (event.type != skui::ElementEventType::Click)
            {
                return;
            }

            if (page_ == Page::FbxPreview && event.id == "fbx-reset-view")
            {
                dx12FbxRenderer_.resetView();
                requestRedraw();
                return;
            }

            if (page_ == Page::FbxPreview &&
                (event.id == "fbx-preview-mode-textured" ||
                 event.id == "fbx-preview-mode-wireframe" ||
                 event.id == "fbx-preview-mode-both"))
            {
                if (event.id == "fbx-preview-mode-textured")
                {
                    dx12FbxRenderer_.setMode(FbxPreviewMode::Textured);
                }
                else if (event.id == "fbx-preview-mode-wireframe")
                {
                    dx12FbxRenderer_.setMode(FbxPreviewMode::Wireframe);
                }
                else if (event.id == "fbx-preview-mode-both")
                {
                    dx12FbxRenderer_.setMode(FbxPreviewMode::TexturedWireframe);
                }
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "open-export-page" || event.id == "tasks-new-export")
            {
                schedulePageChange(Page::Export, runtime);
                return;
            }
            if (event.id == "open-task-page" || event.id == "export-open-tasks")
            {
                schedulePageChange(Page::Tasks, runtime);
                return;
            }
            if (event.id == "export-back-preview" || event.id == "export-cancel" ||
                event.id == "tasks-back-preview" || event.id == "fbx-preview-back")
            {
                schedulePageChange(event.id == "fbx-preview-back" ? Page::Tasks : Page::Preview,
                                   runtime);
                return;
            }
            if (event.id == "export-submit")
            {
                SubmitExport(runtime);
                return;
            }
            if (event.id == "export-browse-output")
            {
                const std::optional<std::filesystem::path> selectedPath =
                    selectOutputDirectory(window_.load(std::memory_order_relaxed));
                if (selectedPath.has_value())
                {
                    exportOutputValue_ = pathToUtf8(*selectedPath);
                    runtime.setValueById("export-output", exportOutputValue_);
                    requestRedraw();
                }
                return;
            }
            if (event.id == "tasks-clear-finished")
            {
                taskDeleteTargetId_.reset();
                taskClearConfirmOpen_ = true;
                updateUi(runtime);
                requestRedraw();
                return;
            }
            if (event.id == "tasks-cancel-clear" || event.id == "task-clear-confirm")
            {
                taskClearConfirmOpen_ = false;
                updateUi(runtime);
                requestRedraw();
                return;
            }
            if (event.id == "tasks-confirm-clear")
            {
                taskClearConfirmOpen_ = false;
                exportTaskManager_.ClearFinished();
                UpdateTaskUi(runtime);
                updateUi(runtime);
                requestRedraw();
                return;
            }
            if (event.id == "task-delete-cancel" ||
                event.id == "task-delete-confirm")
            {
                taskDeleteTargetId_.reset();
                taskDeleteTargetTitle_.clear();
                taskDeleteTargetPath_.clear();
                updateUi(runtime);
                requestRedraw();
                return;
            }
            if (event.id == "task-delete-accept")
            {
                if (taskDeleteTargetId_.has_value())
                {
                    std::string error;
                    if (!exportTaskManager_.DeleteFinishedOutput(
                            *taskDeleteTargetId_, error))
                    {
                        MessageBoxW(
                            window_.load(std::memory_order_relaxed),
                            utf8ToWide(error).c_str(), L"DomDownTool",
                            MB_OK | MB_ICONERROR);
                    }
                }
                taskDeleteTargetId_.reset();
                taskDeleteTargetTitle_.clear();
                taskDeleteTargetPath_.clear();
                UpdateTaskUi(runtime);
                updateUi(runtime);
                requestRedraw();
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromCancelButton(event.id))
            {
                if (!exportTaskManager_.Cancel(*taskId))
                {
                    MessageBoxW(
                        window_.load(std::memory_order_relaxed),
                        L"任务已经结束或不存在，无法取消。", L"DomDownTool",
                        MB_OK | MB_ICONINFORMATION);
                }
                UpdateTaskUi(runtime);
                requestRedraw();
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromOpenButton(event.id))
            {
                const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
                const auto record = std::find_if(
                    records.begin(), records.end(),
                    [&taskId](const ExportTaskRecord& item) { return item.id == *taskId; });
                const std::optional<std::filesystem::path> outputPath =
                    record == records.end() ? std::nullopt :
                    existingTaskOutputPath(record->outputPath);
                if (!outputPath.has_value() ||
                    !openTaskOutputDirectory(
                        window_.load(std::memory_order_relaxed), *outputPath))
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                L"导出目录不存在或无法打开。", L"DomDownTool",
                                MB_OK | MB_ICONWARNING);
                }
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromPreviewButton(event.id))
            {
                const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
                const auto record = std::find_if(
                    records.begin(), records.end(),
                    [&taskId](const ExportTaskRecord& item) { return item.id == *taskId; });
                if (record == records.end() ||
                    record->state != ExportTaskState::Succeeded)
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                L"只有已完成的任务可以预览 FBX 网格。", L"DomDownTool",
                                MB_OK | MB_ICONINFORMATION);
                    return;
                }
                const std::vector<std::filesystem::path> fbxFiles = findTaskFbxFiles(*record);
                if (fbxFiles.empty())
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                L"该任务没有找到可预览的 FBX 文件。", L"DomDownTool",
                                MB_OK | MB_ICONWARNING);
                    return;
                }
                std::string loadError;
                if (!dx12FbxRenderer_.load(fbxFiles, loadError))
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                utf8ToWide(loadError).c_str(), L"DomDownTool",
                                MB_OK | MB_ICONWARNING);
                    return;
                }
                OpenFbxPreviewPage(runtime, fbxFiles.front().parent_path(), record->title);
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromConfigButton(event.id))
            {
                const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
                const auto record = std::find_if(
                    records.begin(), records.end(),
                    [&taskId](const ExportTaskRecord& item) { return item.id == *taskId; });
                const std::optional<ExportConfiguration> configuration =
                    record == records.end() ? std::nullopt :
                    (record->configuration.has_value() ? record->configuration :
                     loadLegacyTaskConfiguration(*record));
                if (!configuration.has_value())
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                L"该任务没有找到可恢复的导出参数。", L"DomDownTool",
                                MB_OK | MB_ICONINFORMATION);
                    return;
                }
                pendingExportConfiguration_ = *configuration;
                schedulePageChange(Page::Export, runtime);
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromResumeButton(event.id))
            {
                const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
                const auto record = std::find_if(
                    records.begin(), records.end(),
                    [&taskId](const ExportTaskRecord& item) { return item.id == *taskId; });
                if (record == records.end() ||
                    (record->state != ExportTaskState::Failed &&
                     record->state != ExportTaskState::Canceled))
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                L"只有失败、已中断或已取消的任务可以继续。", L"DomDownTool",
                                MB_OK | MB_ICONINFORMATION);
                    return;
                }
                const std::optional<ExportConfiguration> configuration =
                    record->configuration.has_value() ? record->configuration :
                    loadLegacyTaskConfiguration(*record);
                if (!configuration.has_value())
                {
                    MessageBoxW(window_.load(std::memory_order_relaxed),
                                L"该任务没有找到可恢复的导出参数。", L"DomDownTool",
                                MB_OK | MB_ICONINFORMATION);
                    return;
                }
                (void)exportTaskManager_.Enqueue(*configuration);
                UpdateTaskUi(runtime);
                requestRedraw();
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromRemoveButton(event.id))
            {
                exportTaskManager_.RemoveFinished(*taskId);
                UpdateTaskUi(runtime);
                requestRedraw();
                return;
            }
            if (const std::optional<std::string> taskId =
                    taskIdFromDeleteButton(event.id))
            {
                const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
                const auto record = std::find_if(
                    records.begin(), records.end(),
                    [&taskId](const ExportTaskRecord& item) { return item.id == *taskId; });
                if (record != records.end())
                {
                    taskClearConfirmOpen_ = false;
                    taskDeleteTargetId_ = *taskId;
                    taskDeleteTargetTitle_ = record->title;
                    taskDeleteTargetPath_ = record->outputPath;
                    updateUi(runtime);
                    requestRedraw();
                }
                return;
            }
            if (const std::optional<GisOverlayCategory> target =
                    gisColorTargetCategoryFromId(event.id))
            {
                selectGisColorCategory(*target);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "gis-color-eyedropper")
            {
                setGisColorEyedropperActive(!gisColorEyedropperActive_);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "gis-color-reset")
            {
                setGisColorEyedropperActive(false);
                renderer_.ResetGisOverlayColors();
                selectGisColorCategory(gisColorPaletteCategory_);
                updateUi(runtime);
                requestRedraw();
                return;
            }

            if (event.id == "zoom-in")
            {
                renderer_.zoomAt(static_cast<double>(runtime.width()) * 0.5,
                                 static_cast<double>(runtime.height()) * 0.5, 1.);
            }
            else if (event.id == "zoom-out")
            {
                renderer_.zoomAt(static_cast<double>(runtime.width()) * 0.5,
                                 static_cast<double>(runtime.height()) * 0.5, -1.);
            }
            else if (event.id == "reload-tiles")
            {
                renderer_.clearCache();
            }
            else if (event.id == "tianditu-multidate-login")
            {
                LoginTianDiTuMultidate(runtime);
                return;
            }
            else if (event.id == "tianditu-multidate-refresh")
            {
                RefreshTianDiTuMultidateTimelines(runtime, GetSelectedTianDiTuMultidateYear());
                return;
            }
            else if (event.id == "gis-color-toggle")
            {
                gisColorPopupOpen_ = !gisColorPopupOpen_;
                gisOverlayPopupOpen_ = false;
                if (!gisColorPopupOpen_)
                {
                    setGisColorEyedropperActive(false);
                }
            }
            else if (event.id == "gis-overlay-toggle")
            {
                gisOverlayPopupOpen_ = !gisOverlayPopupOpen_;
                gisColorPopupOpen_ = false;
                setGisColorEyedropperActive(false);
                writeSearchTrace("Controller", "gis_popup_toggle",
                                 fmt::format("open={}", gisOverlayPopupOpen_));
            }
            else if (event.id == "view-span-decrease")
            {
                renderer_.adjustViewSpan(-0.5);
            }
            else if (event.id == "view-span-increase")
            {
                renderer_.adjustViewSpan(0.5);
            }
            else if (event.id == "place-search-submit")
            {
                submitPlaceSearch(runtime);
            }
            else if (event.id == "place-search-clear")
            {
                clearPlaceSearch(runtime);
            }
            else if (const std::optional<size_t> resultIndex = searchResultIndex(event.id))
            {
                selectPlaceSearchResult(runtime, *resultIndex);
            }
            else
            {
                return;
            }

            updateUi(runtime);
            requestRedraw();
        });
    runtime.setElementKeyDownCallback(
        [this, &runtime](const skui::ElementEvent& event)
        {
            if (event.key == VK_ESCAPE &&
                (taskClearConfirmOpen_ || taskDeleteTargetId_.has_value()))
            {
                taskClearConfirmOpen_ = false;
                taskDeleteTargetId_.reset();
                updateUi(runtime);
                requestRedraw();
                return true;
            }
            if (event.id == "center-coordinate")
            {
                if (event.key == VK_RETURN)
                {
                    submitCenterCoordinate(runtime, event.value);
                    return true;
                }
                if (event.key == VK_ESCAPE)
                {
                    runtime.setValueById("center-coordinate", lastCenterCoordinate_);
                    requestRedraw();
                    return true;
                }
                return false;
            }
            if (event.key == VK_ESCAPE &&
                (gisOverlayPopupOpen_ || gisColorPopupOpen_ ||
                 gisColorEyedropperActive_))
            {
                gisOverlayPopupOpen_ = false;
                gisColorPopupOpen_ = false;
                setGisColorEyedropperActive(false);
                updateUi(runtime);
                requestRedraw();
                return true;
            }
            if (event.id != "place-search-input")
            {
                return false;
            }
            if (event.key == VK_RETURN)
            {
                if (searchPopupOpen_ && highlightedSearchResult_.has_value() &&
                    *highlightedSearchResult_ < searchResults_.size())
                {
                    selectPlaceSearchResult(runtime, *highlightedSearchResult_);
                    return true;
                }
                submitPlaceSearch(runtime);
                return true;
            }
            if (event.key == VK_DOWN)
            {
                moveSearchHighlight(runtime, 1);
                return true;
            }
            if (event.key == VK_UP)
            {
                moveSearchHighlight(runtime, -1);
                return true;
            }
            if (event.key == VK_ESCAPE)
            {
                placeSearch_.cancelPending();
                pendingLocationRequest_.reset();
                searchPopupOpen_ = false;
                updateSearchUi(runtime);
                requestRedraw();
                return true;
            }
            return false;
        });
    updateUi(runtime);
}

void DomDownToolPreview::PreviewController::resizeRuntime(skui::Runtime& runtime)
{
    updateUi(runtime);
}

void DomDownToolPreview::PreviewController::draw(SkCanvas& canvas, int width, int height,
                                                 float dpiScale)
{
    if (page_ == Page::Preview)
    {
        renderer_.draw(canvas, width, height, dpiScale);
    }
    else if (page_ == Page::FbxPreview)
    {
        canvas.clear(SkColorSetRGB(22, 27, 26));
        static_cast<void>(dx12FbxRenderer_.draw(canvas, width, height, dpiScale));
    }
    else
    {
        canvas.clear(SkColorSetRGB(25, 29, 28));
    }

    if (runtime_)
    {
        if (page_ == Page::FbxPreview)
        {
            recordFbxPreviewFrame(*runtime_);
        }
        SkAutoCanvasRestore restore(&canvas, true);
        canvas.clipRect(SkRect::MakeLTRB(0.F, 0.F, static_cast<float>(width),
                                         static_cast<float>(height)),
                        SkClipOp::kIntersect, false);
        skui::RuntimeRenderRegion overlayRegion;
        overlayRegion.drawBackground = false;
        runtime_->renderInto(canvas, overlayRegion);
    }

    scheduleUiRefresh();
}

bool DomDownToolPreview::PreviewController::handleWindowMessage(HWND window, UINT message,
                                                                WPARAM wParam, LPARAM lParam,
                                                                skui::Runtime& runtime)
{
    const bool destroyingWindow = message == WM_DESTROY || message == WM_NCDESTROY;
    if (message == WM_DESTROY && gisColorEyedropperActive_)
    {
        KillTimer(window, kPreviewEyedropperTimerId);
        gisColorEyedropperActive_ = false;
    }
    if (destroyingWindow)
    {
        restoreFbxInputMethod();
    }
    if (message == WM_DESTROY)
    {
        stopFbxKeyboardPanFrame(window);
        stopFbxPreviewFrameLoop(window);
        fbxKeyboardRightDirection_ = 0;
        fbxKeyboardForwardDirection_ = 0;
        fbxKeyboardVerticalDirection_ = 0;
    }
    window_.store(destroyingWindow ? nullptr : window, std::memory_order_relaxed);
    if (destroyingWindow)
    {
        immediateMapRedrawPosted_ = false;
        immediateFbxRedrawPosted_ = false;
        mapUiRefreshPosted_ = false;
        mapDragRedrawThrottle_.reset();
        fbxDragRedrawThrottle_.reset();
        mapCenterUiRefreshThrottle_.reset();
    }
    if (message == kPreviewImmediateMapRedrawMessage)
    {
        immediateMapRedrawPosted_ = false;
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOCHILDREN | RDW_UPDATENOW);
        return false;
    }
    if (message == kPreviewImmediateFbxRedrawMessage)
    {
        immediateFbxRedrawPosted_ = false;
        if (page_ == Page::FbxPreview && hasFbxKeyboardPanDirection())
        {
            lastFbxKeyboardPanFrame_ = std::chrono::steady_clock::now();
            applyFbxKeyboardPanTick();
        }
        RedrawWindow(window, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_NOCHILDREN | RDW_UPDATENOW);
        if (page_ == Page::FbxPreview && hasFbxKeyboardPanDirection())
        {
            scheduleFbxKeyboardPanFrame(window);
        }
        return false;
    }
    if (message == kPreviewRedrawMessage)
    {
        if (page_ == Page::FbxPreview)
        {
            fbxRedrawPosted_.store(false, std::memory_order_relaxed);
            updateUi(runtime);
        }
        return true;
    }
    if (message == kPreviewUiRefreshMessage)
    {
        updateUi(runtime);
        return true;
    }
    if (message == kPreviewSearchRefreshMessage)
    {
        writeSearchTrace("Controller", "search_refresh_message_received");
        processPlaceSearchResponses(runtime);
        return true;
    }
    if (message == kPreviewExportTaskRefreshMessage)
    {
        exportTaskRefreshPosted_.store(false, std::memory_order_relaxed);
        UpdateTaskUi(runtime);
        requestRedraw();
        return true;
    }
    if (message == kPreviewPageChangeMessage)
    {
        switch (static_cast<Page>(wParam))
        {
        case Page::Preview:
            OpenPreviewPage(runtime);
            break;
        case Page::Export:
            OpenExportPage(runtime);
            break;
        case Page::Tasks:
            OpenTaskPage(runtime);
            break;
        case Page::FbxPreview:
            OpenTaskPage(runtime);
            break;
        }
        return true;
    }
    if (message == kPreviewMapUiRefreshMessage)
    {
        mapUiRefreshPosted_ = false;
        updateMapInteractionUi(runtime);
        return false;
    }
    if (message == WM_TIMER && wParam == kPreviewEyedropperTimerId)
    {
        if (gisColorEyedropperActive_)
        {
            sampleGisColorAtScreenCursor(runtime);
            const bool cancel = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            const bool confirm = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (cancel || confirm)
            {
                setGisColorEyedropperActive(false);
                updateUi(runtime);
                requestRedraw();
            }
        }
        return true;
    }
    if (message == WM_TIMER && wParam == kPreviewFbxKeyboardPanTimerId)
    {
        fbxKeyboardPanTimerActive_ = false;
        KillTimer(window, kPreviewFbxKeyboardPanTimerId);
        if (page_ == Page::FbxPreview && hasFbxKeyboardPanDirection())
        {
            scheduleImmediateFbxRedraw(window, false);
        }
        return true;
    }
    if (message == WM_TIMER && wParam == kPreviewFbxFrameTimerId)
    {
        if (page_ == Page::FbxPreview)
        {
            scheduleImmediateFbxRedraw(window, false);
        }
        else
        {
            stopFbxPreviewFrameLoop(window);
        }
        return true;
    }
    if (page_ == Page::FbxPreview && !destroyingWindow)
    {
        disableFbxInputMethod(window);
    }

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE &&
        (taskClearConfirmOpen_ || taskDeleteTargetId_.has_value()))
    {
        taskClearConfirmOpen_ = false;
        taskDeleteTargetId_.reset();
        updateUi(runtime);
        requestRedraw();
        return true;
    }

    if (page_ == Page::FbxPreview)
    {
        if (message == WM_IME_SETCONTEXT || message == WM_IME_STARTCOMPOSITION ||
            message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION ||
            message == WM_IME_NOTIFY || message == WM_IME_CHAR ||
            message == WM_IME_REQUEST)
        {
            return true;
        }
        if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
        {
            OpenTaskPage(runtime);
            return true;
        }
        if ((message == WM_KEYDOWN || message == WM_KEYUP) && isFbxKeyboardPanKey(wParam))
        {
            const bool nextWDown = isKeyDownAfterMessage(message, wParam, 'W');
            const bool nextADown = isKeyDownAfterMessage(message, wParam, 'A');
            const bool nextSDown = isKeyDownAfterMessage(message, wParam, 'S');
            const bool nextDDown = isKeyDownAfterMessage(message, wParam, 'D');
            const bool nextQDown = isKeyDownAfterMessage(message, wParam, 'Q');
            const bool nextEDown = isKeyDownAfterMessage(message, wParam, 'E');
            const int rightDirection = (nextDDown ? 1 : 0) - (nextADown ? 1 : 0);
            const int forwardDirection = (nextWDown ? 1 : 0) - (nextSDown ? 1 : 0);
            const int verticalDirection = (nextEDown ? 1 : 0) - (nextQDown ? 1 : 0);
            setFbxKeyboardPanDirection(window, rightDirection, forwardDirection,
                                       verticalDirection);
            return true;
        }
        if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS)
        {
            dragging_ = false;
            fbxDragMode_ = FbxDragMode::None;
            fbxDragRedrawThrottle_.reset();
            setFbxKeyboardPanDirection(window, 0, 0, 0);
            return false;
        }
        if ((message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP) &&
            dragging_)
        {
            dragging_ = false;
            fbxDragMode_ = FbxDragMode::None;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            fbxDragRedrawThrottle_.reset();
            scheduleImmediateFbxRedraw(window);
            return true;
        }
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN)
        {
            const POINT point = clientPoint(lParam);
            if (!isFbxPreviewPoint(window, point.x, point.y))
            {
                return false;
            }
            dragging_ = true;
            fbxDragMode_ = (message == WM_LBUTTONDOWN && (wParam & MK_SHIFT) == 0)
                ? FbxDragMode::Orbit : FbxDragMode::Pan;
            fbxDragRedrawThrottle_.reset();
            lastDragPoint_ = point;
            SetCapture(window);
            return true;
        }
        if (message == WM_MOUSEMOVE && dragging_)
        {
            const POINT point = clientPoint(lParam);
            const double deltaX = static_cast<double>(point.x - lastDragPoint_.x);
            const double deltaY = static_cast<double>(point.y - lastDragPoint_.y);
            if (fbxDragMode_ == FbxDragMode::Pan)
            {
                dx12FbxRenderer_.panBy(deltaX, deltaY);
            }
            else
            {
                dx12FbxRenderer_.rotateBy(deltaX, deltaY);
            }
            lastDragPoint_ = point;
            scheduleImmediateFbxRedraw(window);
            return true;
        }
        if (message == WM_MOUSEWHEEL)
        {
            POINT point{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                        static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
            if (!isFbxPreviewPoint(window, point.x, point.y))
            {
                return false;
            }
            dx12FbxRenderer_.zoomBy(
                static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
            scheduleImmediateFbxRedraw(window);
            return true;
        }
        return false;
    }

    if (page_ != Page::Preview)
    {
        dragging_ = false;
        return false;
    }

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE &&
        (gisOverlayPopupOpen_ || gisColorPopupOpen_ ||
         gisColorEyedropperActive_))
    {
        gisOverlayPopupOpen_ = false;
        gisColorPopupOpen_ = false;
        setGisColorEyedropperActive(false);
        updateUi(runtime);
        requestRedraw();
        return true;
    }

    if (gisColorEyedropperActive_)
    {
        if (message == WM_SETCURSOR)
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return true;
        }
        if (message == WM_MOUSEMOVE)
        {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            sampleGisColorAtScreenCursor(runtime);
            return true;
        }
        if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN)
        {
            return true;
        }
        if (message == WM_RBUTTONUP)
        {
            setGisColorEyedropperActive(false);
            updateUi(runtime);
            requestRedraw();
            return true;
        }
        if (message == WM_LBUTTONUP)
        {
            sampleGisColorAtScreenCursor(runtime);
            setGisColorEyedropperActive(false);
            updateUi(runtime);
            requestRedraw();
            return true;
        }
    }

    if (searchPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isSearchInteractionPoint(runtime, point.x, point.y))
        {
            return shouldConsumeSearchPopupPointerMessage(message, true);
        }
        if (message == WM_LBUTTONDOWN)
        {
            placeSearch_.cancelPending();
            pendingLocationRequest_.reset();
            searchPopupOpen_ = false;
            updateSearchUi(runtime);
        }
    }

    if (gisOverlayPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isGisOverlayInteractionPoint(runtime, point.x, point.y))
        {
            return true;
        }
        if (message == WM_LBUTTONDOWN)
        {
            gisOverlayPopupOpen_ = false;
            updateUi(runtime);
            requestRedraw();
        }
    }

    if (gisColorPopupOpen_ &&
        (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE ||
         message == WM_MOUSEWHEEL))
    {
        POINT point = clientPoint(lParam);
        if (message == WM_MOUSEWHEEL)
        {
            point = POINT{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                          static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
            ScreenToClient(window, &point);
        }
        if (isGisColorInteractionPoint(runtime, point.x, point.y))
        {
            return true;
        }
        if (message == WM_LBUTTONDOWN)
        {
            gisColorPopupOpen_ = false;
            updateUi(runtime);
            requestRedraw();
        }
    }

    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE ||
        message == WM_KILLFOCUS)
    {
        dragging_ = false;
        gisColorFieldDragging_ = false;
        gisColorHueDragging_ = false;
        mapDragRedrawThrottle_.reset();
        if (message == WM_CANCELMODE && gisColorEyedropperActive_)
        {
            setGisColorEyedropperActive(false);
            updateUi(runtime);
            requestRedraw();
        }
        if (message == WM_KILLFOCUS && !gisColorEyedropperActive_ &&
            (gisOverlayPopupOpen_ || gisColorPopupOpen_))
        {
            gisOverlayPopupOpen_ = false;
            gisColorPopupOpen_ = false;
            updateUi(runtime);
            requestRedraw();
        }
        return false;
    }

    if (message == WM_LBUTTONUP && dragging_)
    {
        dragging_ = false;
        if (GetCapture() == window)
        {
            ReleaseCapture();
        }
        mapDragRedrawThrottle_.reset();
        scheduleImmediateMapRedraw(window);
        mapCenterUiRefreshThrottle_.reset();
        scheduleMapUiRefresh(window);
        return true;
    }

    if (message == WM_MOUSEWHEEL)
    {
        POINT point{static_cast<LONG>(static_cast<int16_t>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<int16_t>(HIWORD(lParam)))};
        ScreenToClient(window, &point);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }

        const double wheelSteps = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        renderer_.zoomAt(point.x, point.y, wheelSteps * 0.25);
        updateUi(runtime);
        return true;
    }

    if (message == WM_LBUTTONDBLCLK)
    {
        const POINT point = clientPoint(lParam);
        if (!isMapPoint(window, point.x, point.y))
        {
            return false;
        }
        renderer_.zoomAt(point.x, point.y, 1.);
        updateUi(runtime);
        return true;
    }

    if (message == WM_LBUTTONDOWN)
    {
        const POINT point = clientPoint(lParam);
        if (!isMapPoint(window, point.x, point.y))
        {
            dragging_ = false;
            if (GetCapture() == window)
            {
                ReleaseCapture();
            }
            return false;
        }
        dragging_ = true;
        mapDragRedrawThrottle_.reset();
        mapCenterUiRefreshThrottle_.reset();
        lastDragPoint_ = point;
        SetCapture(window);
        return true;
    }

    if (message == WM_MOUSEMOVE)
    {
        const POINT point = clientPoint(lParam);
        if (dragging_)
        {
            const double deltaX = static_cast<double>(point.x - lastDragPoint_.x);
            const double deltaY = static_cast<double>(point.y - lastDragPoint_.y);
            lastDragPoint_ = point;
            if (renderer_.panBy(deltaX, deltaY))
            {
                scheduleImmediateMapRedraw(window);
            }
            if (mapCenterUiRefreshThrottle_.shouldRefresh(
                    MapHoverRefreshThrottle::Clock::now()))
            {
                scheduleMapUiRefresh(window);
            }
            return true;
        }
    }

    return false;
}

bool DomDownToolPreview::PreviewController::isMapPoint(HWND window, int x, int y) const
{
    if (page_ != Page::Preview)
    {
        return false;
    }
    RECT client{};
    GetClientRect(window, &client);
    return x >= 0 && x < client.right && static_cast<double>(y) >= renderer_.toolbarHeight() &&
           static_cast<double>(y) <
               static_cast<double>(client.bottom) - renderer_.statusBarHeight();
}

bool DomDownToolPreview::PreviewController::isFbxPreviewPoint(HWND window, int x, int y) const
{
    if (page_ != Page::FbxPreview)
    {
        return false;
    }
    RECT client{};
    GetClientRect(window, &client);
    const int panelLeft = client.right - kFbxSpeedPanelRight - kFbxSpeedPanelWidth;
    const int panelTop = client.bottom - kFbxSpeedPanelBottom - kFbxSpeedPanelHeight;
    const int panelRight = client.right - kFbxSpeedPanelRight;
    const int panelBottom = client.bottom - kFbxSpeedPanelBottom;
    const bool insideSettingsPanel = x >= panelLeft && x < panelRight &&
        y >= panelTop && y < panelBottom;
    return !insideSettingsPanel && x >= 0 && x < client.right &&
           y >= kFbxPreviewHeaderHeight && y < client.bottom - kFbxPreviewFooterHeight;
}

bool DomDownToolPreview::PreviewController::isSearchInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.effectiveScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    const bool insideControl = logicalX >= kSearchControlLeft &&
        logicalX <= kSearchControlLeft + kSearchControlWidth && logicalY >= 0. && logicalY <= 52.;
    const bool insidePopup = searchPopupOpen_ && logicalX >= kSearchControlLeft &&
        logicalX <= kSearchControlLeft + kSearchPopupWidth && logicalY >= kSearchPopupTop &&
        logicalY <= kSearchPopupTop + kSearchPopupHeight;
    return insideControl || insidePopup;
}

bool DomDownToolPreview::PreviewController::isGisOverlayInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.effectiveScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    return isPointInside(logicalX, logicalY, kGisOverlayPanelLeft, kGisOverlayPanelTop,
                         kGisOverlayPanelWidth, kGisOverlayPanelHeight);
}

bool DomDownToolPreview::PreviewController::isGisColorInteractionPoint(
    const skui::Runtime& runtime, int x, int y) const
{
    const double scale = std::max(0.1F, runtime.effectiveScale());
    const double logicalX = static_cast<double>(x) / scale;
    const double logicalY = static_cast<double>(y) / scale;
    return isPointInside(logicalX, logicalY, kGisColorPanelLeft, kGisColorPanelTop,
                         kGisColorPanelWidth, kGisColorPanelHeight);
}

void DomDownToolPreview::PreviewController::submitPlaceSearch(skui::Runtime& runtime)
{
    if (searchQuery_.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        clearPlaceSearch(runtime);
        return;
    }

    searchResults_.clear();
    highlightedSearchResult_.reset();
    searchMessage_.clear();
    pendingLocationRequest_.reset();
    searchState_ = SearchState::Loading;
    searchPopupOpen_ = true;
    const uint64_t requestId = placeSearch_.submitSearch(searchQuery_, renderer_.center());
    writeSearchTrace("Controller", "search_requested",
                     fmt::format("id={} query={}", requestId, searchQuery_));
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::clearPlaceSearch(skui::Runtime& runtime)
{
    writeSearchTrace("Controller", "search_cleared");
    placeSearch_.cancelPending();
    searchQuery_.clear();
    searchMessage_.clear();
    searchResults_.clear();
    highlightedSearchResult_.reset();
    pendingLocationRequest_.reset();
    searchState_ = SearchState::Idle;
    searchPopupOpen_ = false;
    renderer_.setLocationMarker(std::nullopt);
    runtime.setValueById("place-search-input", "");
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::selectPlaceSearchResult(skui::Runtime& runtime,
                                                                    size_t index)
{
    if (index >= searchResults_.size())
    {
        return;
    }

    const PlaceSearchResult& result = searchResults_[index];
    if (!result.coordinateWgs84.has_value())
    {
        const std::string resultId = result.id;
        const std::string resultName = result.name;
        const std::string resolutionQuery =
            result.name + (result.address.empty() ? "" : " " + result.address);
        searchQuery_ = result.name;
        runtime.setValueById("place-search-input", searchQuery_);
        searchResults_.clear();
        highlightedSearchResult_.reset();
        searchMessage_.clear();
        searchState_ = SearchState::Loading;
        searchPopupOpen_ = true;
        const uint64_t requestId =
            placeSearch_.submitSearch(resolutionQuery, renderer_.center());
        pendingLocationRequest_ = PendingLocationRequest{requestId, resultId, resultName};
        writeSearchTrace("Controller", "location_resolution_requested",
                         fmt::format("id={} query={} expected_id={} expected_name={}",
                                     requestId, resolutionQuery, resultId, resultName));
        updateSearchUi(runtime);
        requestRedraw();
        return;
    }

    pendingLocationRequest_.reset();
    const std::string preferredMultidateYear = GetSelectedTianDiTuMultidateYear();
    renderer_.setView(*result.coordinateWgs84, result.recommendedZoom);
    renderer_.setLocationMarker(result.coordinateWgs84);
    placeSearch_.cancelPending();
    searchQuery_ = result.name;
    runtime.setValueById("place-search-input", searchQuery_);
    searchPopupOpen_ = false;
    writeSearchTrace(
        "Controller", "location_applied",
        fmt::format("name={} lon={:.6f} lat={:.6f} zoom={:.2f}", result.name,
                    result.coordinateWgs84->lon, result.coordinateWgs84->lat,
                    result.recommendedZoom));
    updateSearchUi(runtime);
    RefreshTianDiTuMultidateAfterViewChange(runtime, preferredMultidateYear);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::moveSearchHighlight(skui::Runtime& runtime,
                                                                 int direction)
{
    if (searchResults_.empty())
    {
        return;
    }

    const size_t resultCount = searchResults_.size();
    size_t nextIndex = highlightedSearchResult_.value_or(direction > 0 ? resultCount - 1 : 0);
    if (direction > 0)
    {
        nextIndex = (nextIndex + 1) % resultCount;
    }
    else
    {
        nextIndex = nextIndex == 0 ? resultCount - 1 : nextIndex - 1;
    }

    highlightedSearchResult_ = nextIndex;
    searchPopupOpen_ = true;
    updateSearchUi(runtime);
    runtime.scrollIntoViewById("search-result-" + std::to_string(nextIndex));
    requestRedraw();
}

void DomDownToolPreview::PreviewController::processPlaceSearchResponses(skui::Runtime& runtime)
{
    const std::vector<PlaceSearchResponse> responses = placeSearch_.takeCompleted();
    if (responses.empty())
    {
        return;
    }

    const PlaceSearchResponse& response = responses.back();
    writeSearchTrace(
        "Controller", "response_processing",
        fmt::format("id={} kind={} results={} error={}", response.requestId,
                    response.kind == PlaceSearchRequestKind::Suggestions ? "suggestions"
                                                                         : "search",
                    response.results.size(), response.error));
    const bool applyResolvedLocation = pendingLocationRequest_.has_value() &&
        pendingLocationRequest_->requestId == response.requestId;
    const std::optional<PendingLocationRequest> locationRequest =
        applyResolvedLocation ? pendingLocationRequest_ : std::nullopt;
    if (applyResolvedLocation)
    {
        pendingLocationRequest_.reset();
    }
    searchResults_ = response.results;
    searchMessage_ = response.error;
    if (applyResolvedLocation && response.succeeded())
    {
        const std::optional<size_t> resultIndex = findExactPlaceResultIndex(
            searchResults_, locationRequest->resultId, locationRequest->resultName);
        if (resultIndex.has_value())
        {
            writeSearchTrace("Controller", "location_resolution_complete",
                             fmt::format("id={} result_index={}", response.requestId,
                                         *resultIndex));
            selectPlaceSearchResult(runtime, *resultIndex);
            return;
        }

        writeSearchTrace(
            "Controller", "location_resolution_rejected",
            fmt::format("id={} expected_id={} expected_name={} candidates={}",
                        response.requestId, locationRequest->resultId,
                        locationRequest->resultName, searchResults_.size()));
        searchResults_.clear();
        searchMessage_ = "无法确认所选地点的坐标，请选择更具体的候选项。";
        searchState_ = SearchState::Error;
        highlightedSearchResult_.reset();
        searchPopupOpen_ = true;
        updateSearchUi(runtime);
        requestRedraw();
        return;
    }
    if (!response.succeeded())
    {
        searchState_ = SearchState::Error;
        highlightedSearchResult_.reset();
    }
    else if (searchResults_.empty())
    {
        searchState_ = SearchState::Empty;
        highlightedSearchResult_.reset();
    }
    else
    {
        searchState_ = SearchState::Results;
        highlightedSearchResult_ = 0;
    }
    searchPopupOpen_ = true;
    updateSearchUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::updateSearchUi(skui::Runtime& runtime)
{
    runtime.setVisibleById("place-search-results", searchPopupOpen_);
    if (!searchPopupOpen_)
    {
        return;
    }

    std::string html;
    if (searchState_ == SearchState::Loading)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state\">正在搜索...</div>";
    }
    else if (searchState_ == SearchState::Empty)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state\">没有找到匹配地点</div>";
    }
    else if (searchState_ == SearchState::Error)
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-state search-error\">" +
            escapeHtml(searchMessage_) + "</div>";
    }
    else
    {
        html = "<div id=\"place-search-results-content\" "
            "class=\"search-result-list\">";
        for (size_t index = 0; index < searchResults_.size(); ++index)
        {
            const PlaceSearchResult& result = searchResults_[index];
            const std::string subtitle = result.district +
                (result.address.empty() ? "" : " · " + result.address);
            html += fmt::format(
                "<button id=\"search-result-{}\" class=\"search-result{}\" type=\"button\" "
                "data-action=\"select-place-result\">"
                "<span class=\"search-result-name\">{}</span>"
                "<span class=\"search-result-address\">{}</span></button>",
                index,
                highlightedSearchResult_.has_value() && *highlightedSearchResult_ == index
                    ? " selected"
                    : "",
                escapeHtml(result.name), escapeHtml(subtitle));
        }
        html += "</div>";
    }
    const bool replaced = runtime.replaceHtmlById("place-search-results-content", html);
    writeSearchTrace(
        "Controller", "search_ui_replaced",
        fmt::format("success={} state={} results={}", replaced,
                    static_cast<int>(searchState_), searchResults_.size()));
}

void DomDownToolPreview::PreviewController::LoginTianDiTuMultidate(skui::Runtime& runtime)
{
    tianDiTuMultidateStatus_ = "请在弹出的天地图窗口完成登录。";
    UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
    requestRedraw();

    const TianDiTuMultidateCookieResponse response =
        tianDiTuMultidateSession_.requestSessionCookie();
    if (!response.error.empty())
    {
        tianDiTuMultidateStatus_ = response.error;
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    tianDiTuMultidateCookie_ = response.cookie;
    renderer_.SetTianDiTuMultidateCookie(tianDiTuMultidateCookie_);
    tianDiTuMultidateStatus_ = "已同步本次运行的天地图登录态，可刷新历史时相。";
    RefreshTianDiTuMultidateTimelines(runtime);
}

void DomDownToolPreview::PreviewController::RefreshTianDiTuMultidateTimelines(
    skui::Runtime& runtime, std::string_view preferredYear)
{
    if (tianDiTuMultidateCookie_.empty())
    {
        tianDiTuMultidateStatus_ = "历史时相登录仅本次运行有效，请登录后刷新年份。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    const std::optional<ViewportGeoBounds> bounds = renderer_.visibleGeoBounds();
    if (!bounds.has_value())
    {
        tianDiTuMultidateStatus_ = "当前视窗无法换算经纬度范围。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    DownTool::TianDiTuMultidateDown downloader;
    downloader.SetTileSize(256);
    downloader.SetCenterLonLat(renderer_.center());
    downloader.SetSessionCookie(tianDiTuMultidateCookie_);
    const uint32_t queryZoom = std::clamp(
        static_cast<uint32_t>(std::lround(renderer_.zoom())), 3u, 18u);

    if (!downloader.PrepareTimelineQueryForBounds(
            {bounds->minLon, bounds->minLat}, {bounds->maxLon, bounds->maxLat}, queryZoom))
    {
        tianDiTuMultidateStatus_ = "当前视窗无法解析天地图行政区域。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    const std::string timelineUrl = downloader.GetTimelineListUrl();
    writeSearchTrace(
        "Controller", "tianditu_timeline_request",
        fmt::format("zoom={} gb={} region={} bounds={:.8f},{:.8f},{:.8f},{:.8f}",
                    queryZoom, downloader.GetRegionGbCode(), downloader.GetRegionName(),
                    bounds->minLon, bounds->minLat, bounds->maxLon, bounds->maxLat));
    TianDiTuMultidateFetchResponse response;
    {
        const std::lock_guard lock(tianDiTuMultidateTileFetchMutex_);
        response = tianDiTuMultidateSession_.fetchUrl(timelineUrl);
    }
    writeSearchTrace(
        "Controller", "tianditu_timeline_response",
        fmt::format("status={} body_bytes={} final_url={} error={}", response.statusCode,
                    response.body.size(), response.finalUrl, response.error));

    const auto resetToLatest = [this]
    {
        tianDiTuMultidateTimelines_.clear();
        tianDiTuMultidateRegionGbCode_.clear();
        tianDiTuMultidateRegionName_.clear();
        tianDiTuMultidateSelectedValue_ = "latest";
        renderer_.UseLatestTianDiTuMultidate();
    };

    const bool redirectedToLogin =
        response.finalUrl.find("oauth.tianditu.gov.cn/login") != std::string::npos ||
        response.body.find("oauth.tianditu.gov.cn/login") != std::string::npos;
    if (redirectedToLogin)
    {
        resetToLatest();
        tianDiTuMultidateCookie_.clear();
        tianDiTuMultidateStatus_ = "本次天地图登录态已失效，请重新登录后刷新年份。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    if (!response.error.empty())
    {
        resetToLatest();
        tianDiTuMultidateStatus_ = "刷新历史时相失败：" + response.error;
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    if (response.statusCode != 200)
    {
        resetToLatest();
        tianDiTuMultidateStatus_ = fmt::format(
            "时间线接口 HTTP {}，请重新登录后刷新年份。", response.statusCode);
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    std::vector<DownTool::TianDiTuMultidateDown::TimelineInfo> timelines;
    if (!downloader.TryParseTimelineRecords(response.body, timelines))
    {
        resetToLatest();
        tianDiTuMultidateStatus_ = response.body.empty()
            ? "时间线接口返回空内容，请重新登录后刷新年份。"
            : "时间线返回无法解析：" + responseBodyPreview(response.body);
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    if (timelines.empty())
    {
        resetToLatest();
        tianDiTuMultidateStatus_ = "当前视窗没有返回可用历史时相，已保持最新影像。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    std::ranges::sort(
        timelines,
        [](const auto& left, const auto& right) { return left.time > right.time; });
    tianDiTuMultidateTimelines_ = std::move(timelines);
    tianDiTuMultidateRegionGbCode_ = downloader.GetRegionGbCode();
    tianDiTuMultidateRegionName_ = downloader.GetRegionName();
    const auto selectedByPreferredYear = std::ranges::find_if(
        tianDiTuMultidateTimelines_,
        [preferredYear](const auto& item)
        {
            return !preferredYear.empty() &&
                (item.year == preferredYear || item.time == preferredYear);
        });
    if (selectedByPreferredYear != tianDiTuMultidateTimelines_.end())
    {
        tianDiTuMultidateSelectedValue_ = selectedByPreferredYear->key;
    }
    else if (!preferredYear.empty())
    {
        tianDiTuMultidateSelectedValue_ = "latest";
        renderer_.UseLatestTianDiTuMultidate();
    }

    if (tianDiTuMultidateSelectedValue_ != "latest")
    {
        const auto selected = std::ranges::find_if(
            tianDiTuMultidateTimelines_,
            [this](const auto& item) { return item.key == tianDiTuMultidateSelectedValue_; });
        if (selected == tianDiTuMultidateTimelines_.end())
        {
            tianDiTuMultidateSelectedValue_ = "latest";
            renderer_.UseLatestTianDiTuMultidate();
        }
        else
        {
            renderer_.ConfigureTianDiTuMultidateLayer(
                selected->year, selected->key, tianDiTuMultidateRegionGbCode_,
                tianDiTuMultidateRegionName_, tianDiTuMultidateCookie_);
        }
    }
    const std::string regionText = tianDiTuMultidateRegionName_.empty()
        ? ""
        : "，区域 " + tianDiTuMultidateRegionName_;
    if (!preferredYear.empty() && selectedByPreferredYear != tianDiTuMultidateTimelines_.end())
    {
        tianDiTuMultidateStatus_ = fmt::format(
            "已获取 {} 个历史时相{}，并切换到 {}。",
            tianDiTuMultidateTimelines_.size(), regionText, preferredYear);
    }
    else if (!preferredYear.empty())
    {
        tianDiTuMultidateStatus_ = fmt::format(
            "已获取 {} 个历史时相{}，当前区域没有 {}。",
            tianDiTuMultidateTimelines_.size(), regionText, preferredYear);
    }
    else
    {
        tianDiTuMultidateStatus_ = fmt::format(
            "已获取 {} 个历史时相{}。", tianDiTuMultidateTimelines_.size(), regionText);
    }
    UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
    requestRedraw();
}

bool DomDownToolPreview::PreviewController::DownloadTianDiTuMultidateTile(
    const DownTool::TileHttpRequest& request,
    const std::filesystem::path& partialPath)
{
    TianDiTuMultidateFetchResponse response;
    {
        const std::lock_guard lock(tianDiTuMultidateTileFetchMutex_);
        response = tianDiTuMultidateSession_.fetchBinaryUrl(request.url);
    }

    const bool redirectedToLogin =
        response.finalUrl.find("oauth.tianditu.gov.cn/login") != std::string::npos ||
        response.body.find("oauth.tianditu.gov.cn/login") != std::string::npos;
    const bool htmlResponse =
        response.contentType.find("text/html") != std::string::npos ||
        response.contentType.find("text/plain") != std::string::npos;
    if (!response.error.empty() || response.statusCode != 200 || redirectedToLogin ||
        htmlResponse)
    {
        writeSearchTrace(
            "Controller", "tianditu_tile_fetch_failed",
            fmt::format("status={} body_bytes={} content_type={} final_url={} error={}",
                        response.statusCode, response.body.size(), response.contentType,
                        response.finalUrl, response.error));
        return false;
    }

    std::ofstream file(partialPath, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return false;
    }

    file.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
    return file.good();
}

std::string DomDownToolPreview::PreviewController::GetSelectedTianDiTuMultidateYear() const
{
    if (tianDiTuMultidateSelectedValue_ == "latest")
    {
        return {};
    }

    const auto selected = std::ranges::find_if(
        tianDiTuMultidateTimelines_,
        [this](const auto& item) { return item.key == tianDiTuMultidateSelectedValue_; });
    if (selected == tianDiTuMultidateTimelines_.end())
    {
        return {};
    }
    return selected->year.empty() ? selected->time : selected->year;
}

void DomDownToolPreview::PreviewController::RefreshTianDiTuMultidateAfterViewChange(
    skui::Runtime& runtime, std::string_view preferredYear)
{
    if (renderer_.activeSource().id != "tianditu.multidate" || tianDiTuMultidateCookie_.empty())
    {
        return;
    }

    RefreshTianDiTuMultidateTimelines(runtime, preferredYear);
}

void DomDownToolPreview::PreviewController::SelectTianDiTuMultidateTimeline(
    skui::Runtime& runtime, std::string_view value)
{
    if (value == "latest")
    {
        tianDiTuMultidateSelectedValue_ = "latest";
        renderer_.UseLatestTianDiTuMultidate();
        tianDiTuMultidateStatus_ = "已切回最新影像。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    if (tianDiTuMultidateCookie_.empty())
    {
        tianDiTuMultidateStatus_ = "历史时相需要先登录天地图。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    const auto selected = std::ranges::find_if(
        tianDiTuMultidateTimelines_,
        [value](const auto& item) { return item.key == value; });
    if (selected == tianDiTuMultidateTimelines_.end())
    {
        tianDiTuMultidateStatus_ = "请选择刷新后返回的历史时相。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    tianDiTuMultidateSelectedValue_ = selected->key;
    if (!renderer_.ConfigureTianDiTuMultidateLayer(
            selected->year, selected->key, tianDiTuMultidateRegionGbCode_,
            tianDiTuMultidateRegionName_, tianDiTuMultidateCookie_))
    {
        tianDiTuMultidateStatus_ = "无法切换天地图多时相图层。";
        UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
        requestRedraw();
        return;
    }

    tianDiTuMultidateStatus_ = fmt::format(
        "已切换到 {}。", selected->time.empty() ? selected->year : selected->time);
    UpdateTianDiTuMultidateUi(runtime, renderer_.activeSource());
    requestRedraw();
}

void DomDownToolPreview::PreviewController::UpdateTianDiTuMultidateUi(
    skui::Runtime& runtime, const MapSourceSummary& source)
{
    const bool visible = source.id == "tianditu.multidate";
    if (!lastTianDiTuMultidatePanelVisible_.has_value() ||
        *lastTianDiTuMultidatePanelVisible_ != visible)
    {
        runtime.setVisibleById("tianditu-multidate-panel", visible);
        lastTianDiTuMultidatePanelVisible_ = visible;
    }
    if (!visible)
    {
        return;
    }

    if (tianDiTuMultidateYearPickerActive_ &&
        std::chrono::steady_clock::now() >= tianDiTuMultidateYearPickerReleaseAt_)
    {
        tianDiTuMultidateYearPickerActive_ = false;
    }

    if (!tianDiTuMultidateYearPickerActive_)
    {
        std::string selectHtml =
            "<select id=\"tianditu-multidate-year\" aria-label=\"天地图多时相年份\">"
            "<option value=\"latest\">最新影像</option>";
        for (const DownTool::TianDiTuMultidateDown::TimelineInfo& timeline :
             tianDiTuMultidateTimelines_)
        {
            const std::string title = timeline.time.empty() ? timeline.year : timeline.time;
            const std::string zoomText =
                timeline.minZoom == 0 && timeline.maxZoom == 0
                    ? ""
                    : fmt::format(" z{}-{}", timeline.minZoom, timeline.maxZoom);
            selectHtml += fmt::format(
                "<option value=\"{}\">{}{}</option>", escapeHtml(timeline.key),
                escapeHtml(title), escapeHtml(zoomText));
        }
        selectHtml += "</select>";
        if (selectHtml != lastTianDiTuMultidateSelectHtml_)
        {
            runtime.replaceHtmlById("tianditu-multidate-year", selectHtml);
            lastTianDiTuMultidateSelectHtml_ = selectHtml;
            lastTianDiTuMultidateSelectedValue_.clear();
            lastTianDiTuMultidateYearDisabled_.reset();
        }
        if (tianDiTuMultidateSelectedValue_ != lastTianDiTuMultidateSelectedValue_)
        {
            runtime.setValueById("tianditu-multidate-year", tianDiTuMultidateSelectedValue_);
            lastTianDiTuMultidateSelectedValue_ = tianDiTuMultidateSelectedValue_;
        }
    }
    if (tianDiTuMultidateStatus_ != lastTianDiTuMultidateStatus_)
    {
        runtime.setTextById("tianditu-multidate-status", tianDiTuMultidateStatus_);
        lastTianDiTuMultidateStatus_ = tianDiTuMultidateStatus_;
    }

    const bool refreshDisabled = tianDiTuMultidateCookie_.empty();
    if (!lastTianDiTuMultidateRefreshDisabled_.has_value() ||
        *lastTianDiTuMultidateRefreshDisabled_ != refreshDisabled)
    {
        setControlDisabled(runtime, "tianditu-multidate-refresh", refreshDisabled);
        lastTianDiTuMultidateRefreshDisabled_ = refreshDisabled;
    }
    const bool yearDisabled = tianDiTuMultidateTimelines_.empty();
    if (!tianDiTuMultidateYearPickerActive_ &&
        (!lastTianDiTuMultidateYearDisabled_.has_value() ||
         *lastTianDiTuMultidateYearDisabled_ != yearDisabled))
    {
        setControlDisabled(runtime, "tianditu-multidate-year", yearDisabled);
        lastTianDiTuMultidateYearDisabled_ = yearDisabled;
    }
}

void DomDownToolPreview::PreviewController::selectGisColorCategory(
    GisOverlayCategory category)
{
    if (category == GisOverlayCategory::None || category == GisOverlayCategory::All)
    {
        return;
    }
    gisColorPaletteCategory_ = category;
    const HsvColor hsv = toHsv(renderer_.GetGisOverlayColor(category));
    gisColorHue_ = hsv.hue;
    gisColorSaturation_ = hsv.saturation;
    gisColorBrightness_ = hsv.brightness;
}

void DomDownToolPreview::PreviewController::applyGisColorPicker(skui::Runtime& runtime)
{
    const DownTool::AmapVectorColor color =
        fromHsv(HsvColor{gisColorHue_, gisColorSaturation_, gisColorBrightness_});
    renderer_.SetGisOverlayColor(gisColorPaletteCategory_, color);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::setGisColorEyedropperActive(bool active)
{
    gisColorEyedropperActive_ = active;
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window)
    {
        return;
    }
    if (active)
    {
        if (SetTimer(window, kPreviewEyedropperTimerId,
                     kPreviewEyedropperTimerIntervalMilliseconds, nullptr) == 0)
        {
            gisColorEyedropperActive_ = false;
        }
        return;
    }
    KillTimer(window, kPreviewEyedropperTimerId);
}

bool DomDownToolPreview::PreviewController::sampleGisColorAtScreenCursor(skui::Runtime& runtime)
{
    POINT screenPoint{};
    if (!GetCursorPos(&screenPoint))
    {
        return false;
    }
    const std::optional<DownTool::AmapVectorColor> color = screenPixelColor(screenPoint);
    if (!color.has_value())
    {
        return false;
    }

    const DownTool::AmapVectorColor currentColor =
        renderer_.GetGisOverlayColor(gisColorPaletteCategory_);
    if (currentColor.red == color->red && currentColor.green == color->green &&
        currentColor.blue == color->blue && currentColor.alpha == color->alpha)
    {
        return false;
    }

    renderer_.SetGisOverlayColor(gisColorPaletteCategory_, *color);
    selectGisColorCategory(gisColorPaletteCategory_);
    updateUi(runtime);
    requestRedraw();
    return true;
}

std::string DomDownToolPreview::PreviewController::buildFbxPreviewStatsText() const
{
    const FbxPreviewLoadStatus loadStatus = dx12FbxRenderer_.loadStatus();
    if (!loadStatus.error.empty())
    {
        return "加载失败：" + loadStatus.error;
    }
    if (!loadStatus.meshReady)
    {
        return "正在读取网格...";
    }

    const FbxPreviewRenderStats renderStats = dx12FbxRenderer_.renderStats();
    std::string statsText = fmt::format(
        "{} 个顶点 · 当前面数 {} · 总面数 {} · 当前 DrawCall {} · 总 DrawCall {}",
        dx12FbxRenderer_.vertexCount(),
        renderStats.currentTriangleCount,
        renderStats.totalTriangleCount,
        renderStats.currentDrawCallCount,
        renderStats.totalDrawCallCount);
    if (loadStatus.totalTextureCount > 0)
    {
        statsText += fmt::format(" · 贴图 {}/{}{}",
                                 loadStatus.readyTextureCount,
                                 loadStatus.totalTextureCount,
                                 loadStatus.loading ? "（后台处理中）" : "");
    }
    return statsText;
}

void DomDownToolPreview::PreviewController::updateFbxPreviewFrameStatsUi(
    skui::Runtime& runtime)
{
    runtime.setTextById("fbx-preview-stats", buildFbxPreviewStatsText());
}

void DomDownToolPreview::PreviewController::updateUi(skui::Runtime& runtime)
{
    const skui::RuntimeUpdateBatch updateBatch(runtime);
    runtime.setVisibleById("preview-page", page_ == Page::Preview);
    runtime.setVisibleById("export-page", page_ == Page::Export);
    runtime.setVisibleById("task-page", page_ == Page::Tasks);
    runtime.setVisibleById("fbx-page", page_ == Page::FbxPreview);
    runtime.setVisibleById("task-clear-confirm",
                           page_ == Page::Tasks && taskClearConfirmOpen_);
    runtime.setVisibleById("task-delete-confirm",
                           page_ == Page::Tasks && taskDeleteTargetId_.has_value());
    runtime.setTextsById({
        {"task-delete-title-value", taskDeleteTargetTitle_},
        {"task-delete-path-value", taskDeleteTargetPath_}});
    if (page_ == Page::Export)
    {
        UpdateExportUi(runtime);
        return;
    }
    if (page_ == Page::Tasks)
    {
        UpdateTaskUi(runtime);
        return;
    }
    if (page_ == Page::FbxPreview)
    {
        runtime.setTextsById({
            {"fbx-preview-title", fbxPreviewTitle_},
            {"fbx-preview-directory", pathToUtf8(fbxPreviewTaskRoot_)},
            {"fbx-preview-stats", buildFbxPreviewStatsText()},
            {"fbx-preview-fps", fbxFrameRateText_}});
        const std::array<std::pair<const char*, bool>, 3> modeButtons = {{
            {"fbx-preview-mode-textured", dx12FbxRenderer_.mode() == FbxPreviewMode::Textured},
            {"fbx-preview-mode-wireframe", dx12FbxRenderer_.mode() == FbxPreviewMode::Wireframe},
            {"fbx-preview-mode-both",
             dx12FbxRenderer_.mode() == FbxPreviewMode::TexturedWireframe}}};
        for (const auto& [id, active] : modeButtons)
        {
            runtime.setAttributeById(id, "aria-pressed", active ? "true" : "false");
        }
        syncFbxInteractionSettingsUi(runtime);
        return;
    }

    const MapSourceSummary source = renderer_.activeSource();
    const DownTool::LonLat center = renderer_.center();
    const MapRenderStats stats = renderer_.stats();
    const double memoryMegabytes =
        static_cast<double>(stats.cache.cpuBytes + stats.cache.gpuBytes) / (1024. * 1024.);

    runtime.setValueById("provider-select", source.id);
    runtime.setValueById("export-radius", previewRadiusValue_);
    runtime.setTextsById(
        {{"active-provider", source.name},
         {"coordinate-system", source.coordinateSystem},
         {"zoom-value", fmt::format("Z {:.2f}", renderer_.zoom())},
         {"view-span-value", fmt::format("视野 {:.1f}x", renderer_.GetViewSpan())},
         {"cache-path", pathToUtf8(source.cacheRoot)}});
    UpdateTianDiTuMultidateUi(runtime, source);
    const std::string tileStatsState =
        fmt::format("{}:{}:{}:{}:{}:{}:{:.1f}", stats.visibleTiles, stats.readyTiles,
                    stats.cache.pendingTiles, stats.downloads.activeTiles,
                    stats.downloads.queuedTiles, stats.downloads.failedTiles,
                    memoryMegabytes);
    if (tileStatsState != lastUiTileStats_)
    {
        runtime.setTextsById(
            {{"tile-stat-visible", fmt::format("{}", stats.visibleTiles)},
             {"tile-stat-ready", fmt::format("{}", stats.readyTiles)},
             {"tile-stat-pending", fmt::format("{}", stats.cache.pendingTiles)},
             {"tile-stat-downloads",
              fmt::format("{}/{}", stats.downloads.activeTiles,
                          stats.downloads.queuedTiles)},
             {"tile-stat-failed", fmt::format("{}", stats.downloads.failedTiles)},
             {"tile-stat-cache", fmt::format("{:.1f} MB", memoryMegabytes)}});
        lastUiTileStats_ = tileStatsState;
    }
    const std::string centerText = fmt::format("{:.6f}, {:.6f}", center.lon, center.lat);
    if (centerText != lastCenterCoordinate_)
    {
        runtime.setValueById("center-coordinate", centerText);
        lastCenterCoordinate_ = centerText;
    }
    const GisOverlayCategories gisCategories = renderer_.GetGisOverlayCategories();
    if (!lastUiGisCategories_.has_value() || *lastUiGisCategories_ != gisCategories)
    {
        writeSearchTrace("Controller", "gis_ui_sync",
                         fmt::format("categories={}", gisCategoriesValue(gisCategories)));
        syncGisCheckboxes(runtime, gisCategories);
        lastUiGisCategories_ = gisCategories;
    }
    runtime.setVisibleById("gis-overlay-panel", gisOverlayPopupOpen_);
    runtime.setAttributeById("gis-overlay-toggle", "aria-expanded",
                             gisOverlayPopupOpen_ ? "true" : "false");

    std::string_view paletteTargetLabel = kGisCategoryControls.front().label;
    for (size_t index = 0; index < kGisCategoryControls.size(); ++index)
    {
        const GisCategoryControl& control = kGisCategoryControls[index];
        const std::string categoryValue(control.value);
        const std::string colorValue =
            gisColorHex(renderer_.GetGisOverlayColor(control.category));
        if (lastUiGisColorValues_[index] != colorValue)
        {
            runtime.setValueById("gis-color-" + categoryValue, colorValue);
            runtime.setStyleById("gis-color-target-" + categoryValue,
                                 "background-color: " + colorValue + ";");
            lastUiGisColorValues_[index] = colorValue;
        }
        runtime.setAttributeById("gis-color-target-" + categoryValue, "aria-pressed",
                                 control.category == gisColorPaletteCategory_ ? "true"
                                                                             : "false");
        if (control.category == gisColorPaletteCategory_)
        {
            paletteTargetLabel = control.label;
        }
    }
    runtime.setTextById("gis-color-palette-target", paletteTargetLabel);
    const bool eyedropperActive = gisColorEyedropperActive_;
    runtime.setAttributeById("gis-color-eyedropper", "aria-pressed",
                             eyedropperActive ? "true" : "false");
    runtime.setAttributeById("gis-color-eyedropper", "aria-label",
                             eyedropperActive ? "取消屏幕取色" : "从屏幕吸取颜色");
    runtime.setAttributeById("gis-color-eyedropper", "title",
                             eyedropperActive ? "取消屏幕取色" : "从屏幕吸取颜色");
    const DownTool::AmapVectorColor pickerColor =
        fromHsv(HsvColor{gisColorHue_, gisColorSaturation_, gisColorBrightness_});
    const std::string pickerHex = gisColorHex(pickerColor);
    const std::string hueHex =
        gisColorHex(fromHsv(HsvColor{gisColorHue_, 1., 1.}));
    const std::string pickerState = fmt::format(
        "{}:{:.6f}:{:.6f}:{:.6f}", static_cast<int>(gisColorPaletteCategory_),
        gisColorHue_, gisColorSaturation_, gisColorBrightness_);
    if (lastUiGisPickerState_ != pickerState)
    {
        runtime.setStyleById("gis-color-toggle-swatch",
                             "background-color: " + pickerHex + ";");
        runtime.setStyleById(
            "gis-color-picker-field",
            "background-color: " + hueHex + "; background-image: "
                "linear-gradient(to bottom, transparent 0%, #000000 100%), "
                "linear-gradient(to right, #ffffff 0%, transparent 100%);");
        runtime.setStyleById("gis-color-current",
                             "background-color: " + pickerHex + ";");
        runtime.setStyleById(
            "gis-color-picker-marker",
            fmt::format("left: {:.1f}px; top: {:.1f}px;",
                        gisColorSaturation_ * kGisColorFieldWidth - 5.,
                        (1. - gisColorBrightness_) * kGisColorFieldHeight - 5.));
        runtime.setStyleById(
            "gis-color-hue-marker",
            fmt::format("top: {:.1f}px;", gisColorHue_ / 360. * kGisColorHueHeight - 2.));
        runtime.setTextById("gis-color-hue-value",
                            fmt::format("{:.0f}°", gisColorHue_));
        runtime.setTextById("gis-color-saturation-value",
                            fmt::format("{:.0f}%", gisColorSaturation_ * 100.));
        runtime.setTextById("gis-color-brightness-value",
                            fmt::format("{:.0f}%", gisColorBrightness_ * 100.));
        lastUiGisPickerState_ = pickerState;
    }
    runtime.setVisibleById("gis-color-panel", gisColorPopupOpen_);
}

void DomDownToolPreview::PreviewController::schedulePageChange(Page page,
                                                                skui::Runtime& runtime)
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (window && PostMessageW(window, kPreviewPageChangeMessage,
                               static_cast<WPARAM>(page), 0))
    {
        return;
    }

    switch (page)
    {
    case Page::Preview:
        OpenPreviewPage(runtime);
        break;
    case Page::Export:
        OpenExportPage(runtime);
        break;
    case Page::Tasks:
        OpenTaskPage(runtime);
        break;
    case Page::FbxPreview:
        OpenTaskPage(runtime);
        break;
    }
}

void DomDownToolPreview::PreviewController::OpenExportPage(skui::Runtime& runtime)
{
    if (page_ == Page::FbxPreview)
    {
        restoreFbxInputMethod();
        dx12FbxRenderer_.clear();
        dragging_ = false;
        fbxRedrawPosted_.store(false, std::memory_order_relaxed);
        immediateFbxRedrawPosted_ = false;
        fbxDragMode_ = FbxDragMode::None;
        fbxDragRedrawThrottle_.reset();
        setFbxKeyboardPanDirection(window_.load(std::memory_order_relaxed), 0, 0, 0);
        stopFbxPreviewFrameLoop(window_.load(std::memory_order_relaxed));
        resetFbxPreviewFrameRate();
    }
    taskClearConfirmOpen_ = false;
    taskDeleteTargetId_.reset();
    std::optional<ExportConfiguration> configuration =
        std::move(pendingExportConfiguration_);
    pendingExportConfiguration_.reset();
    InitializeExportDraft(runtime, configuration ? &*configuration : nullptr);
    page_ = Page::Export;
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::OpenTaskPage(skui::Runtime& runtime)
{
    if (page_ == Page::FbxPreview)
    {
        restoreFbxInputMethod();
        dx12FbxRenderer_.clear();
        dragging_ = false;
        fbxRedrawPosted_.store(false, std::memory_order_relaxed);
        immediateFbxRedrawPosted_ = false;
        fbxDragMode_ = FbxDragMode::None;
        fbxDragRedrawThrottle_.reset();
        setFbxKeyboardPanDirection(window_.load(std::memory_order_relaxed), 0, 0, 0);
        stopFbxPreviewFrameLoop(window_.load(std::memory_order_relaxed));
        resetFbxPreviewFrameRate();
    }
    taskClearConfirmOpen_ = false;
    taskDeleteTargetId_.reset();
    pendingExportConfiguration_.reset();
    page_ = Page::Tasks;
    UpdateTaskUi(runtime);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::OpenPreviewPage(skui::Runtime& runtime)
{
    if (page_ == Page::FbxPreview)
    {
        restoreFbxInputMethod();
        dx12FbxRenderer_.clear();
        dragging_ = false;
        fbxRedrawPosted_.store(false, std::memory_order_relaxed);
        immediateFbxRedrawPosted_ = false;
        fbxDragMode_ = FbxDragMode::None;
        fbxDragRedrawThrottle_.reset();
        setFbxKeyboardPanDirection(window_.load(std::memory_order_relaxed), 0, 0, 0);
        stopFbxPreviewFrameLoop(window_.load(std::memory_order_relaxed));
        resetFbxPreviewFrameRate();
    }
    taskClearConfirmOpen_ = false;
    taskDeleteTargetId_.reset();
    pendingExportConfiguration_.reset();
    page_ = Page::Preview;
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::OpenFbxPreviewPage(
    skui::Runtime& runtime, const std::filesystem::path& taskRoot, std::string title)
{
    fbxRedrawPosted_.store(false, std::memory_order_relaxed);
    dragging_ = false;
    immediateFbxRedrawPosted_ = false;
    fbxDragMode_ = FbxDragMode::None;
    fbxDragRedrawThrottle_.reset();
    const HWND window = window_.load(std::memory_order_relaxed);
    setFbxKeyboardPanDirection(window, 0, 0, 0);
    resetFbxPreviewFrameRate();
    taskClearConfirmOpen_ = false;
    taskDeleteTargetId_.reset();
    fbxPreviewTaskRoot_ = taskRoot;
    fbxPreviewTitle_ = std::move(title);
    page_ = Page::FbxPreview;
    disableFbxInputMethod(window);
    startFbxPreviewFrameLoop(window);
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::InitializeExportDraft(
    skui::Runtime& runtime, const ExportConfiguration* configuration)
{
    const MapSourceSummary source = renderer_.activeSource();
    const DownTool::LonLat center = renderer_.center();
    exportLongitudeValue_ = fmt::format("{:.6f}", configuration ?
                                        configuration->centerWgs84.lon : center.lon);
    exportLatitudeValue_ = fmt::format("{:.6f}", configuration ?
                                       configuration->centerWgs84.lat : center.lat);
    exportRadiusValue_ = fmt::format("{:.0f}", configuration ?
                                     configuration->radiusMeters :
                                     renderer_.GetExportRadiusMeters());
    exportZoomValue_ = fmt::format("{:.0f}", configuration ?
                                   configuration->zoom : std::round(renderer_.zoom()));
    exportMapSourceIds_.clear();
    if (configuration)
    {
        for (const SatelliteExportSource& exportSource : configuration->satelliteSources)
        {
            exportMapSourceIds_.insert(exportSource.id);
        }
    }
    else if (source.id != "none" && isExportProviderSource(source.id))
    {
        exportMapSourceIds_.insert(source.id);
    }
    exportOutputValue_ = configuration && !configuration->outputRoot.empty() ?
        pathToUtf8(configuration->outputRoot) : "";
    exportValidationMessage_.clear();
    exportTiles_ = configuration ? configuration->exportTiles : false;
    exportMergedImage_ = configuration ? configuration->exportMergedImage : false;
    exportFbx_ = configuration ? configuration->exportFbx : false;
    exportTerrainHeight_ = configuration && configuration->exportFbx ?
        configuration->exportTerrainHeight : false;
    exportGeoJson_ = configuration ? configuration->exportGeoJson : false;
    exportBuildingHeightColors_ = configuration ? configuration->colorBuildingsByHeight : false;
    exportBuildingFbx_ = configuration ? configuration->exportBuildingFbx : false;
    exportBuildingWallTexturePathValue_ = configuration ?
        pathToUtf8(configuration->buildingWallTexturePath) : "贴图/墙体";
    exportBuildingRoofTexturePathValue_ = configuration ?
        pathToUtf8(configuration->buildingRoofTexturePath) : "贴图/房顶";
    exportBuildingWallTextureModeValue_ = configuration ?
        buildingFbxTextureModeValue(configuration->buildingFbxWallTextureMode) : "scattered";
    exportBuildingRoofTextureModeValue_ = configuration ?
        buildingFbxRoofTextureModeValue(configuration->buildingFbxRoofTextureMode) :
        "scattered";
    exportBuildingAtlasFormatValue_ = configuration ?
        buildingFbxAtlasImageFormatValue(configuration->buildingFbxAtlasImageFormat) : "png";
    exportBuildingLayerHeightValue_ = configuration ?
        fmt::format("{:.0f}", configuration->buildingLayerHeightCentimeters) : "45";
    exportBuildingMaxWallMaterialsValue_ = configuration ?
        std::to_string(configuration->buildingMaxWallMaterialsPerBucket) : "1";
    exportBuildingTextureSeedValue_ = configuration ?
        std::to_string(configuration->buildingTextureRandomSeed) : "0";
    exportGisOverlayOnSatellite_ = configuration && configuration->gisModes.contains(
        GisExportMode::OverlayOnSatellite);
    exportGisLayersMerged_ = configuration && configuration->gisModes.contains(
        GisExportMode::MergeSelectedLayers);
    exportGisLayersSeparate_ = configuration && configuration->gisModes.contains(
        GisExportMode::SeparateLayers);
    if (configuration)
    {
        exportMergedFormat_ = configuration->mergedImageFormat;
        exportMergedResolutionValue_ = std::to_string(configuration->mergedImageResolution);
        exportFbxSizeValue_ = std::to_string(configuration->fbxTextureSize);
        exportFbxTextureFormatValue_ = imageFormatValue(configuration->fbxTextureFormat);
        exportTerrainSampleRateValue_ = fmt::format(
            "{:.0f}", configuration->fbxTerrainSampleRateMeters);
        exportGisColors_ = configuration->gisColors;
    }
    else
    {
        exportFbxTextureFormatValue_ = "png";
        exportGisColors_.clear();
        for (const DownTool::AmapVectorRenderCategory category : kExportCategories)
        {
            exportGisColors_[category] = renderer_.GetGisOverlayColor(previewCategory(category));
        }
    }

    const GisOverlayCategories previewGisCategories =
        renderer_.GetGisOverlayCategories();
    const bool allGisCategoriesSelected = configuration ?
        configuration->gisCategories.contains(DownTool::AmapVectorRenderCategory::All) :
        previewGisCategories.contains(GisOverlayCategory::All);
    exportGisGroups_.clear();
    uint64_t groupId = 1;
    for (const DownTool::AmapVectorRenderCategory category : kExportCategories)
    {
        const bool selected = configuration ?
            allGisCategoriesSelected || configuration->gisCategories.contains(category) :
            allGisCategoriesSelected || previewGisCategories.contains(previewCategory(category));
        exportGisGroups_.push_back(ExportGisUiGroup{groupId++, {category}, selected});
    }

    runtime.setValueById("export-lon", exportLongitudeValue_);
    runtime.setValueById("export-lat", exportLatitudeValue_);
    runtime.setValueById("export-config-radius", exportRadiusValue_);
    runtime.setValueById("export-zoom", exportZoomValue_);
    runtime.setValueById("export-output", exportOutputValue_);
    runtime.setValueById("export-merged-format", exportMergedFormat_);
    runtime.setValueById("export-merged-resolution", exportMergedResolutionValue_);
    runtime.setValueById("export-fbx-size", exportFbxSizeValue_);
    runtime.setValueById("export-fbx-texture-format", exportFbxTextureFormatValue_);
    runtime.setValueById(
        "export-terrain-sample-rate", exportTerrainSampleRateValue_);
    runtime.setValueById("export-building-wall-textures",
                         exportBuildingWallTexturePathValue_);
    runtime.setValueById("export-building-roof-textures",
                         exportBuildingRoofTexturePathValue_);
    runtime.setValueById("export-building-wall-texture-mode",
                         exportBuildingWallTextureModeValue_);
    runtime.setValueById("export-building-roof-texture-mode",
                         exportBuildingRoofTextureModeValue_);
    runtime.setValueById("export-building-atlas-format",
                         exportBuildingAtlasFormatValue_);
    runtime.setValueById("export-building-layer-height",
                         exportBuildingLayerHeightValue_);
    runtime.setValueById("export-building-max-wall-materials",
                         exportBuildingMaxWallMaterialsValue_);
    runtime.setValueById("export-building-texture-seed", exportBuildingTextureSeedValue_);
    for (const auto& [sourceId, controlId] : kExportProviderControls)
    {
        setCheckboxChecked(runtime, controlId, exportMapSourceIds_.contains(
            std::string(sourceId)));
    }
    setCheckboxChecked(runtime, "export-satellite-tiles", exportTiles_);
    setCheckboxChecked(runtime, "export-satellite-merged", exportMergedImage_);
    setCheckboxChecked(runtime, "export-gis-combine-overlay",
                       exportGisOverlayOnSatellite_);
    setCheckboxChecked(runtime, "export-gis-combine-merged", exportGisLayersMerged_);
    setCheckboxChecked(runtime, "export-gis-combine-separate",
                       exportGisLayersSeparate_);
    setCheckboxChecked(runtime, "export-fbx", exportFbx_);
    setCheckboxChecked(runtime, "export-terrain-height", exportTerrainHeight_);
    setCheckboxChecked(runtime, "export-geojson", exportGeoJson_);
    setCheckboxChecked(runtime, "export-building-height-colors", exportBuildingHeightColors_);
    setCheckboxChecked(runtime, "export-building-fbx", exportBuildingFbx_);
    UpdateExportUi(runtime);
}

void DomDownToolPreview::PreviewController::UpdateExportUi(skui::Runtime& runtime)
{
    const std::optional<double> radius = parseFiniteDouble(exportRadiusValue_);
    runtime.setTextById(
        "export-config-summary",
        fmt::format("中心 {:.5f}, {:.5f} · z{} · 半径 {} 米",
                    parseFiniteDouble(exportLongitudeValue_).value_or(0.),
                    parseFiniteDouble(exportLatitudeValue_).value_or(0.),
                    exportZoomValue_, radius.value_or(0.)));
    runtime.setTextById("export-validation-message", exportValidationMessage_);

    for (const ExportGisUiGroup& group : exportGisGroups_)
    {
        setCheckboxChecked(runtime, fmt::format("export-gis-group-{}", group.id),
                           group.selected);
    }

    const bool hasGisData = std::ranges::any_of(
        exportGisGroups_, [](const ExportGisUiGroup& group) { return group.selected; });
    const bool canExportBuildingFbx = exportFbx_ && hasSelectedBuildingExportGroup();
    const auto setDisabled = [&runtime](std::string_view id, bool disabled)
    {
        if (disabled)
        {
            runtime.setAttributeById(id, "disabled", "");
        }
        else
        {
            runtime.removeAttributeById(id, "disabled");
        }
    };
    setDisabled("export-geojson", !hasGisData);
    setDisabled("export-gis-combine-merged", !hasGisData);
    setDisabled("export-gis-combine-separate", !hasGisData);
    setDisabled("export-gis-combine-overlay",
                !hasGisData || exportMapSourceIds_.empty());
    setDisabled("export-fbx-size", !exportFbx_);
    setDisabled("export-fbx-texture-format", !exportFbx_);
    setDisabled("export-terrain-height", !exportFbx_);
    setDisabled("export-terrain-sample-rate", !exportFbx_ || !exportTerrainHeight_);
    setDisabled("export-building-wall-textures", !exportBuildingFbx_);
    setDisabled("export-building-roof-textures",
                !exportBuildingFbx_ || exportBuildingRoofTextureModeValue_ == "satellite");
    setDisabled("export-building-wall-texture-mode", !exportBuildingFbx_);
    setDisabled("export-building-roof-texture-mode", !exportBuildingFbx_);
    setDisabled(
        "export-building-atlas-format",
        !exportBuildingFbx_ ||
            (exportBuildingWallTextureModeValue_ != "merged-atlas" &&
             exportBuildingRoofTextureModeValue_ != "merged-atlas"));
    setDisabled("export-building-layer-height", !exportBuildingFbx_);
    setDisabled("export-building-max-wall-materials", !exportBuildingFbx_);
    setDisabled("export-building-texture-seed", !exportBuildingFbx_);
    runtime.setTextById(
        "export-building-fbx-hint",
        exportBuildingFbx_ && canExportBuildingFbx ?
            buildingFbxTextureModeHint(exportBuildingRoofTextureModeValue_ == "satellite" ?
                exportBuildingRoofTextureModeValue_ : exportBuildingWallTextureModeValue_) :
            "默认关闭；启用后需要同时勾选 FBX 和 GIS 建筑图层。");
    runtime.setVisibleById("export-terrain-height-item", exportFbx_);
    setDisabled("export-merged-format", !exportMergedImage_);
    setDisabled("export-merged-resolution", !exportMergedImage_);
    setDisabled("export-building-height-colors", !hasGisData);
    setCheckboxChecked(runtime, "export-geojson", exportGeoJson_);
    setCheckboxChecked(runtime, "export-gis-combine-overlay",
                       exportGisOverlayOnSatellite_);
    setCheckboxChecked(runtime, "export-gis-combine-merged", exportGisLayersMerged_);
    setCheckboxChecked(runtime, "export-gis-combine-separate",
                       exportGisLayersSeparate_);
    setCheckboxChecked(runtime, "export-building-height-colors",
                       exportBuildingHeightColors_);
    setCheckboxChecked(runtime, "export-building-fbx", exportBuildingFbx_);
    setCheckboxChecked(runtime, "export-terrain-height", exportTerrainHeight_);
}

void DomDownToolPreview::PreviewController::UpdateTaskUi(skui::Runtime& runtime)
{
    const std::vector<ExportTaskRecord> records = exportTaskManager_.GetRecords();
    size_t queuedCount = 0;
    size_t runningCount = 0;
    size_t succeededCount = 0;
    size_t failedCount = 0;
    std::string listHtml;
    for (const ExportTaskRecord& record : records)
    {
        switch (record.state)
        {
        case ExportTaskState::Queued:
            ++queuedCount;
            break;
        case ExportTaskState::Running:
            ++runningCount;
            break;
        case ExportTaskState::Succeeded:
            ++succeededCount;
            break;
        case ExportTaskState::Failed:
        case ExportTaskState::Canceled:
            ++failedCount;
            break;
        }
        const std::string stateValue =
            record.state == ExportTaskState::Queued ? "queued" :
            record.state == ExportTaskState::Running ? "running" :
            record.state == ExportTaskState::Succeeded ? "succeeded" :
            record.state == ExportTaskState::Canceled ? "canceled" : "failed";
        const std::string progressText = record.total == 0 ?
            fmt::format("{:.0f}%", record.progress * 100.) :
            fmt::format("{:.0f}% · {}/{}", record.progress * 100.,
                        record.completed, record.total);
        const bool finished = record.state == ExportTaskState::Succeeded ||
            record.state == ExportTaskState::Failed ||
            record.state == ExportTaskState::Canceled;
        const bool canceling = record.state == ExportTaskState::Running &&
            record.stage == "正在取消";
        const std::string action = record.state == ExportTaskState::Queued ||
                record.state == ExportTaskState::Running ?
            fmt::format("<button id=\"task-cancel-{}\" class=\"danger-command\" "
                        "type=\"button\" data-action=\"cancel-export-task\"{}>"
                        "{}</button>",
                        escapeHtml(record.id), canceling ? " disabled" : "",
                        canceling ? "正在取消" : "取消") : "";
        const std::string openOutput = fmt::format(
            "<button id=\"task-open-{}\" class=\"task-open-output\" type=\"button\" "
            "data-action=\"open-export-task-output\" title=\"打开导出路径\" "
            "aria-label=\"打开导出路径\"><svg viewBox=\"0 0 24 24\" width=\"17\" "
            "height=\"17\" pointer-events=\"none\" "
            "aria-hidden=\"true\"><path d=\"M3 7a2 2 0 0 1 2-2h4l2 2.5h8a2 2 0 "
            "0 1 2 2V17a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z\" fill=\"none\" "
            "stroke=\"#cbd4d0\" stroke-width=\"1.6\" stroke-linejoin=\"round\"/>"
            "</svg></button>",
            escapeHtml(record.id));
        const std::string openConfiguration = fmt::format(
            "<button id=\"task-config-{}\" class=\"task-icon-button "
            "task-reopen-config\" type=\"button\" "
            "data-action=\"open-export-task-config\" title=\"按此任务参数打开导出配置\" "
            "aria-label=\"按此任务参数打开导出配置\"><svg viewBox=\"0 0 24 24\" "
            "width=\"17\" height=\"17\" pointer-events=\"none\" "
            "aria-hidden=\"true\"><path d=\"M4 6h7M15 6h5M4 12h3M11 12h9M4 18h9M17 18h3\" "
            "fill=\"none\" stroke=\"#88d9be\" stroke-width=\"1.6\" "
            "stroke-linecap=\"round\"/><circle cx=\"13\" cy=\"6\" r=\"2\" "
            "fill=\"#1e2121\" stroke=\"#88d9be\" stroke-width=\"1.6\"/>"
            "<circle cx=\"9\" cy=\"12\" r=\"2\" fill=\"#1e2121\" "
            "stroke=\"#88d9be\" stroke-width=\"1.6\"/><circle cx=\"15\" cy=\"18\" "
            "r=\"2\" fill=\"#1e2121\" stroke=\"#88d9be\" stroke-width=\"1.6\"/>"
            "</svg></button>",
            escapeHtml(record.id));
        const bool resumable = record.state == ExportTaskState::Failed ||
            record.state == ExportTaskState::Canceled;
        const std::string resumeAction = resumable ? fmt::format(
            "<button id=\"task-resume-{}\" class=\"task-icon-button task-resume\" "
            "type=\"button\" data-action=\"resume-export-task\" "
            "title=\"继续任务（复用下载缓存）\" aria-label=\"继续任务（复用下载缓存）\">"
            "<svg viewBox=\"0 0 24 24\" width=\"17\" height=\"17\" "
            "pointer-events=\"none\" aria-hidden=\"true\"><path d=\"M20 11a8 8 0 1 0 "
            "2 5.3M20 5v6h-6\" fill=\"none\" stroke=\"#e4c779\" "
            "stroke-width=\"1.6\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
            "</svg></button>",
            escapeHtml(record.id)) : "";
        const std::string finishedActions = finished ? fmt::format(
            "<button id=\"task-remove-{}\" class=\"task-icon-button "
            "task-remove-record\" type=\"button\" data-action=\"remove-export-task\" "
            "title=\"移除任务记录（保留导出文件）\" aria-label=\"移除任务记录\">"
            "<svg viewBox=\"0 0 24 24\" width=\"17\" height=\"17\" "
            "pointer-events=\"none\" aria-hidden=\"true\"><path "
            "d=\"M6 6l12 12M18 6L6 18\" fill=\"none\" stroke=\"#cbd4d0\" "
            "stroke-width=\"1.6\" stroke-linecap=\"round\"/></svg></button>"
            "<button id=\"task-delete-{}\" class=\"task-icon-button "
            "task-delete-output\" type=\"button\" "
            "data-action=\"delete-export-task-output\" title=\"永久删除导出文件\" "
            "aria-label=\"永久删除导出文件\"><svg viewBox=\"0 0 24 24\" width=\"17\" "
            "height=\"17\" pointer-events=\"none\" "
            "aria-hidden=\"true\"><path d=\"M4 7h16M9 7V4h6v3M7 7l1 13h8l1-13"
            "M10 11v5M14 11v5\" fill=\"none\" stroke=\"#ffaaa2\" "
            "stroke-width=\"1.6\" stroke-linecap=\"round\" "
            "stroke-linejoin=\"round\"/></svg></button>",
             escapeHtml(record.id), escapeHtml(record.id)) : "";
        const bool canPreviewFbx = record.state == ExportTaskState::Succeeded &&
            taskDeclaresFbxOutput(record);
        const std::string fbxPreviewAction = canPreviewFbx ?
            fmt::format(
                "<button id=\"task-preview-{}\" class=\"task-icon-button task-preview\" "
                "type=\"button\" data-action=\"preview-export-task-fbx\" "
                "title=\"预览 FBX 网格\" aria-label=\"预览 FBX 网格\">"
                "<svg viewBox=\"0 0 24 24\" width=\"17\" height=\"17\" "
                "pointer-events=\"none\" aria-hidden=\"true\"><path d=\"M3 12 12 4l9 8-9 8z\" "
                "fill=\"none\" stroke=\"#8bd6c2\" stroke-width=\"1.6\" "
                "stroke-linejoin=\"round\"/><path d=\"M7 10v6l5 3 5-3v-6M12 4v7m-5-1 5 3 5-3\" "
                "fill=\"none\" stroke=\"#8bd6c2\" stroke-width=\"1.6\" "
                "stroke-linejoin=\"round\" stroke-linecap=\"round\"/></svg></button>",
                escapeHtml(record.id)) : "";
        const std::string errorHtml = record.error.empty() ? "" :
            fmt::format("<div class=\"task-error\" title=\"{}\">失败原因：{}</div>",
                        escapeHtml(record.error), escapeHtml(record.error));
        listHtml += fmt::format(
            "<article class=\"task-row\"><div><div class=\"task-title\">{}</div>"
            "<div class=\"task-detail\">{} · {}</div><div class=\"task-output\">{}</div>"
            "{}</div><div><div class=\"task-stage\">"
            "<span>{}</span><span>{}</span></div><div class=\"progress-track\"><div "
            "class=\"progress-value\" style=\"width: {:.2f}%;\"></div></div></div>"
            "<div><span class=\"task-state {}\">{}</span><div class=\"task-time\">{}</div></div>"
            "<div class=\"task-row-actions\">{}</div></article>",
            escapeHtml(record.title), escapeHtml(record.detail), escapeHtml(record.createdAt),
            escapeHtml(record.outputPath), errorHtml, escapeHtml(record.stage),
            progressText, record.progress * 100., stateValue,
            ExportTaskStateName(record.state), escapeHtml(record.finishedAt),
             fbxPreviewAction + openOutput + openConfiguration + resumeAction + action +
                 finishedActions);
    }
    if (records.empty())
    {
        listHtml = "<div class=\"task-empty\">暂无导出任务</div>";
    }
    const std::string taskListHtml =
        fmt::format("<div id=\"export-task-list-items\" class=\"task-list-items\">{}</div>",
                    listHtml);
    if (taskListHtml != lastTaskListHtml_)
    {
        if (runtime.replaceHtmlById("export-task-list-items", taskListHtml))
        {
            lastTaskListHtml_ = taskListHtml;
        }
        else
        {
            lastTaskListHtml_.clear();
        }
    }
    runtime.setTextsById({
        {"task-count-queued", std::to_string(queuedCount)},
        {"task-count-running", std::to_string(runningCount)},
        {"task-count-succeeded", std::to_string(succeededCount)},
        {"task-count-failed", std::to_string(failedCount)},
        {"task-clear-count", std::to_string(succeededCount + failedCount)}});
    if (succeededCount + failedCount == 0)
    {
        runtime.setAttributeById("tasks-clear-finished", "disabled", "");
    }
    else
    {
        runtime.removeAttributeById("tasks-clear-finished", "disabled");
    }
}

void DomDownToolPreview::PreviewController::setExportGisGroupSelected(
    uint64_t groupId, bool selected)
{
    const auto group = std::find_if(
        exportGisGroups_.begin(), exportGisGroups_.end(),
        [groupId](const ExportGisUiGroup& item) { return item.id == groupId; });
    if (group == exportGisGroups_.end())
    {
        return;
    }
    group->selected = selected;
    const bool hasSelectedGroup = std::ranges::any_of(
        exportGisGroups_, [](const ExportGisUiGroup& item) { return item.selected; });
    if (!hasSelectedGroup)
    {
        exportGeoJson_ = false;
        exportGisOverlayOnSatellite_ = false;
        exportGisLayersMerged_ = false;
        exportGisLayersSeparate_ = false;
        exportBuildingHeightColors_ = false;
    }
}

bool DomDownToolPreview::PreviewController::hasSelectedBuildingExportGroup() const
{
    return std::ranges::any_of(
        exportGisGroups_,
        [](const ExportGisUiGroup& group)
        {
            return group.selected && group.categories.contains(
                DownTool::AmapVectorRenderCategory::Building);
        });
}

std::optional<DomDownToolPreview::ExportConfiguration>
DomDownToolPreview::PreviewController::BuildExportConfiguration(std::string& error) const
{
    const std::optional<double> lon = parseFiniteDouble(exportLongitudeValue_);
    const std::optional<double> lat = parseFiniteDouble(exportLatitudeValue_);
    const std::optional<double> radius = parseFiniteDouble(exportRadiusValue_);
    const std::optional<uint32_t> zoom = parseUint32(exportZoomValue_);
    const std::optional<uint32_t> mergedResolution = parseUint32(exportMergedResolutionValue_);
    const std::optional<uint32_t> fbxSize = parseUint32(exportFbxSizeValue_);
    const std::optional<double> terrainSampleRate =
        parseFiniteDouble(exportTerrainSampleRateValue_);
    const bool buildingFbxRequested = exportBuildingFbx_;
    const std::optional<double> buildingLayerHeight = buildingFbxRequested ?
        parseFiniteDouble(exportBuildingLayerHeightValue_) : std::optional<double>(45.);
    const std::optional<uint32_t> buildingMaxWallMaterials = buildingFbxRequested ?
        parseUint32(exportBuildingMaxWallMaterialsValue_) : std::optional<uint32_t>(1);
    const std::optional<uint32_t> buildingTextureSeed = buildingFbxRequested ?
        parseUint32(exportBuildingTextureSeedValue_) : std::optional<uint32_t>(0);
    if (!lon || *lon < -180. || *lon > 180. || !lat || *lat < -85. || *lat > 85.)
    {
        error = "中心经纬度无效。";
        return std::nullopt;
    }
    if (!radius || *radius <= 0.)
    {
        error = "导出半径必须大于 0 米。";
        return std::nullopt;
    }
    if (!zoom || *zoom > 30 || !mergedResolution || *mergedResolution == 0 ||
        !fbxSize || !terrainSampleRate || *terrainSampleRate <= 0.)
    {
        error = "层级、合并大图尺寸、FBX 贴图尺寸或高度采样率无效。";
        return std::nullopt;
    }
    if (buildingFbxRequested &&
        (exportBuildingWallTexturePathValue_.empty() ||
         (exportBuildingRoofTextureModeValue_ != "satellite" &&
          exportBuildingRoofTexturePathValue_.empty()) || !buildingLayerHeight ||
         *buildingLayerHeight <= 0. || !buildingMaxWallMaterials ||
         *buildingMaxWallMaterials == 0 || !buildingTextureSeed))
    {
        error = "建筑 FBX 贴图目录、楼层高度、材质上限或随机种子无效。";
        return std::nullopt;
    }

    ExportConfiguration configuration;
    const std::vector<MapSourceSummary> sources = renderer_.sources();
    configuration.centerWgs84 = {*lon, *lat};
    configuration.radiusMeters = *radius;
    configuration.zoom = *zoom;
    configuration.outputRoot = std::filesystem::u8path(exportOutputValue_);
    for (const std::string& sourceId : exportMapSourceIds_)
    {
        const auto source = std::find_if(
            sources.begin(), sources.end(),
            [&sourceId](const MapSourceSummary& item) { return item.id == sourceId; });
        if (source == sources.end() || source->id == "none")
        {
            error = "卫星图来源无效。";
            return std::nullopt;
        }
        configuration.satelliteSources.push_back(
            SatelliteExportSource{source->id, source->name, source->cacheRoot});
    }
    configuration.gisCategories.clear();
    for (const ExportGisUiGroup& group : exportGisGroups_)
    {
        if (group.selected)
        {
            configuration.gisCategories.insert(group.categories.begin(), group.categories.end());
        }
    }
    if (exportGisOverlayOnSatellite_)
    {
        configuration.gisModes.insert(GisExportMode::OverlayOnSatellite);
    }
    if (exportGisLayersMerged_)
    {
        configuration.gisModes.insert(GisExportMode::MergeSelectedLayers);
    }
    if (exportGisLayersSeparate_)
    {
        configuration.gisModes.insert(GisExportMode::SeparateLayers);
    }
    configuration.exportTiles = exportTiles_;
    configuration.exportMergedImage = exportMergedImage_;
    configuration.exportFbx = exportFbx_;
    configuration.exportTerrainHeight = exportFbx_ && exportTerrainHeight_;
    configuration.exportGeoJson = exportGeoJson_;
    configuration.colorBuildingsByHeight = exportBuildingHeightColors_;
    configuration.exportBuildingFbx = buildingFbxRequested;
    configuration.buildingWallTexturePath = std::filesystem::u8path(
        exportBuildingWallTexturePathValue_);
    configuration.buildingRoofTexturePath = std::filesystem::u8path(
        exportBuildingRoofTexturePathValue_);
    configuration.buildingFbxWallTextureMode = buildingFbxTextureModeFromValue(
        exportBuildingWallTextureModeValue_);
    configuration.buildingFbxRoofTextureMode = buildingFbxRoofTextureModeFromValue(
        exportBuildingRoofTextureModeValue_);
    configuration.buildingFbxAtlasImageFormat = buildingFbxAtlasImageFormatFromValue(
        exportBuildingAtlasFormatValue_);
    configuration.buildingLayerHeightCentimeters = *buildingLayerHeight;
    configuration.buildingMaxWallMaterialsPerBucket = *buildingMaxWallMaterials;
    configuration.buildingTextureRandomSeed = *buildingTextureSeed;
    configuration.mergedImageResolution = *mergedResolution;
    configuration.mergedImageFormat = exportMergedFormat_ == "jpg" ? "jpg" : "png";
    configuration.fbxTextureSize = *fbxSize;
    configuration.fbxTextureFormat = imageFormatValue(exportFbxTextureFormatValue_);
    configuration.fbxTerrainSampleRateMeters = *terrainSampleRate;
    if (exportTerrainHeight_ && !exportFbx_)
    {
        error = "导出高度需要同时选择 FBX。";
        return std::nullopt;
    }
    if (buildingFbxRequested && !exportFbx_)
    {
        error = "建筑 FBX 需要同时选择 FBX 切片与网格。";
        return std::nullopt;
    }
    if (buildingFbxRequested && !hasSelectedBuildingExportGroup())
    {
        error = "建筑 FBX 需要同时选择 GIS 建筑图层。";
        return std::nullopt;
    }
    if (buildingFbxRequested && exportBuildingRoofTextureModeValue_ == "satellite" &&
        exportMapSourceIds_.empty())
    {
        error = "屋顶卫星图模式需要至少选择一个卫星图来源。";
        return std::nullopt;
    }
    for (const DownTool::AmapVectorRenderCategory category : kExportCategories)
    {
        const auto color = exportGisColors_.find(category);
        const DownTool::AmapVectorColor value = color != exportGisColors_.end() ?
            color->second : renderer_.GetGisOverlayColor(previewCategory(category));
        configuration.gisColors[category] = value;
    }
    if (!configuration.exportTiles && !configuration.exportMergedImage &&
        !configuration.exportFbx && !configuration.exportGeoJson)
    {
        error = "至少选择一种输出内容。";
        return std::nullopt;
    }
    const bool visualOutputRequested = configuration.exportTiles ||
        configuration.exportMergedImage || configuration.exportFbx;
    if (!configuration.gisModes.empty() && !visualOutputRequested)
    {
        error = "GIS 组合方式需要选择 XYZ、合并大图或 FBX。";
        return std::nullopt;
    }
    if (!configuration.gisCategories.empty() && visualOutputRequested &&
        configuration.gisModes.empty() && !configuration.exportGeoJson &&
        !configuration.exportBuildingFbx)
    {
        error = "所选 GIS 数据需要至少一种 GIS 组合方式。";
        return std::nullopt;
    }
    const bool satelliteRequested = !configuration.satelliteSources.empty() &&
        visualOutputRequested;
    const bool gisVisualRequested = !configuration.gisCategories.empty() &&
        ((!configuration.gisModes.empty() && visualOutputRequested) ||
         configuration.exportBuildingFbx);
    if (configuration.gisModes.contains(GisExportMode::OverlayOnSatellite) &&
        configuration.satelliteSources.empty())
    {
        error = "卫星图导出需要选择地图来源。";
        return std::nullopt;
    }
    if (!satelliteRequested && !gisVisualRequested && !configuration.exportGeoJson)
    {
        error = "所选导出类型没有对应的卫星图或 GIS 组合方式。";
        return std::nullopt;
    }
    if ((configuration.exportGeoJson || !configuration.gisModes.empty() ||
         configuration.exportBuildingFbx) &&
        configuration.gisCategories.empty())
    {
        error = "GIS 输出至少需要选择一个 GIS 数据图层。";
        return std::nullopt;
    }
    return configuration;
}

void DomDownToolPreview::PreviewController::SubmitExport(skui::Runtime& runtime)
{
    std::string error;
    const std::optional<ExportConfiguration> configuration =
        BuildExportConfiguration(error);
    if (!configuration.has_value())
    {
        exportValidationMessage_ = error;
        UpdateExportUi(runtime);
        requestRedraw();
        return;
    }
    const std::string taskId = exportTaskManager_.Enqueue(*configuration);
    (void)taskId;
    exportValidationMessage_.clear();
    schedulePageChange(Page::Tasks, runtime);
}

void DomDownToolPreview::PreviewController::submitCenterCoordinate(
    skui::Runtime& runtime, std::string_view value)
{
    const std::optional<DownTool::LonLat> coordinate = parseCenterCoordinate(value);
    if (coordinate.has_value())
    {
        const std::string preferredMultidateYear = GetSelectedTianDiTuMultidateYear();
        renderer_.setView(*coordinate, renderer_.zoom());
        renderer_.setLocationMarker(std::nullopt);
        RefreshTianDiTuMultidateAfterViewChange(runtime, preferredMultidateYear);
    }
    lastCenterCoordinate_.clear();
    updateUi(runtime);
    requestRedraw();
}

void DomDownToolPreview::PreviewController::scheduleImmediateMapRedraw(HWND window)
{
    if (immediateMapRedrawPosted_ ||
        !mapDragRedrawThrottle_.shouldRefresh(MapHoverRefreshThrottle::Clock::now()))
    {
        return;
    }

    immediateMapRedrawPosted_ = true;
    if (!PostMessageW(window, kPreviewImmediateMapRedrawMessage, 0, 0))
    {
        immediateMapRedrawPosted_ = false;
    }
}

void DomDownToolPreview::PreviewController::scheduleImmediateFbxRedraw(HWND window,
                                                                      bool throttle)
{
    if (immediateFbxRedrawPosted_ || !window)
    {
        return;
    }
    if (throttle &&
        !fbxDragRedrawThrottle_.shouldRefresh(MapHoverRefreshThrottle::Clock::now()))
    {
        return;
    }

    immediateFbxRedrawPosted_ = true;
    if (!PostMessageW(window, kPreviewImmediateFbxRedrawMessage, 0, 0))
    {
        immediateFbxRedrawPosted_ = false;
    }
}

bool DomDownToolPreview::PreviewController::hasFbxKeyboardPanDirection() const
{
    return fbxKeyboardRightDirection_ != 0 || fbxKeyboardForwardDirection_ != 0 ||
        fbxKeyboardVerticalDirection_ != 0;
}

void DomDownToolPreview::PreviewController::setFbxKeyboardPanDirection(
    HWND window, int rightDirection, int forwardDirection, int verticalDirection)
{
    rightDirection = std::clamp(rightDirection, -1, 1);
    forwardDirection = std::clamp(forwardDirection, -1, 1);
    verticalDirection = std::clamp(verticalDirection, -1, 1);
    if (rightDirection == fbxKeyboardRightDirection_ &&
        forwardDirection == fbxKeyboardForwardDirection_ &&
        verticalDirection == fbxKeyboardVerticalDirection_)
    {
        return;
    }

    fbxKeyboardRightDirection_ = rightDirection;
    fbxKeyboardForwardDirection_ = forwardDirection;
    fbxKeyboardVerticalDirection_ = verticalDirection;
    if (!hasFbxKeyboardPanDirection())
    {
        stopFbxKeyboardPanFrame(window);
        return;
    }

    lastFbxKeyboardPanTick_ = std::chrono::steady_clock::now() -
        std::chrono::milliseconds(4);
    lastFbxKeyboardPanFrame_ = std::chrono::steady_clock::time_point{};
    scheduleFbxKeyboardPanFrame(window);
}

void DomDownToolPreview::PreviewController::applyFbxKeyboardPanTick()
{
    if (!hasFbxKeyboardPanDirection())
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::clamp(
        std::chrono::duration<double>(now - lastFbxKeyboardPanTick_).count(),
        0.0, 0.05);
    lastFbxKeyboardPanTick_ = now;
    if (seconds <= 0.0)
    {
        return;
    }

    const double pixels = kFbxKeyboardPanPixelsPerSecond * seconds;
    dx12FbxRenderer_.keyboardPanBy(
        static_cast<double>(fbxKeyboardRightDirection_) * pixels,
        static_cast<double>(fbxKeyboardForwardDirection_) * pixels,
        static_cast<double>(fbxKeyboardVerticalDirection_) * pixels);
}

void DomDownToolPreview::PreviewController::scheduleFbxKeyboardPanFrame(HWND window)
{
    if (!window || !hasFbxKeyboardPanDirection() || immediateFbxRedrawPosted_ ||
        fbxKeyboardPanTimerActive_)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto minimumFrameInterval =
        std::chrono::milliseconds(kPreviewFbxKeyboardPanFrameIntervalMilliseconds);
    if (lastFbxKeyboardPanFrame_ == std::chrono::steady_clock::time_point{} ||
        now - lastFbxKeyboardPanFrame_ >= minimumFrameInterval)
    {
        scheduleImmediateFbxRedraw(window, false);
        return;
    }

    const auto remaining = minimumFrameInterval - (now - lastFbxKeyboardPanFrame_);
    const UINT delayMilliseconds = static_cast<UINT>(std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count()));
    if (SetTimer(window, kPreviewFbxKeyboardPanTimerId, delayMilliseconds, nullptr) != 0)
    {
        fbxKeyboardPanTimerActive_ = true;
        return;
    }

    scheduleImmediateFbxRedraw(window, false);
}

void DomDownToolPreview::PreviewController::stopFbxKeyboardPanFrame(HWND window)
{
    if (window && fbxKeyboardPanTimerActive_)
    {
        KillTimer(window, kPreviewFbxKeyboardPanTimerId);
    }
    fbxKeyboardPanTimerActive_ = false;
}

void DomDownToolPreview::PreviewController::startFbxPreviewFrameLoop(HWND window)
{
    if (!window || fbxFrameTimerActive_)
    {
        return;
    }

    if (SetTimer(window, kPreviewFbxFrameTimerId,
                 kPreviewFbxFrameTimerIntervalMilliseconds, nullptr) != 0)
    {
        fbxFrameTimerActive_ = true;
    }
}

void DomDownToolPreview::PreviewController::stopFbxPreviewFrameLoop(HWND window)
{
    if (window && fbxFrameTimerActive_)
    {
        KillTimer(window, kPreviewFbxFrameTimerId);
    }
    fbxFrameTimerActive_ = false;
}

void DomDownToolPreview::PreviewController::recordFbxPreviewFrame(skui::Runtime& runtime)
{
    updateFbxPreviewFrameStatsUi(runtime);

    const auto now = std::chrono::steady_clock::now();
    if (lastFbxFrameTime_ != std::chrono::steady_clock::time_point{} &&
        now - lastFbxFrameTime_ > kFbxFrameRateIdleResetInterval)
    {
        resetFbxPreviewFrameRate();
    }

    lastFbxFrameTime_ = now;
    if (fbxFrameRateWindowStart_ == std::chrono::steady_clock::time_point{})
    {
        fbxFrameRateWindowStart_ = now;
        fbxFrameRateFrameCount_ = 1;
        runtime.setTextById("fbx-preview-fps", fbxFrameRateText_);
        return;
    }

    ++fbxFrameRateFrameCount_;
    const auto elapsed = now - fbxFrameRateWindowStart_;
    if (elapsed < kFbxFrameRateUpdateInterval)
    {
        return;
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    fbxFrameRateText_ = fmt::format("FPS {:.0f}",
                                    static_cast<double>(fbxFrameRateFrameCount_) / seconds);
    runtime.setTextById("fbx-preview-fps", fbxFrameRateText_);
    fbxFrameRateWindowStart_ = now;
    fbxFrameRateFrameCount_ = 0;
}

void DomDownToolPreview::PreviewController::resetFbxPreviewFrameRate()
{
    lastFbxFrameTime_ = std::chrono::steady_clock::time_point{};
    fbxFrameRateWindowStart_ = std::chrono::steady_clock::time_point{};
    fbxFrameRateFrameCount_ = 0;
    fbxFrameRateText_ = "FPS --";
}

void DomDownToolPreview::PreviewController::disableFbxInputMethod(HWND window)
{
    if (!window)
    {
        return;
    }
    if (fbxInputMethodDisabled_)
    {
        if (fbxInputMethodWindow_ == window)
        {
            return;
        }
        restoreFbxInputMethod();
    }

    // FBX 预览只消费相机快捷键和鼠标输入，不应让中文输入法抢占 Q/E 等按键。
    fbxInputMethodContext_ = ImmAssociateContext(window, nullptr);
    fbxInputMethodWindow_ = window;
    fbxInputMethodDisabled_ = true;
}

void DomDownToolPreview::PreviewController::restoreFbxInputMethod()
{
    if (!fbxInputMethodDisabled_)
    {
        return;
    }
    if (fbxInputMethodWindow_)
    {
        ImmAssociateContext(fbxInputMethodWindow_, fbxInputMethodContext_);
    }
    fbxInputMethodWindow_ = nullptr;
    fbxInputMethodContext_ = nullptr;
    fbxInputMethodDisabled_ = false;
}

void DomDownToolPreview::PreviewController::scheduleMapUiRefresh(HWND window)
{
    if (mapUiRefreshPosted_)
    {
        return;
    }

    mapUiRefreshPosted_ = true;
    if (!PostMessageW(window, kPreviewMapUiRefreshMessage, 0, 0))
    {
        mapUiRefreshPosted_ = false;
    }
}

void DomDownToolPreview::PreviewController::updateMapInteractionUi(
    skui::Runtime& runtime)
{
    const skui::RuntimeUpdateBatch updateBatch(runtime);
    const std::string centerText =
        fmt::format("{:.6f}, {:.6f}", renderer_.center().lon, renderer_.center().lat);
    const std::string zoomText = fmt::format("Z {:.2f}", renderer_.zoom());
    bool statusChanged = false;
    if (centerText != lastCenterCoordinate_)
    {
        runtime.setValueById("center-coordinate", centerText);
        lastCenterCoordinate_ = centerText;
        statusChanged = true;
    }
    if (zoomText != lastZoomValue_)
    {
        runtime.setTextById("zoom-value", zoomText);
        lastZoomValue_ = zoomText;
        statusChanged = true;
    }

    if (statusChanged)
    {
        requestRedraw();
    }
}

void DomDownToolPreview::PreviewController::applyFbxInteractionSettings(
    FbxPreviewInteractionSettings settings)
{
    settings.rotateSpeed = clampFbxInteractionSpeed(settings.rotateSpeed);
    settings.zoomSpeed = clampFbxInteractionSpeed(settings.zoomSpeed);
    settings.panSpeed = clampFbxInteractionSpeed(settings.panSpeed);
    settings.moveSpeed = clampFbxInteractionSpeed(settings.moveSpeed);
    fbxInteractionSettings_ = settings;
    dx12FbxRenderer_.setInteractionSettings(fbxInteractionSettings_);
}

bool DomDownToolPreview::PreviewController::updateFbxInteractionSetting(
    skui::Runtime& runtime, std::string_view id, std::string_view value, bool persist)
{
    const std::optional<double> parsedValue = parseFiniteDouble(value);
    if (!parsedValue.has_value())
    {
        return id == "fbx-rotate-speed" || id == "fbx-zoom-speed" ||
            id == "fbx-pan-speed" || id == "fbx-move-speed";
    }

    FbxPreviewInteractionSettings nextSettings = fbxInteractionSettings_;
    if (id == "fbx-rotate-speed")
    {
        nextSettings.rotateSpeed = clampFbxInteractionSpeed(*parsedValue);
    }
    else if (id == "fbx-zoom-speed")
    {
        nextSettings.zoomSpeed = clampFbxInteractionSpeed(*parsedValue);
    }
    else if (id == "fbx-pan-speed")
    {
        nextSettings.panSpeed = clampFbxInteractionSpeed(*parsedValue);
    }
    else if (id == "fbx-move-speed")
    {
        nextSettings.moveSpeed = clampFbxInteractionSpeed(*parsedValue);
    }
    else
    {
        return false;
    }

    applyFbxInteractionSettings(nextSettings);
    syncFbxInteractionSettingsUi(runtime);
    if (persist)
    {
        static_cast<void>(saveFbxInteractionSettings(fbxInteractionSettings_));
    }
    return true;
}

void DomDownToolPreview::PreviewController::syncFbxInteractionSettingsUi(
    skui::Runtime& runtime)
{
    runtime.setValueById("fbx-rotate-speed",
                         fmt::format("{:.1f}", fbxInteractionSettings_.rotateSpeed));
    runtime.setValueById("fbx-zoom-speed",
                         fmt::format("{:.1f}", fbxInteractionSettings_.zoomSpeed));
    runtime.setValueById("fbx-pan-speed",
                         fmt::format("{:.1f}", fbxInteractionSettings_.panSpeed));
    runtime.setValueById("fbx-move-speed",
                         fmt::format("{:.1f}", fbxInteractionSettings_.moveSpeed));
    runtime.setTextById("fbx-rotate-speed-value",
                        fbxInteractionSpeedText(fbxInteractionSettings_.rotateSpeed));
    runtime.setTextById("fbx-zoom-speed-value",
                        fbxInteractionSpeedText(fbxInteractionSettings_.zoomSpeed));
    runtime.setTextById("fbx-pan-speed-value",
                        fbxInteractionSpeedText(fbxInteractionSettings_.panSpeed));
    runtime.setTextById("fbx-move-speed-value",
                        fbxInteractionSpeedText(fbxInteractionSettings_.moveSpeed));
}

void DomDownToolPreview::PreviewController::scheduleUiRefresh()
{
    const MapRenderStats stats = renderer_.stats();
    if (scheduledStats_.has_value() && equalStats(*scheduledStats_, stats))
    {
        return;
    }

    const HWND window = window_.load(std::memory_order_relaxed);
    if (window && PostMessageW(window, kPreviewUiRefreshMessage, 0, 0))
    {
        scheduledStats_ = stats;
    }
}

void DomDownToolPreview::PreviewController::scheduleSearchRefresh() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window)
    {
        writeSearchTrace("Controller", "search_refresh_post_failed", "reason=no_window");
        return;
    }

    const BOOL posted = PostMessageW(window, kPreviewSearchRefreshMessage, 0, 0);
    writeSearchTrace("Controller", posted ? "search_refresh_posted"
                                           : "search_refresh_post_failed",
                     posted ? "" : fmt::format("win32_error={}", GetLastError()));
}

void DomDownToolPreview::PreviewController::scheduleExportTaskRefresh()
{
    const HWND window = window_.load(std::memory_order_relaxed);
    if (!window || exportTaskRefreshPosted_.exchange(true, std::memory_order_relaxed))
    {
        return;
    }
    if (!PostMessageW(window, kPreviewExportTaskRefreshMessage, 0, 0))
    {
        exportTaskRefreshPosted_.store(false, std::memory_order_relaxed);
    }
}

void DomDownToolPreview::PreviewController::requestRedraw() const
{
    const HWND window = window_.load(std::memory_order_relaxed);
    const bool isFbxPreview = page_ == Page::FbxPreview;
    if (isFbxPreview && fbxRedrawPosted_.exchange(true, std::memory_order_relaxed))
    {
        return;
    }
    if (window && PostMessageW(window, kPreviewRedrawMessage, 0, 0))
    {
        return;
    }
    if (isFbxPreview)
    {
        fbxRedrawPosted_.store(false, std::memory_order_relaxed);
    }
}
