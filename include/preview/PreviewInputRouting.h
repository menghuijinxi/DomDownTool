#pragma once

#include <windows.h>

namespace DomDownToolPreview
{
[[nodiscard]] constexpr bool shouldConsumeSearchPopupPointerMessage(
    UINT message, bool insideSearchPopup)
{
    return insideSearchPopup && message != WM_MOUSEWHEEL;
}
} // namespace DomDownToolPreview
