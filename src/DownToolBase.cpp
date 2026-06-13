#include "DownToolBase.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "fmt/format.h"
#include "opencv2/core/utility.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

namespace
{
	enum class ImageType { Unknown, Png, Jpeg };

	enum class TileRepairWriteResult { Failed, Existing, Written };

	constexpr uint64_t MaxBatchRepairedTileCount = 4096;
	constexpr size_t CacheScanBatchSize = 128;
	constexpr size_t MaxCacheIssueSampleCount = 20;
	constexpr uint32_t CacheScanConcurrencyMultiplier = 4;

	struct TileRepairTask
	{
		DownTool::Tile childTile;
		uint32_t childX = 0;
		uint32_t childY = 0;
	};

	bool IsSameTile(const DownTool::Tile& left, const DownTool::Tile& right)
	{
		return left.X == right.X && left.Y == right.Y && left.Zoom == right.Zoom;
	}

	uint32_t GetEffectiveQueueCount(uint32_t configuredCount)
	{
		const uint32_t opencvThreadCount =
			static_cast<uint32_t>(std::max(1, cv::getNumThreads()));
		if (configuredCount == 0)
		{
			return opencvThreadCount;
		}

		return std::max(1u, configuredCount);
	}

	int GetTileRepairStripeCount(uint64_t tileCount, uint32_t configuredQueueCount)
	{
		const uint32_t opencvThreadCount =
			static_cast<uint32_t>(std::max(1, cv::getNumThreads()));
		const uint32_t queueCount = GetEffectiveQueueCount(configuredQueueCount);
		const uint32_t cappedTileCount =
			static_cast<uint32_t>(std::min<uint64_t>(tileCount, opencvThreadCount));
		const uint32_t stripeCount = std::min(queueCount, cappedTileCount);
		return static_cast<int>(std::max(1u, stripeCount));
	}

	uint32_t GetHardwareThreadCount()
	{
		return std::max(1u, std::thread::hardware_concurrency());
	}

	uint32_t GetCacheScanWorkerCount(uint64_t tileCount)
	{
		if (tileCount == 0)
		{
			return 0;
		}

		const uint64_t workerLimit =
			static_cast<uint64_t>(GetHardwareThreadCount()) * CacheScanConcurrencyMultiplier;
		const uint64_t workerCount = std::min<uint64_t>(tileCount, workerLimit);
		return static_cast<uint32_t>(std::max<uint64_t>(1, workerCount));
	}

	const char* GetTileCacheStateText(DownTool::TileCacheState state)
	{
		switch (state)
		{
		case DownTool::TileCacheState::Ready:
			return "可用";
		case DownTool::TileCacheState::Missing:
			return "缺失";
		case DownTool::TileCacheState::Unavailable:
			return "占位图";
		case DownTool::TileCacheState::Invalid:
			return "无效文件";
		default:
			return "未知";
		}
	}

	DownTool::Tile GetFirstChildTile(
		const DownTool::Tile& parentTile, uint32_t targetZoom)
	{
		if (targetZoom <= parentTile.Zoom)
		{
			return parentTile;
		}

		const uint32_t zoomOffset = targetZoom - parentTile.Zoom;
		if (zoomOffset >= 31)
		{
			return parentTile;
		}

		const uint64_t scale = uint64_t{ 1 } << zoomOffset;
		return DownTool::Tile(
			static_cast<uint32_t>(static_cast<uint64_t>(parentTile.X) * scale),
			static_cast<uint32_t>(static_cast<uint64_t>(parentTile.Y) * scale),
			targetZoom);
	}

	size_t HashCombine(size_t seed, size_t value) noexcept
	{
		return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
	}

	struct TileHash
	{
		size_t operator()(const DownTool::Tile& tile) const noexcept
		{
			size_t seed = std::hash<uint32_t>{}(tile.Zoom);
			seed = HashCombine(seed, std::hash<uint32_t>{}(tile.X));
			seed = HashCombine(seed, std::hash<uint32_t>{}(tile.Y));
			return seed;
		}
	};

	struct TileEqual
	{
		bool operator()(const DownTool::Tile& left, const DownTool::Tile& right) const noexcept
		{
			return IsSameTile(left, right);
		}
	};

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

	bool IsValidImageFile(const std::filesystem::path& path)
	{
		std::ifstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		unsigned char header[8];
		fileStream.read(reinterpret_cast<char*>(header), sizeof(header));
		return DetectImageType(header, fileStream.gcount()) != ImageType::Unknown;
	}

	cv::Mat ReadImage(const std::filesystem::path& path)
	{
		std::ifstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return {};
		}

		std::vector<unsigned char> data(
			(std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
		if (data.empty())
		{
			return {};
		}

		try
		{
			return cv::imdecode(data, cv::IMREAD_COLOR);
		}
		catch (const cv::Exception&)
		{
			return {};
		}
	}

	bool WriteImage(const std::filesystem::path& path, const cv::Mat& image)
	{
		if (image.empty())
		{
			return false;
		}

		std::error_code createDirectoryError;
		std::filesystem::create_directories(path.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}
		std::string extension = path.extension().string();
		if (extension.empty())
		{
			extension = ".png";
		}

		std::vector<unsigned char> data;
		if (!cv::imencode(extension, image, data))
		{
			return false;
		}

		std::ofstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		fileStream.write(reinterpret_cast<const char*>(data.data()), data.size());
		const bool writeSuccess = fileStream.good();
		fileStream.close();
		return writeSuccess && !fileStream.fail();
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
			auto handleIt = curlEasyHandles.find(easy);
			Tile tile;
			std::filesystem::path tilePath;
			std::string url;
			CurlRequestType requestType = CurlRequestType::Tile;
			uint32_t repairTargetZoom = 0;
			bool hasContext = false;
			if (handleIt != curlEasyHandles.end())
			{
				tile = handleIt->second.tile;
				tilePath = handleIt->second.filePath;
				url = handleIt->second.url;
				requestType = handleIt->second.requestType;
				repairTargetZoom = handleIt->second.repairTargetZoom;
				handleIt->second.fileStream.close();
				hasContext = true;
			}

			// 可选：检查结果
			if (msg->data.result != CURLE_OK)
			{
				if (hasContext)
				{
					fmt::print("\nRequest failed: {}, url: {}\n",
						curl_easy_strerror(msg->data.result), url);
				}
				else
				{
					fmt::print("\nRequest failed: {}\n", curl_easy_strerror(msg->data.result));
				}
			}
			// 清理 easy handle（重要！）
			curl_multi_remove_handle(curlMulti, easy);
			curl_easy_cleanup(easy);
			if (handleIt != curlEasyHandles.end())
			{
				curlEasyHandles.erase(handleIt);
			}
			if (!hasContext)
			{
				continue;
			}

			if (requestType == CurlRequestType::Tile)
			{
				if (msg->data.result == CURLE_OK && IsUnavailableTileFile(tilePath, tile))
				{
					if (!RepairTileFromLowerZoom(tile, tilePath))
					{
						fmt::print("\n无法从低级别瓦片修复占位图: {}\n", tilePath.string());
					}
				}

				DowningCount += 1;
				hasProgressUpdate = true;
				continue;
			}

			if (msg->data.result == CURLE_OK &&
				IsValidImageFile(tilePath) &&
				!IsUnavailableTileFile(tilePath, tile))
			{
				if (RepairTilesFromParent(tile, repairTargetZoom))
				{
					continue;
				}

				fmt::print("\n无法完整批量生成高级别修复瓦片: {}\n", tilePath.string());
			}

			if (tile.Zoom > GetMinFallbackZoom() && repairTargetZoom > tile.Zoom)
			{
				const Tile retryTile = GetFirstChildTile(tile, repairTargetZoom);
				if (!RepairTileFromLowerZoom(
					retryTile, GetTileFilePath(retryTile), tile.Zoom - 1))
				{
					fmt::print("\n无法继续降级修复占位图: {}\n", tilePath.string());
				}
			}
		}
	}

	if (hasProgressUpdate)
	{
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	// 3. 等待 socket 事件（避免 busy loop）
	if (!curlEasyHandles.empty())
	{
		CURLMcode  mc = curl_multi_wait(curlMulti, nullptr, 0, 100, nullptr);
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
	bStop = false;
	DowningCount = 0;
	TotalDownCount = 0;

	curlMulti = curl_multi_init();

	// 初始化共享句柄，设置共享DNS缓存
	share = curl_share_init();
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

	CalculateDownRange();
	std::vector<Tile> targetTiles;
	BuildTargetTiles(targetTiles);
	TotalDownCount = static_cast<uint32_t>(targetTiles.size());

	fmt::print("下载范围：\n左上角经纬度：({}, {})\n右下角经纬度：({}, {})\n左上角瓦片坐标：({}, {})\n右下角瓦片坐标：({}, {})\n缓存路径：{}\n总下载切片数量预估：{}\n",
		MinLonLat.lon, MaxLonLat.lat, MaxLonLat.lon, MinLonLat.lat,
		MinTile.X, MinTile.Y, MaxTile.X, MaxTile.Y, CachePath, TotalDownCount);

	std::vector<Tile> pendingTiles =
		ScanCacheAndCollectPendingTiles(targetTiles, "下载前缓存检测", true);

	for (const Tile& tile : pendingTiles)
	{
		if (bStop) break;
		DownloadTile(tile.X, tile.Y);
	}

	// 等待所有下载完成
	while (curlEasyHandles.size() > 0)
	{
		if (bStop) break;
		WaitDown();
	}
	fmt::print("\n");

	std::vector<Tile> failedTiles =
		ScanCacheAndCollectPendingTiles(targetTiles, "下载后缓存检测", false);
	if (!failedTiles.empty())
	{
		fmt::print("下载结束后仍有 {} 个瓦片不可用，请查看上面的缓存检测明细。\n",
			failedTiles.size());
	}

	/*for (auto& d : downTileCount)
	{
		fmt::print("URL前缀：{}，下载次数：{}\n", d.first, d.second);
	}*/

	bDowning = false;
}

void DownTool::DownToolBase::BuildTargetTiles(std::vector<Tile>& targetTiles)
{
	targetTiles.clear();
	const uint64_t xCount = static_cast<uint64_t>(MaxTile.X) - MinTile.X + 1;
	const uint64_t yCount = static_cast<uint64_t>(MaxTile.Y) - MinTile.Y + 1;
	const uint64_t estimatedTileCount = xCount * yCount * (IsTileOffset() ? 4 : 1);
	targetTiles.reserve(static_cast<size_t>(estimatedTileCount));

	std::unordered_set<Tile, TileHash, TileEqual> uniqueTiles;
	uniqueTiles.reserve(static_cast<size_t>(estimatedTileCount));

	auto addTile = [&](const Tile& tile)
	{
		if (uniqueTiles.insert(tile).second)
		{
			targetTiles.push_back(tile);
		}
	};

	for (uint32_t xBase = MinTile.X; xBase <= MaxTile.X; xBase++)
	{
		if (bStop) break;
		for (uint32_t yBase = MinTile.Y; yBase <= MaxTile.Y; yBase++)
		{
			if (bStop) break;

			if (IsTileOffset())
			{
				const TileRange offsetTiles = GetOffsetTiles(Tile(xBase, yBase, Zoom));
				for (uint32_t x = offsetTiles.MinTile.X; x <= offsetTiles.MaxTile.X; x++)
				{
					for (uint32_t y = offsetTiles.MinTile.Y; y <= offsetTiles.MaxTile.Y; y++)
					{
						addTile(Tile(x, y, Zoom));
					}
				}
			}
			else
			{
				addTile(Tile(xBase, yBase, Zoom));
			}
		}
	}
}

DownTool::TileCacheState DownTool::DownToolBase::GetTileCacheState(
	const Tile& tile) const
{
	const std::filesystem::path tilePath = GetTileFilePath(tile);
	std::error_code existsError;
	const bool tileExists = std::filesystem::exists(tilePath, existsError);
	if (existsError)
	{
		return TileCacheState::Invalid;
	}

	if (!tileExists)
	{
		return TileCacheState::Missing;
	}

	if (IsUnavailableTileFile(tilePath, tile))
	{
		return TileCacheState::Unavailable;
	}

	if (IsValidImageFile(tilePath))
	{
		return TileCacheState::Ready;
	}

	return TileCacheState::Invalid;
}

std::vector<DownTool::Tile> DownTool::DownToolBase::ScanCacheAndCollectPendingTiles(
	const std::vector<Tile>& targetTiles,
	const std::string& phaseName,
	bool updateProgress)
{
	std::vector<TileCacheState> states(targetTiles.size(), TileCacheState::Missing);
	fmt::print("{}: 开始扫描 {} 个瓦片\n", phaseName, targetTiles.size());

	const uint32_t plannedWorkerCount = GetCacheScanWorkerCount(targetTiles.size());
	std::atomic_size_t nextTileIndex = 0;
	std::atomic_size_t scannedTileCount = 0;
	std::atomic_uint32_t scanErrorCount = 0;
	std::mutex scanErrorMutex;
	std::string firstScanErrorMessage;
	std::vector<std::thread> scanWorkers;
	scanWorkers.reserve(plannedWorkerCount);

	auto recordScanError = [&](const Tile& tile, const char* message)
	{
		if (scanErrorCount.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			std::lock_guard<std::mutex> lock(scanErrorMutex);
			firstScanErrorMessage = fmt::format(
				"z{} ({}, {}) {}",
				tile.Zoom,
				tile.X,
				tile.Y,
				message);
		}
	};

	auto scanTile = [&](size_t index)
	{
		try
		{
			states[index] = GetTileCacheState(targetTiles[index]);
		}
		catch (const std::exception& exception)
		{
			states[index] = TileCacheState::Invalid;
			recordScanError(targetTiles[index], exception.what());
		}
		catch (...)
		{
			states[index] = TileCacheState::Invalid;
			recordScanError(targetTiles[index], "未知异常");
		}
	};

	for (uint32_t workerIndex = 0; workerIndex < plannedWorkerCount; workerIndex++)
	{
		(void)workerIndex;
		try
		{
			scanWorkers.emplace_back(
				[&]()
				{
					for (;;)
					{
						const size_t blockBegin = nextTileIndex.fetch_add(
							CacheScanBatchSize, std::memory_order_relaxed);
						if (blockBegin >= targetTiles.size())
						{
							break;
						}

						const size_t blockEnd =
							std::min(blockBegin + CacheScanBatchSize, targetTiles.size());
						for (size_t index = blockBegin; index < blockEnd; index++)
						{
							scanTile(index);
						}
						scannedTileCount.fetch_add(
							blockEnd - blockBegin, std::memory_order_relaxed);
					}
				});
		}
		catch (const std::exception& exception)
		{
			fmt::print("\n{}: 创建缓存检测 worker 失败: {}\n",
				phaseName, exception.what());
			break;
		}
	}

	if (scanWorkers.empty())
	{
		for (size_t index = 0; index < targetTiles.size(); index++)
		{
			scanTile(index);
			scannedTileCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	size_t lastPrintedCount = 0;
	while (lastPrintedCount < targetTiles.size())
	{
		const size_t currentCount = scannedTileCount.load(std::memory_order_relaxed);
		if (currentCount != lastPrintedCount)
		{
			fmt::print("\r{}: {}/{}，worker {}",
				phaseName, currentCount, targetTiles.size(), scanWorkers.size());
			lastPrintedCount = currentCount;
		}

		if (currentCount >= targetTiles.size())
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	for (std::thread& worker : scanWorkers)
	{
		if (worker.joinable())
		{
			worker.join();
		}
	}
	fmt::print("\r{}: {}/{}，worker {}\n",
		phaseName, targetTiles.size(), targetTiles.size(), scanWorkers.size());
	if (scanErrorCount.load(std::memory_order_relaxed) > 0)
	{
		fmt::print("{}: 缓存检测异常 {} 次，首个异常: {}\n",
			phaseName,
			scanErrorCount.load(std::memory_order_relaxed),
			firstScanErrorMessage);
	}

	uint32_t readyCount = 0;
	uint32_t missingCount = 0;
	uint32_t unavailableCount = 0;
	uint32_t invalidCount = 0;
	std::vector<Tile> pendingTiles;
	pendingTiles.reserve(targetTiles.size());

	std::vector<size_t> issueSampleIndexes;
	issueSampleIndexes.reserve(MaxCacheIssueSampleCount);
	for (size_t index = 0; index < targetTiles.size(); index++)
	{
		switch (states[index])
		{
		case TileCacheState::Ready:
			readyCount++;
			break;
		case TileCacheState::Missing:
			missingCount++;
			pendingTiles.push_back(targetTiles[index]);
			break;
		case TileCacheState::Unavailable:
			unavailableCount++;
			pendingTiles.push_back(targetTiles[index]);
			break;
		case TileCacheState::Invalid:
			invalidCount++;
			pendingTiles.push_back(targetTiles[index]);
			break;
		default:
			pendingTiles.push_back(targetTiles[index]);
			break;
		}

		if (states[index] != TileCacheState::Ready &&
			issueSampleIndexes.size() < MaxCacheIssueSampleCount)
		{
			issueSampleIndexes.push_back(index);
		}
	}

	if (updateProgress)
	{
		DowningCount = static_cast<std::int32_t>(readyCount);
	}

	fmt::print("{}完成：可用 {}，缺失 {}，占位图 {}，无效文件 {}，待处理 {}\n",
		phaseName, readyCount, missingCount, unavailableCount, invalidCount,
		pendingTiles.size());
	if (!issueSampleIndexes.empty())
	{
		fmt::print("{}问题样例（最多 {} 个）：\n",
			phaseName, MaxCacheIssueSampleCount);
		for (size_t index : issueSampleIndexes)
		{
			const Tile& tile = targetTiles[index];
			fmt::print("  [{}] z{} ({}, {}) {}\n",
				GetTileCacheStateText(states[index]),
				tile.Zoom,
				tile.X,
				tile.Y,
				GetTileFilePath(tile).string());
		}
	}

	if (updateProgress)
	{
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
	}

	return pendingTiles;
}

void DownTool::DownToolBase::DownloadTile(uint32_t x, uint32_t y)
{
	// 等待至少一个下载完成
	while (curlEasyHandles.size() >= GetEffectiveQueueCount(MaxDownQueue))
	{
		WaitDown();
	}

	Tile tile(x, y, GetZoom());
	std::filesystem::path nativePath = GetTileFilePath(tile);

	if (IsTileDownloading(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	//如果文件存在，检测是不是一个有效的图片文件，如果是的话就跳过下载，如果不是的话就删除这个文件重新下载
	std::error_code existsError;
	const bool tileExists = std::filesystem::exists(nativePath, existsError);
	if (existsError)
	{
		fmt::print("\n检测缓存文件失败，将重新下载: {}\n", nativePath.string());
	}
	if (tileExists)
	{
		if (IsUnavailableTileFile(nativePath, tile))
		{
			if (!RepairTileFromLowerZoom(tile, nativePath))
			{
				fmt::print("\n无法从低级别瓦片修复占位图: {}\n", nativePath.string());
			}

			DowningCount++;
			fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
			return;
		}
		else if (IsValidImageFile(nativePath))
		{
			DowningCount++;
			fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
			return;
		}
	}

	if (!QueueTileDownload(tile, nativePath, CurlRequestType::Tile, tile.Zoom))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
	}
}

std::filesystem::path DownTool::DownToolBase::GetTileFilePath(const Tile& tile) const
{
	std::string filePath = fmt::format("{}/{}/{}/{}.png", CachePath, tile.Zoom, tile.X, tile.Y);
	return std::filesystem::u8path(filePath);
}

bool DownTool::DownToolBase::QueueTileDownload(
	const Tile& tile,
	const std::filesystem::path& tilePath,
	CurlRequestType requestType,
	uint32_t repairTargetZoom)
{
	if (IsTileDownloading(tile))
	{
		return true;
	}

	std::error_code createDirectoryError;
	std::filesystem::create_directories(tilePath.parent_path(), createDirectoryError);
	if (createDirectoryError)
	{
		return false;
	}

	CurlMultiHandle multiHandle;
	multiHandle.fileStream.open(tilePath, std::ios::binary);
	if (!multiHandle.fileStream.is_open())
	{
		fmt::print("无法打开文件进行写入，切片将会被跳过，解决文件占用问题后再重新下载一遍即可修复：{}\n", tilePath.string());
		return false;
	}
	multiHandle.owner = this;
	multiHandle.requestType = requestType;
	multiHandle.tile = tile;
	multiHandle.repairTargetZoom = repairTargetZoom;
	multiHandle.filePath = tilePath;

	CURL* easy = curl_easy_init();
	if (!easy)
	{
		fmt::print("无法初始化 CURL easy handle\n");
		return false;
	}

	std::string url = MakeUri(multiHandle.tile);
	multiHandle.url = url;
	//downTileCount[url.substr(0, 20)]++;
	curlEasyHandles[easy] = std::move(multiHandle);
	//避免高速下载被服务器拒绝，设置最大连接数为6（根据服务器限制调整）
	//curl_easy_setopt(easy, CURLOPT_MAXCONNECTS, 2L);
	if (share)
	{
		CURLcode result = curl_easy_setopt(easy, CURLOPT_SHARE, share);
		if (result != CURLE_OK)
		{
			fmt::print("设置 CURL 共享 DNS 失败: {}\n", curl_easy_strerror(result));
		}
	}
	curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteTileData);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, &curlEasyHandles[easy]);
	if (curl_multi_add_handle(curlMulti, easy) != CURLM_OK)
	{
		fmt::print("无法将 CURL easy handle 添加到 multi handle\n");
		curlEasyHandles[easy].fileStream.close();
		curlEasyHandles.erase(easy);
		curl_easy_cleanup(easy);
		return false;
	}

	return true;
}

bool DownTool::DownToolBase::IsTileDownloading(const Tile& tile) const
{
	for (const auto& [easy, context] : curlEasyHandles)
	{
		(void)easy;
		if (IsSameTile(context.tile, tile))
		{
			return true;
		}
	}

	return false;
}

uint32_t DownTool::DownToolBase::CancelPendingChildDownloads(
	const Tile& parentTile, uint32_t childZoom)
{
	if (childZoom <= parentTile.Zoom)
	{
		return 0;
	}

	const uint32_t zoomOffset = childZoom - parentTile.Zoom;
	if (zoomOffset >= 31)
	{
		return 0;
	}

	const uint64_t scale = uint64_t{ 1 } << zoomOffset;
	const uint64_t minX = static_cast<uint64_t>(parentTile.X) * scale;
	const uint64_t minY = static_cast<uint64_t>(parentTile.Y) * scale;
	const uint64_t maxX = minX + scale;
	const uint64_t maxY = minY + scale;

	std::vector<CURL*> handlesToCancel;
	for (const auto& [easy, context] : curlEasyHandles)
	{
		const uint64_t tileX = context.tile.X;
		const uint64_t tileY = context.tile.Y;
		if (context.requestType == CurlRequestType::Tile &&
			context.tile.Zoom == childZoom &&
			minX <= tileX && tileX < maxX &&
			minY <= tileY && tileY < maxY)
		{
			handlesToCancel.push_back(easy);
		}
	}

	uint32_t canceledCount = 0;
	for (CURL* easy : handlesToCancel)
	{
		auto handleIt = curlEasyHandles.find(easy);
		if (handleIt == curlEasyHandles.end())
		{
			continue;
		}

		handleIt->second.fileStream.close();
		curl_multi_remove_handle(curlMulti, easy);
		curl_easy_cleanup(easy);
		curlEasyHandles.erase(handleIt);
		canceledCount++;
	}

	return canceledCount;
}

bool DownTool::DownToolBase::RepairTileFromLowerZoom(
	const Tile& tile, const std::filesystem::path& tilePath)
{
	if (tile.Zoom == 0)
	{
		return false;
	}

	return RepairTileFromLowerZoom(tile, tilePath, tile.Zoom - 1);
}

bool DownTool::DownToolBase::RepairTileFromLowerZoom(
	const Tile& tile,
	const std::filesystem::path& tilePath,
	uint32_t startParentZoom)
{
	(void)tilePath;

	if (tile.Zoom == 0 || startParentZoom >= tile.Zoom)
	{
		return false;
	}

	if (startParentZoom < GetMinFallbackZoom())
	{
		return false;
	}

	for (uint32_t parentZoom = startParentZoom;; parentZoom--)
	{
		const uint32_t zoomOffset = tile.Zoom - parentZoom;
		if (zoomOffset >= 31)
		{
			return false;
		}

		const uint32_t scale = 1u << zoomOffset;
		const Tile parentTile(tile.X / scale, tile.Y / scale, parentZoom);
		const std::filesystem::path parentPath = GetTileFilePath(parentTile);
		std::error_code parentExistsError;
		const bool parentExists = std::filesystem::exists(parentPath, parentExistsError);
		if (parentExistsError)
		{
			if (parentZoom == GetMinFallbackZoom())
			{
				break;
			}
			continue;
		}
		if (parentExists && IsUnavailableTileFile(parentPath, parentTile))
		{
			if (parentZoom == GetMinFallbackZoom())
			{
				break;
			}
			continue;
		}

		if (!parentExists || !IsValidImageFile(parentPath))
		{
			if (QueueTileDownload(
				parentTile, parentPath, CurlRequestType::RepairParent, tile.Zoom))
			{
				return true;
			}

			if (parentZoom == GetMinFallbackZoom())
			{
				break;
			}
			continue;
		}

		if (RepairTilesFromParent(parentTile, tile.Zoom))
		{
			return true;
		}

		if (parentZoom == GetMinFallbackZoom())
		{
			break;
		}
	}

	return false;
}

bool DownTool::DownToolBase::RepairTilesFromParent(
	const Tile& parentTile, uint32_t targetZoom)
{
	if (targetZoom <= parentTile.Zoom)
	{
		return false;
	}

	const uint32_t zoomOffset = targetZoom - parentTile.Zoom;
	if (zoomOffset >= 31)
	{
		return false;
	}

	const uint32_t scale = 1u << zoomOffset;
	const uint64_t batchTileCount = static_cast<uint64_t>(scale) * scale;
	if (batchTileCount > MaxBatchRepairedTileCount)
	{
		return false;
	}

	const std::filesystem::path parentPath = GetTileFilePath(parentTile);
	if (!IsValidImageFile(parentPath) || IsUnavailableTileFile(parentPath, parentTile))
	{
		return false;
	}

	cv::Mat parentImage = ReadImage(parentPath);
	if (parentImage.empty())
	{
		return false;
	}

	const int cropWidth = parentImage.cols / static_cast<int>(scale);
	const int cropHeight = parentImage.rows / static_cast<int>(scale);
	if (cropWidth <= 0 || cropHeight <= 0)
	{
		return false;
	}

	auto writeRepairedTile = [&](const Tile& childTile, uint32_t childX, uint32_t childY)
		-> TileRepairWriteResult
	{
		const std::filesystem::path childPath = GetTileFilePath(childTile);
		std::error_code childExistsError;
		const bool childExists = std::filesystem::exists(childPath, childExistsError);
		if (!childExistsError && childExists &&
			IsValidImageFile(childPath) &&
			!IsUnavailableTileFile(childPath, childTile))
		{
			return TileRepairWriteResult::Existing;
		}

		const int cropX = static_cast<int>(childX) * cropWidth;
		const int cropY = static_cast<int>(childY) * cropHeight;
		if (cropX + cropWidth > parentImage.cols || cropY + cropHeight > parentImage.rows)
		{
			return TileRepairWriteResult::Failed;
		}

		cv::Mat cropped = parentImage(cv::Rect(cropX, cropY, cropWidth, cropHeight));
		cv::Mat resized;
		cv::resize(cropped, resized, parentImage.size(), 0., 0., cv::INTER_CUBIC);
		if (!WriteImage(childPath, resized))
		{
			return TileRepairWriteResult::Failed;
		}

		return TileRepairWriteResult::Written;
	};

	const uint32_t canceledTileCount = CancelPendingChildDownloads(parentTile, targetZoom);
	DowningCount += canceledTileCount;

	std::vector<TileRepairTask> repairTasks;
	repairTasks.reserve(static_cast<size_t>(batchTileCount));
	for (uint32_t childX = 0; childX < scale; childX++)
	{
		for (uint32_t childY = 0; childY < scale; childY++)
		{
			const Tile childTile(
				parentTile.X * scale + childX,
				parentTile.Y * scale + childY,
				targetZoom);
			repairTasks.push_back(TileRepairTask{ childTile, childX, childY });
		}
	}

	std::atomic_uint32_t atomicReadyTileCount = 0;
	std::atomic_uint32_t atomicRepairedTileCount = 0;
	const int stripeCount = GetTileRepairStripeCount(repairTasks.size(), MaxDownQueue);
	cv::parallel_for_(
		cv::Range(0, static_cast<int>(repairTasks.size())),
		[&](const cv::Range& range)
		{
			for (int taskIndex = range.start; taskIndex < range.end; taskIndex++)
			{
				const TileRepairTask& task = repairTasks[static_cast<size_t>(taskIndex)];
				TileRepairWriteResult result = TileRepairWriteResult::Failed;
				try
				{
					result = writeRepairedTile(task.childTile, task.childX, task.childY);
				}
				catch (const std::exception&)
				{
					result = TileRepairWriteResult::Failed;
				}
				catch (...)
				{
					result = TileRepairWriteResult::Failed;
				}
				if (result != TileRepairWriteResult::Failed)
				{
					atomicReadyTileCount.fetch_add(1, std::memory_order_relaxed);
				}
				if (result == TileRepairWriteResult::Written)
				{
					atomicRepairedTileCount.fetch_add(1, std::memory_order_relaxed);
				}
			}
		},
		stripeCount);

	const uint32_t readyTileCount = atomicReadyTileCount.load(std::memory_order_relaxed);
	const uint32_t repairedTileCount =
		atomicRepairedTileCount.load(std::memory_order_relaxed);

	fmt::print("\n使用低级别瓦片批量修复占位图: z{} ({}, {}) -> z{}，可用 {} / {} 张，写入 {} 张\n",
		parentTile.Zoom, parentTile.X, parentTile.Y, targetZoom,
		readyTileCount, batchTileCount, repairedTileCount);
	return readyTileCount == batchTileCount;
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
