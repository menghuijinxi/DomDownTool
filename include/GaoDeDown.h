#pragma once

#include <map>
#include <string>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
    enum class GaoDeMapType
    {
        Satellite,
        Electronic,
        RoadNetworkOverlay
    };

    class GaoDeDown : public GCJ02GoogleDownBase
    {
    public:
        explicit GaoDeDown(GaoDeMapType mapType = GaoDeMapType::Satellite);

        void SetMapType(GaoDeMapType mapType);

        GaoDeMapType GetMapType() const
        {
            return mapType_;
        }

    protected:
        GaoDeMapType mapType_ = GaoDeMapType::Satellite;

        bool IsUnavailableTileResponseHeader(
            const std::map<std::string, std::string>& responseHeaders,
            const Tile& tile) const override;
    };
} // namespace DownTool
