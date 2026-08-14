#include "preview/GuiStandardStreams.h"

#include <cstdio>

bool DomDownToolPreview::initializeGuiStandardStreams()
{
    FILE* redirectedStream = nullptr;
    if (freopen_s(&redirectedStream, "NUL", "w", stdout) != 0)
    {
        return false;
    }
    redirectedStream = nullptr;
    return freopen_s(&redirectedStream, "NUL", "w", stderr) == 0;
}
