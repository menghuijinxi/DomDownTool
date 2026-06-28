#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TianDiTuDown.h"

namespace DownTool
{
	class TianDiTuMultidateDown : public TianDiTuDown
	{
	public:

		struct TimelineInfo
		{
			std::string time;
			std::string year;
			std::string key;
			std::string projection;
			uint32_t minZoom = 0;
			uint32_t maxZoom = 0;
		};

		TianDiTuMultidateDown();

		virtual void StartDown() override;

		virtual bool IsTileOffset() const override { return true; }

		virtual TileRange GetOffsetTiles(const Tile& tile) const override;

		virtual bool TryGetSourceTilePixelForLonLat(
			const LonLat& lonLat,
			uint32_t zoom,
			Tile& tile,
			double& pixelX,
			double& pixelY) const override;

		// 指定要下载的年份；空字符串表示使用最新年份。
		void SetYear(const std::string& year);

		const std::string& GetYear() const { return SelectedYear; }

		void UseLatestYear();

		void DownloadYear(const std::string& year);

		std::vector<TimelineInfo> ResolveAvailableTimelines();

		std::vector<std::string> ResolveAvailableYears();

		const std::string& GetResolvedYear() const { return ResolvedYear; }

		void SetMultidateRegion(const std::string& gbCode, const std::string& name);

		void SetSessionCookie(const std::string& cookie);

		void SetYearKey(const std::string& year, const std::string& key);

	protected:

		virtual std::vector<std::string> GetRequestHeaders(
			const Tile& tile) const override;

		virtual uint32_t GetMinFallbackZoom() const override { return 3; }

	private:

		void SetLatestWmtsUrlTemplate();

		void SetTimelineTileUrlTemplate();

		bool PrepareMultidateLayer();

		bool EnsureRegion();

		bool FetchTimelineRecords(std::vector<TimelineInfo>& records);

		bool SelectTimelineRecord(
			const std::vector<TimelineInfo>& records,
			TimelineInfo& record) const;

		bool ParseTimelineRecords(
			const std::string& responseBody,
			std::vector<TimelineInfo>& records) const;

		bool TryResolveRegionFromCenter();

		std::string BuildTimelineListUrl() const;

		std::string BuildChangeCityUrl() const;

		std::string GetLayerCacheName() const;

		bool TryGetGeographicTilePixelForLonLat(
			const LonLat& lonLat,
			uint32_t zoom,
			Tile& tile,
			double& pixelX,
			double& pixelY) const;

		std::string SelectedYear;
		std::string ResolvedYear;
		std::string ResolvedKey;
		std::string RegionGbCode;
		std::string RegionName;
		std::string SessionCookie;
		bool bUseLatestYear = true;
		bool bHasPresetYearKey = false;
	};
}
