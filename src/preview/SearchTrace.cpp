#include "preview/SearchTrace.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
std::mutex gTraceMutex;
std::ofstream gTraceFile;

std::string currentTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &time);
#else
    localtime_r(&time, &localTime);
#endif

    std::ostringstream timestamp;
    timestamp << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S") << '.'
              << std::setfill('0') << std::setw(3) << milliseconds.count();
    return timestamp.str();
}
} // namespace

void DomDownToolPreview::initializeSearchTrace(const std::filesystem::path& logPath)
{
    std::error_code directoryError;
    std::filesystem::create_directories(logPath.parent_path(), directoryError);

    const std::lock_guard lock(gTraceMutex);
    gTraceFile.close();
    if (directoryError)
    {
        return;
    }
    gTraceFile.open(logPath, std::ios::binary | std::ios::trunc);
    if (gTraceFile)
    {
        gTraceFile << currentTimestamp() << " [thread=" << std::this_thread::get_id()
                   << "] [App] trace_initialized path=" << logPath.string() << '\n';
        gTraceFile.flush();
    }
}

void DomDownToolPreview::writeSearchTrace(std::string_view component, std::string_view event,
                                          std::string_view detail)
{
    const std::lock_guard lock(gTraceMutex);
    if (!gTraceFile)
    {
        return;
    }

    gTraceFile << currentTimestamp() << " [thread=" << std::this_thread::get_id() << "] ["
               << component << "] " << event;
    if (!detail.empty())
    {
        gTraceFile << ' ' << detail;
    }
    gTraceFile << '\n';
    gTraceFile.flush();
}
