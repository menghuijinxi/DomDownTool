#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "GCJ02GoogleDownBase.h"

namespace DownTool
{
    enum class TencentMapType
    {
        Satellite,
        Electronic
    };

    class TencentDown : public GCJ02GoogleDownBase
    {
    public:
        explicit TencentDown(TencentMapType mapType = TencentMapType::Satellite);

        void SetMapType(TencentMapType mapType);

        TencentMapType GetMapType() const
        {
            return mapType_;
        }

        virtual std::string MakeUri(const Tile& tile) override;

    protected:
        TencentMapType mapType_ = TencentMapType::Satellite;

        virtual bool IsUnavailableTileFile(
            const std::filesystem::path& tilePath, const Tile& tile) const override;

    private:
        static uint64_t ToTencentY(const Tile& tile);
    };
}
