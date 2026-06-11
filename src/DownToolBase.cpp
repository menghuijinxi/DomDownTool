#include "DownToolBase.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "fmt/format.h"

namespace
{
	enum class ImageType { Unknown, Png, Jpeg };

	ImageType DetectImageType(const unsigned char* data, size_t len)
	{
		if (len >= 8 &&
			data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
			data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
			return ImageType::Png;

		if (len >= 3 &&
			data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
			return ImageType::Jpeg;

		return ImageType::Unknown;
	}

	size_t WriteTileData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		DownTool::CurlMultiHandle* context = static_cast<DownTool::CurlMultiHandle*>(userdata);
		size_t totalSize = size * nmemb;
		if (context->fileStream.is_open())
		{
			context->fileStream.write(static_cast<char*>(ptr), totalSize);
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
	bool hasProgressUpdate = false;
	while ((msg = curl_multi_info_read(curlMulti, &msgs_left)))
	{
		if (bStop) break;

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
			DowningCount += 1;
			hasProgressUpdate = true;
		}
	}

	if (hasProgressUpdate)
	{
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
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

	TotalDownCount += (MaxTile.X - MinTile.X + 1) * (MaxTile.Y - MinTile.Y + 1);

	for (uint32_t x_base = MinTile.X; x_base <= MaxTile.X; x_base++)
	{
		if (bStop) break;
		for (uint32_t y_base = MinTile.Y; y_base <= MaxTile.Y; y_base++)
		{
			if (bStop) break;

			if (IsTileOffset())
			{
				TileRange offsetTiles = GetOffsetTiles(Tile(x_base, y_base, GetZoom()));
				TotalDownCount += (offsetTiles.MaxTile.X - offsetTiles.MinTile.X + 1) * (offsetTiles.MaxTile.Y - offsetTiles.MinTile.Y + 1) - 1; // 减去原本的1个瓦片，因为下面会下载这个瓦片
				for (uint32_t x = offsetTiles.MinTile.X; x <= offsetTiles.MaxTile.X; x++)
				{
					for (uint32_t y = offsetTiles.MinTile.Y; y <= offsetTiles.MaxTile.Y; y++)
					{
						DownloadTile(x, y);
					}
				}
			}
			else
			{
				DownloadTile(x_base, y_base);
			}
		}
	}

	// 等待所有下载完成
	while (curlEasyHandles.size() > 0)
	{
		if (bStop) break;
		WaitDown();
	}
	fmt::print("\n");

	/*for (auto& d : downTileCount)
	{
		fmt::print("URL前缀：{}，下载次数：{}\n", d.first, d.second);
	}*/

	bDowning = false;
}

void DownTool::DownToolBase::DownloadTile(uint32_t x, uint32_t y)
{
	// 等待至少一个下载完成
	while (curlEasyHandles.size() >= MaxDownQueue)
	{
		WaitDown();
	}

	std::string filePath = fmt::format("{}/{}/{}/{}.png", GetCachePath(), GetZoom(), x, y);
	std::filesystem::path nativePath = std::filesystem::u8path(filePath);
	std::filesystem::create_directories(nativePath.parent_path());

	//如果文件存在，检测是不是一个有效的图片文件，如果是的话就跳过下载，如果不是的话就删除这个文件重新下载
	if (std::filesystem::exists(nativePath))
	{
		std::ifstream fileStream(nativePath, std::ios::binary);
		if (fileStream.is_open())
		{
			unsigned char header[8];
			fileStream.read(reinterpret_cast<char*>(header), sizeof(header));
			ImageType type = DetectImageType(header, fileStream.gcount());
			if (type != ImageType::Unknown)
			{
				DowningCount++;
				fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
				return;
			}
		}
	}

	int still_running = 1;
	CURLMcode mc = curl_multi_perform(curlMulti, &still_running);

	CurlMultiHandle multiHandle;
	multiHandle.fileStream.open(nativePath, std::ios::binary);
	if (!multiHandle.fileStream.is_open())
	{
		fmt::print("无法打开文件进行写入，切片将会被跳过，解决文件占用问题后再重新下载一遍即可修复：{}\n", filePath);
		return;;
	}
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
	//downTileCount[url.substr(0, 20)]++;
	curlEasyHandles[easy] = std::move(multiHandle);
	//避免高速下载被服务器拒绝，设置最大连接数为6（根据服务器限制调整）
	//curl_easy_setopt(easy, CURLOPT_MAXCONNECTS, 2L);
	curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteTileData);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &curlEasyHandles[easy]);
	if (curl_multi_add_handle(curlMulti, easy) != CURLM_OK)
	{
		fmt::print("无法将 CURL easy handle 添加到 multi handle\n");
	}
}

std::string DownTool::DownToolBase::MakeUri(const Tile& tile)
{
	const uint32_t xValue = tile.X;
	const uint32_t yValue = tile.Y;
	const uint32_t zValue = tile.Zoom;
	const int32_t sValue = ServerRange.GetNextServerIndex();
	auto x = fmt::arg("x", xValue);
	auto y = fmt::arg("y", yValue);
	auto z = fmt::arg("z", zValue);
	auto s = fmt::arg("s", sValue);
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
