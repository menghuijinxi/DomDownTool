#pragma once

#include <filesystem>
#include <string_view>

namespace DomDownToolPreview
{
void initializeSearchTrace(const std::filesystem::path& logPath);

void writeSearchTrace(std::string_view component, std::string_view event,
                      std::string_view detail = {});
} // namespace DomDownToolPreview
