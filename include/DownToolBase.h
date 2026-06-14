#pragma once
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <fmt/base.h>
#include <GeographicLib/Geodesic.hpp>
#include "DataType.h"
#include "curl/curl.h"

namespace DownTool
{
	const std::string xReplace = "{x}";
	const std::string yReplace = "{y}";
	const std::string zReplace = "{z}";
	const std::string serverIndexReplace = "{s}";

	class DownToolBase;

	enum class CurlRequestType
	{
		Tile,
		RepairParent
	};

	enum class TileCacheState
	{
		Ready,
		Missing,
		Unavailable,
		Invalid
	};

	struct CurlMultiHandle
	{
		DownToolBase* owner = nullptr;
		CurlRequestType requestType = CurlRequestType::Tile;
		Tile tile;
		uint32_t repairTargetZoom = 0;
		std::string url;
		std::filesystem::path filePath;
		std::ofstream fileStream;
	};

	class DownToolBase
	{
	protected:

		//以这里为中心点开始下载
		LonLat CenterLonLat;

		//设置负载服务器索引范围
		GetDownServerIndex ServerRange;

		//下载半径，单位米
		double Radius = 0.;

		//下载级别，如果服务器没有提供某个级别的瓦片，下载工具会自动降级下载更低级别的瓦片然后合并成更高等级的瓦片
		uint32_t Zoom = 18;

		//下载URL模板
		std::string UrlTemplate = "";

		//计算得到的左上角和右下角经纬度
		LonLat MinLonLat, MaxLonLat;
		//计算得到的左上角和右下角瓦片坐标
		Tile MinTile, MaxTile;

		//最大下载线程数，默认使用硬件并发数
		uint32_t MaxDownQueue = std::thread::hardware_concurrency();

		//多线程下载时使用的CURLM句柄
		CURLM* curlMulti = nullptr;

		CURLSH* share = nullptr;

		//所有下载句柄
		std::map<CURL*, CurlMultiHandle> curlEasyHandles;

		//是否正在下载
		bool bDowning = false;

		//临时文件的缓存路径
		std::string CachePath = "cache";

		//下载完成后的瓦片后处理输出目录，空字符串时默认输出到 CachePath/Google_墨卡托
		std::string PostProcessOutputPath = "";

		//是否在下载完成后自动执行瓦片后处理
		bool PostProcessEnabled = false;

		//瓦片像素大小
		uint32_t TileSize = 256;

		//停止下载的标志，设置为true后，正在下载的线程会尽快停止下载
		std::atomic_bool bStop = false;

		//确定完成下载的数量
		std::int32_t DowningCount = 0;

		std::uint32_t TotalDownCount = 0;

		void WaitDown();

		void BuildTargetTiles(std::vector<Tile>& targetTiles);

		void BuildRequestedTiles(std::vector<Tile>& targetTiles) const;

		std::filesystem::path GetTileFilePath(const Tile& tile) const;

		std::filesystem::path GetPostProcessRootPath() const;

		std::filesystem::path GetPostProcessTileFilePath(const Tile& tile) const;

		TileCacheState GetTileCacheState(const Tile& tile) const;

		std::vector<Tile> ScanCacheAndCollectPendingTiles(
			const std::vector<Tile>& targetTiles,
			const std::string& phaseName,
			bool updateProgress);

		bool QueueTileDownload(
			const Tile& tile,
			const std::filesystem::path& tilePath,
			CurlRequestType requestType,
			uint32_t repairTargetZoom);

		bool RepairTileFromLowerZoom(const Tile& tile, const std::filesystem::path& tilePath);

		bool RepairTileFromLowerZoom(
			const Tile& tile,
			const std::filesystem::path& tilePath,
			uint32_t startParentZoom);

		bool RepairTilesFromParent(const Tile& parentTile, uint32_t targetZoom);

		bool IsTileDownloading(const Tile& tile) const;

		uint32_t CancelPendingChildDownloads(const Tile& parentTile, uint32_t childZoom);

		void RunPostProcess();

		bool GeneratePostProcessMaxZoomTiles(const std::vector<Tile>& targetTiles);

		void GeneratePostProcessLowerZoomTiles(std::vector<Tile> currentLevelTiles);

		virtual bool WritePostProcessMaxZoomTile(
			const Tile& targetTile, const std::filesystem::path& outputPath) const;

		bool WritePostProcessMetadata() const;

		virtual bool CanCopyPostProcessTileDirectly() const { return !IsTileOffset(); }

		virtual LonLat ConvertTargetLonLatToSourceLonLat(const LonLat& lonLat) const
		{
			return lonLat;
		}

		virtual std::filesystem::path GetPostProcessSourceTileFilePath(
			const Tile& tile) const
		{
			return GetTileFilePath(tile);
		}

		virtual bool TryGetLonLatForTilePixel(
			const Tile& tile, double pixelX, double pixelY, LonLat& lonLat) const
		{
			(void)tile;
			(void)pixelX;
			(void)pixelY;
			(void)lonLat;
			return false;
		}

		virtual bool TryGetTilePixelForLonLat(
			const LonLat& lonLat,
			uint32_t zoom,
			Tile& tile,
			double& pixelX,
			double& pixelY) const
		{
			(void)lonLat;
			(void)zoom;
			(void)tile;
			(void)pixelX;
			(void)pixelY;
			return false;
		}

		virtual bool IsUnavailableTileFile(
			const std::filesystem::path& tilePath, const Tile& tile) const { return false; }

		virtual uint32_t GetMinFallbackZoom() const { return 0; }

		//std::map<std::string, int32_t> downTileCount;

	public:

		virtual ~DownToolBase();

		//开始下载，做一些准备工作
		virtual void StartDown();

		void DownloadTile(uint32_t x, uint32_t y);

		//对于经过了坐标偏移的瓦片来说，我们就可能会需要下载很多个瓦片了，比如火星偏移过的瓦片一般一个瓦片需要下载4个，然后再进行合并操作
		virtual bool IsTileOffset() const { return false; }

		//获取经过坐标偏移后的瓦片坐标，返回值是一个瓦片坐标的元组，分别代表了偏移后需要下载的瓦片坐标范围的左上角和右下角瓦片坐标
		virtual TileRange GetOffsetTiles(const Tile& tile) const { return { tile, tile }; }

		//根据瓦片坐标生成下载URL
		virtual std::string MakeUri(const Tile& tile);

		//计算下载范围
		virtual void CalculateDownRange();

        GEN_GETSET_FOR(CenterLonLat, Radius, UrlTemplate, Zoom, MaxLonLat, MinLonLat, CachePath, MaxDownQueue, MinTile, MaxTile, DowningCount, PostProcessOutputPath, PostProcessEnabled, TileSize)
		
			//停止下载，正在下载的线程会尽快停止下载
		void StopDown()
		{
			bStop = true;
		}
	};
}
