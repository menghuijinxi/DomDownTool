#pragma once
#include <string>
#include <thread>
#include <GeographicLib/Geodesic.hpp>
#include "DataType.h"
#include "curl/curl.h"

namespace DownTool
{
	const std::string xReplace = "{x}";
	const std::string yReplace = "{y}";
	const std::string zReplace = "{z}";
	const std::string serverIndexReplace = "{s}";

	class DownToolBase
	{
	protected:

		//以这里为中心点开始下载
		LonLat CenterLonLat;

		//设置负载服务器索引范围
		GetDownServerIndex ServerRange;

		//下载半径，单位米
		double Radius = 0.;

		//下载URL模板
		std::string UrlTemplate = "";

		//计算得到的左上角和右下角经纬度
		LonLat MinLonLat, MaxLonLat;
		//计算得到的左上角和右下角瓦片坐标
		Tile MinTile, MaxTile;

		//最大下载线程数，默认使用硬件并发数
		uint32_t MaxDownQueue = std::thread::hardware_concurrency();

		CURLM* curlMulti = nullptr;

		//是否正在下载
		bool bDowning = false;

	public:

		virtual ~DownToolBase();

		//开始下载，做一些准备工作
		virtual void StartDown();

		//根据瓦片坐标生成下载URL
		virtual std::string MakeUri(const Tile& tile);

		//计算下载范围
		virtual void CalculateDownRange();

		GEN_GETSET_FOR(CenterLonLat, Radius, UrlTemplate)
	};
}
