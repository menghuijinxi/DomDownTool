#include "DownToolBase.h"

#include <filesystem>
#include <fstream>

#include "fmt/format.h"

namespace
{
	size_t WriteTileData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		DownTool::CurlMultiHandle* context = static_cast<DownTool::CurlMultiHandle*>(userdata);
		std::string filePath = fmt::format("{}/{}/{}/{}.png", context->owner->GetCachePath(), context->tile.Zoom, context->tile.X, context->tile.Y);
		std::filesystem::path nativePath = std::filesystem::u8path(filePath);
		std::filesystem::create_directories(nativePath.parent_path());
		std::ofstream outFile(nativePath, std::ios::binary);
		size_t totalSize = size * nmemb;
		if (outFile.is_open())
		{
			if (totalSize <= 4000)
			{
				fmt::print("下载成功，但文件大小较小，可能是无效数据：{}。url:{}\n", filePath, context->url);
			}
			outFile.write(static_cast<char*>(ptr), totalSize);
			outFile.close();
			fmt::print("成功下载切片：{}\n", filePath);
		}
		else
		{
			fmt::print("无法打开文件进行写入：{}\n", filePath);
		}
		return totalSize;
	}
}

void DownTool::DownToolBase::WaitDown()
{
	// 1. 执行 I/O 操作（关键！）
	int still_running = 1;
	if (curl_multi_perform(curlMulti, &still_running) != CURLM_OK)
	{
		fmt::print("CURL multi perform error\n");
		return;
	}

	// 2. 检查是否有已完成的任务
	CURLMsg* msg;
	int msgs_left;
	while ((msg = curl_multi_info_read(curlMulti, &msgs_left)))
	{
		if (msg->msg == CURLMSG_DONE)
		{
			CURL* easy = msg->easy_handle;
			// 可选：检查结果
			if (msg->data.result != CURLE_OK)
			{
				fmt::print("Request failed: {}\n", curl_easy_strerror(msg->data.result));
			}
			// 清理 easy handle（重要！）
			curl_multi_remove_handle(curlMulti, easy);
			curl_easy_cleanup(easy);
			curlEasyHandles.erase(easy);
		}
	}

	// 3. 等待 socket 事件（避免 busy loop）
	if (still_running)
	{
		CURLMcode  mc = curl_multi_wait(curlMulti, nullptr, 0, 1000, nullptr); // 最多等 1 秒
		if (mc != CURLM_OK)
		{
			fmt::print("curl_multi_wait failed: {}\n", curl_multi_strerror(mc));
		}
	}
}

DownTool::DownToolBase::~DownToolBase()
{
	StopDown();

	if (curlMulti)
	{
		curl_multi_cleanup(curlMulti);
		curlMulti = nullptr;
	}
	if (share)
	{
		curl_share_cleanup(share);
		share = nullptr;
	}
}

void DownTool::DownToolBase::StartDown()
{
	if (bDowning)
	{
		fmt::print("下载中...无法重复进行下载");
		return;
	}

	bDowning = true;

	curlMulti = curl_multi_init();

	// 初始化共享句柄，设置共享DNS缓存
	share = curl_share_init();
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	curl_easy_setopt(curlMulti, CURLOPT_SHARE, share);

	CalculateDownRange();
	fmt::print("下载范围：\n左上角经纬度：({}, {})\n右下角经纬度：({}, {})\n左上角瓦片坐标：({}, {})\n右下角瓦片坐标：({}, {})\n缓存路径：{}\n总下载切片数量预估：{}\n",
		MinLonLat.lon, MaxLonLat.lat, MaxLonLat.lon, MinLonLat.lat,
		MinTile.X, MinTile.Y, MaxTile.X, MaxTile.Y, CachePath, (MaxTile.X - MinTile.X + 1) * (MaxTile.Y - MinTile.Y + 1));

	for (uint32_t x = MinTile.X; x <= MaxTile.X; x++)
	{
		if (bStop) break;
		for (uint32_t y = MinTile.Y; y <= MaxTile.Y; y++)
		{
			if (bStop) break;

			// 等待至少一个下载完成
			while (curlEasyHandles.size() >= MaxDownQueue)
			{
				WaitDown();
			}

			int still_running = 1;
			CURLMcode mc = curl_multi_perform(curlMulti, &still_running);

			CurlMultiHandle multiHandle;
			multiHandle.owner = this;
			multiHandle.tile = Tile(x, y, GetZoom());
			CURL* easy = curl_easy_init();
			if (!easy)
			{
				fmt::print("无法初始化 CURL easy handle\n");
				return;
			}
			std::string url = MakeUri(multiHandle.tile);
			multiHandle.url = url;
			curlEasyHandles[easy] = multiHandle;
			curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
			curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteTileData);
			curl_easy_setopt(easy, CURLOPT_WRITEDATA, &curlEasyHandles[easy]);
			if (curl_multi_add_handle(curlMulti, easy) != CURLM_OK)
			{
				fmt::print("无法将 CURL easy handle 添加到 multi handle\n");
				return;
			}
		}
	}

	// 等待所有下载完成
	while (curlEasyHandles.size() > 0)
	{
		WaitDown();
	}

	bDowning = false;
}

std::string DownTool::DownToolBase::MakeUri(const Tile& tile)
{
	auto x = fmt::arg("x", tile.X);
	auto y = fmt::arg("y", tile.Y);
	auto z = fmt::arg("z", static_cast<uint32_t>(tile.Zoom));
	auto s = fmt::arg("s", ServerRange.GetNextServerIndex());
	fmt::format_args args = fmt::make_format_args(x, y, z, s);
	return fmt::vformat(UrlTemplate, args);
}

void DownTool::DownToolBase::CalculateDownRange()
{
	GeographicLib::Math::real dummy;
	auto& geo = GeographicLib::Geodesic::WGS84();
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 0., GetRadius(), MaxLonLat.lat, dummy);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 180., GetRadius(), MinLonLat.lat, dummy);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 90., GetRadius(), dummy, MaxLonLat.lon);
	geo.Direct(GetCenterLonLat().lat, GetCenterLonLat().lon, 270., GetRadius(), dummy, MinLonLat.lon);

	// 处理经度越界（GeographicLib 通常返回 [-180, 180]）
	if (MinLonLat.lon > MaxLonLat.lon) {
		// 跨越了 ±180° 经线，此时 bounding box 包围极点或环绕地球
		// 简单处理：设为全球（根据需求调整）
		MaxLonLat.lon = 180.0;
		MinLonLat.lon = -180.0;
	}
}
