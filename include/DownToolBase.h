#pragma once
#include <fstream>
#include <map>
#include <string>
#include <thread>
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

	struct CurlMultiHandle
	{
		DownToolBase* owner = nullptr;
		Tile tile;
		std::string url;
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

		//停止下载的标志，设置为true后，正在下载的线程会尽快停止下载
		std::atomic_bool bStop = false;

		//确定完成下载的数量
		std::int32_t DowningCount = 0;

		void WaitDown();

		//std::map<std::string, int32_t> downTileCount;

	public:

		virtual ~DownToolBase();

		//开始下载，做一些准备工作
		virtual void StartDown();

		//根据瓦片坐标生成下载URL
		virtual std::string MakeUri(const Tile& tile);

		//计算下载范围
		virtual void CalculateDownRange();

        GEN_GETSET_FOR(CenterLonLat, Radius, UrlTemplate, Zoom, MaxLonLat, MinLonLat, CachePath, MaxDownQueue, MinTile, MaxTile, DowningCount)
		
			//停止下载，正在下载的线程会尽快停止下载
		void StopDown()
		{
			bStop = true;
		}
	};
}
