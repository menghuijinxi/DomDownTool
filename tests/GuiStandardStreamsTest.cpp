#include <cstdlib>
#include <exception>

#include <windows.h>

#include "fmt/format.h"

#include "preview/GuiStandardStreams.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    if (!DomDownToolPreview::initializeGuiStandardStreams())
    {
        return EXIT_FAILURE;
    }

    try
    {
        fmt::print("GUI export progress: {}/{}\n", 1, 1);
        fmt::print(stderr, "GUI export diagnostic\n");
    }
    catch (const std::exception&)
    {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
