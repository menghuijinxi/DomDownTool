#pragma once

#include <windows.h>

namespace DomDownToolPreview
{
class GitHubUpdate
{
  public:
    GitHubUpdate() = default;
    ~GitHubUpdate();
    GitHubUpdate(const GitHubUpdate&) = delete;
    GitHubUpdate& operator=(const GitHubUpdate&) = delete;

    void Open(HWND owner);

  private:
    HANDLE process_ = nullptr;
};
}
