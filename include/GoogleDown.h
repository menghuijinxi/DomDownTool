#pragma once

#include <string>
#include <vector>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
    class GoogleDown : public GCJ02GoogleDownBase
    {
    public:
        GoogleDown();

    protected:
        std::vector<std::string> GetRequestHeaders(const Tile& tile) const override;
    };
} // namespace DownTool
