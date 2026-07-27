#pragma once

#include <string>

#include "GoogleDownBase.h"

namespace DownTool
{
    class BingDown : public GoogleDownBase
    {
    public:
        BingDown();

        std::string MakeUri(const Tile& tile) override;

        static std::string TileToQuadKey(const Tile& tile);
    };
} // namespace DownTool
