#pragma once

#include <string>
#include <string_view>

#include "preview/ExportTaskManager.h"

namespace DomDownToolPreview
{
std::string ExportDistanceLodHtml(const ExportComponent& component);
bool UpdateExportDistanceLod(
    ExportComponent& component, std::string_view verb,
    std::string_view rowIndex, std::string_view value);
}
