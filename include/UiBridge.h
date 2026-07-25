#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace DownTool
{
    class DownToolBase;
}

namespace DomDownToolUi
{
    class UiBridge
    {
    public:
        UiBridge();

        std::string GetMapSourcesJson() const;

        std::string GetQueueSnapshotJson() const;

        std::string AddDownloadJobJson(const std::string& requestJson);

        std::string EstimatePreviewJson(const std::string& requestJson) const;

    private:
        struct MapSource
        {
            std::string id;
            std::string name;
            std::string group;
            std::string cacheRoot;
            std::string coordinate;
            std::string offsetStrategy;
            std::string backend;
            std::string previewUrlTemplate;
        };

        struct DownloadJob
        {
            uint32_t id = 0;
            std::string project;
            std::string providerId;
            double longitude = 0.;
            double latitude = 0.;
            double radius = 0.;
            uint32_t zoom = 0;
            uint32_t tileSize = 256;
            std::string outputMode;
            uint32_t mergedImageResolution = 16384;
            uint32_t fbxTextureSize = 4096;
            std::string status;
            uint32_t progress = 0;
        };

        const MapSource* FindMapSource(const std::string& providerId) const;

        std::vector<MapSource> mapSources_;
        std::vector<DownloadJob> jobs_;
        uint32_t nextJobId_ = 1;
    };
} // namespace DomDownToolUi
