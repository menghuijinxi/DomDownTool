#include "DownToolBase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "assimp/Exporter.hpp"
#include "assimp/material.h"
#include "assimp/metadata.h"
#include "assimp/scene.h"
#include "cpl_json.h"
#include "fmt/format.h"
#include "libxml/tree.h"
#include "libxml/xmlmemory.h"
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
		int32_t childX = 0;
		int32_t childY = 0;
	};

	struct TileSample
	{
		DownTool::Tile sourceTile;
		double pixelX = 0.;
		double pixelY = 0.;
		bool valid = false;
	};

	struct PostProcessBounds
	{
		double west = 0.;
		double east = 0.;
		double south = 0.;
		double north = 0.;
	};

	struct PostProcessMetadata
	{
		PostProcessBounds bounds;
		uint32_t maxZoom = 0;
		uint32_t tileSize = 256;
	};

	struct MergedImageOutputSize
	{
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct FbxGlobalPixelBounds
	{
		double minX = 0.;
		double minY = 0.;
		double maxX = 0.;
		double maxY = 0.;
	};

	struct FbxTextureOutputInfo
	{
		int32_t gridX = 0;
		int32_t gridY = 0;
		uint32_t ring = 0;
		uint32_t zoom = 0;
		uint32_t tileSize = 256;
		uint32_t sourceTextureSize = 4096;
		uint32_t outputTextureSize = 4096;
		uint32_t distanceOffset = 0;
		int32_t centerTileX = 0;
		int32_t centerTileY = 0;
		double centerPixelX = 0.;
		double centerPixelY = 0.;
		double distanceCm = 0.;
		bool partial = false;
		FbxGlobalPixelBounds pixelBounds;
		DownTool::TileRange tileRange;
		PostProcessBounds bounds;
		std::filesystem::path texturePath;
		std::filesystem::path metadataPath;
	};

	struct FbxMeshVertex
	{
		double x = 0.;
		double y = 0.;
		double z = 0.;
		double u = 0.;
		double v = 0.;
	};

	struct FbxMeshData
	{
		std::string name;
		std::filesystem::path texturePath;
		std::vector<FbxMeshVertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct FbxCoordinateContext
	{
		DownTool::LonLat centerLonLat;
		double centerMercatorX = 0.;
		double centerMercatorY = 0.;
		std::array<double, 3> centerEcef = { 0., 0., 0. };
		double sinCenterLon = 0.;
		double cosCenterLon = 1.;
		double sinCenterLat = 0.;
		double cosCenterLat = 1.;
	};

	bool WriteImage(const std::filesystem::path& path, const cv::Mat& image);

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

	uint32_t GetHardwareThreadCount()
	{
		return std::max(1u, std::thread::hardware_concurrency());
	}

	uint32_t GetParallelWorkerCount(uint64_t itemCount, uint32_t configuredQueueCount)
	{
		if (itemCount == 0)
		{
			return 0;
		}

		const uint32_t hardwareThreadCount = GetHardwareThreadCount();
		const uint32_t queueCount = configuredQueueCount == 0 ?
			hardwareThreadCount :
			std::min(configuredQueueCount, hardwareThreadCount);
		return static_cast<uint32_t>(
			std::min<uint64_t>(itemCount, std::max(1u, queueCount)));
	}

	uint64_t GetAvailablePhysicalMemoryBytes()
	{
#if defined(_WIN32)
		MEMORYSTATUSEX memoryStatus{};
		memoryStatus.dwLength = sizeof(memoryStatus);
		if (GlobalMemoryStatusEx(&memoryStatus) != 0)
		{
			return memoryStatus.ullAvailPhys;
		}
#elif defined(__linux__)
		const long availablePages = sysconf(_SC_AVPHYS_PAGES);
		const long pageSize = sysconf(_SC_PAGESIZE);
		if (availablePages > 0 && pageSize > 0)
		{
			return static_cast<uint64_t>(availablePages) *
				static_cast<uint64_t>(pageSize);
		}
#endif

		return 0;
	}

	uint32_t GetFbxTextureWorkerCount(uint32_t textureSize, uint32_t configuredQueueCount)
	{
		const uint64_t textureBytes =
			static_cast<uint64_t>(textureSize) * textureSize * 4 * 2;
		const uint64_t availableMemoryBytes = GetAvailablePhysicalMemoryBytes();
		constexpr uint64_t fallbackWorkingSetBytes = 1024ull * 1024ull * 1024ull;
		const uint64_t targetWorkingSetBytes = availableMemoryBytes == 0 ?
			fallbackWorkingSetBytes :
			std::max<uint64_t>(
				availableMemoryBytes / 2,
				std::min<uint64_t>(availableMemoryBytes, textureBytes));
		const uint32_t memoryLimitedWorkers = static_cast<uint32_t>(
			std::max<uint64_t>(1, targetWorkingSetBytes / std::max<uint64_t>(1, textureBytes)));
		const uint32_t configuredWorkers = configuredQueueCount == 0 ?
			GetHardwareThreadCount() :
			std::max(1u, configuredQueueCount);
		return std::min(configuredWorkers, memoryLimitedWorkers);
	}

	template<typename Function>
	void ParallelForIndex(size_t itemCount, uint32_t configuredQueueCount, Function function)
	{
		const uint32_t workerCount = GetParallelWorkerCount(itemCount, configuredQueueCount);
		if (workerCount <= 1)
		{
			for (size_t index = 0; index < itemCount; index++)
			{
				function(index);
			}
			return;
		}

		std::atomic_size_t nextIndex = 0;
		std::vector<std::thread> workers;
		workers.reserve(workerCount);
		for (uint32_t workerIndex = 0; workerIndex < workerCount; workerIndex++)
		{
			(void)workerIndex;
			workers.emplace_back(
				[&]()
				{
					for (;;)
					{
						const size_t index =
							nextIndex.fetch_add(1, std::memory_order_relaxed);
						if (index >= itemCount)
						{
							break;
						}
						function(index);
					}
				});
		}

		for (std::thread& worker : workers)
		{
			if (worker.joinable())
			{
				worker.join();
			}
		}
	}

	double SecondsSince(const std::chrono::steady_clock::time_point& startTime)
	{
		return std::chrono::duration<double>(
			std::chrono::steady_clock::now() - startTime).count();
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
			static_cast<int32_t>(static_cast<int64_t>(parentTile.X) *
				static_cast<int64_t>(scale)),
			static_cast<int32_t>(static_cast<int64_t>(parentTile.Y) *
				static_cast<int64_t>(scale)),
			targetZoom);
	}

	std::string TrimAsciiWhitespace(const std::string& text)
	{
		size_t begin = 0;
		while (begin < text.size() &&
			std::isspace(static_cast<unsigned char>(text[begin])) != 0)
		{
			begin++;
		}

		size_t end = text.size();
		while (end > begin &&
			std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
		{
			end--;
		}

		return text.substr(begin, end - begin);
	}

	std::string ToLowerAscii(std::string text)
	{
		for (char& character : text)
		{
			character = static_cast<char>(
				std::tolower(static_cast<unsigned char>(character)));
		}

		return text;
	}

	size_t HashCombine(size_t seed, size_t value) noexcept
	{
		return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
	}

	int32_t FloorDiv(int32_t value, int32_t divisor)
	{
		const int32_t quotient = value / divisor;
		const int32_t remainder = value % divisor;
		if (remainder != 0 && ((remainder < 0) != (divisor < 0)))
		{
			return quotient - 1;
		}

		return quotient;
	}

	uint64_t TileRangeCount(int32_t minValue, int32_t maxValue)
	{
		if (maxValue < minValue)
		{
			return 0;
		}

		return static_cast<uint64_t>(
			static_cast<int64_t>(maxValue) - static_cast<int64_t>(minValue) + 1);
	}

	bool TileInParentRange(
		const DownTool::Tile& parentTile,
		const DownTool::Tile& childTile,
		uint32_t childZoom)
	{
		if (childZoom <= parentTile.Zoom || childTile.Zoom != childZoom)
		{
			return false;
		}

		const uint32_t zoomOffset = childZoom - parentTile.Zoom;
		if (zoomOffset >= 31)
		{
			return false;
		}

		const int32_t scale = static_cast<int32_t>(1u << zoomOffset);
		return FloorDiv(childTile.X, scale) == parentTile.X &&
			FloorDiv(childTile.Y, scale) == parentTile.Y;
	}

	struct TileHash
	{
		size_t operator()(const DownTool::Tile& tile) const noexcept
		{
			size_t seed = std::hash<uint32_t>{}(tile.Zoom);
			seed = HashCombine(seed, std::hash<int32_t>{}(tile.X));
			seed = HashCombine(seed, std::hash<int32_t>{}(tile.Y));
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

	size_t WriteTileHeaderData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		DownTool::CurlMultiHandle* context = static_cast<DownTool::CurlMultiHandle*>(userdata);
		const size_t totalSize = size * nmemb;
		const std::string headerLine(static_cast<char*>(ptr), totalSize);
		const size_t separatorIndex = headerLine.find(':');
		if (separatorIndex == std::string::npos)
		{
			return totalSize;
		}

		std::string headerName = ToLowerAscii(TrimAsciiWhitespace(
			headerLine.substr(0, separatorIndex)));
		std::string headerValue = TrimAsciiWhitespace(
			headerLine.substr(separatorIndex + 1));
		if (!headerName.empty())
		{
			context->responseHeaders[std::move(headerName)] = std::move(headerValue);
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

	cv::Mat ReadImageUnchanged(const std::filesystem::path& path)
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
			return cv::imdecode(data, cv::IMREAD_UNCHANGED);
		}
		catch (const cv::Exception&)
		{
			return {};
		}
	}

	cv::Mat EnsureBgra(const cv::Mat& image)
	{
		if (image.empty())
		{
			return {};
		}

		if (image.channels() == 4)
		{
			return image;
		}

		cv::Mat bgraImage;
		if (image.channels() == 3)
		{
			cv::cvtColor(image, bgraImage, cv::COLOR_BGR2BGRA);
			return bgraImage;
		}
		if (image.channels() == 1)
		{
			cv::cvtColor(image, bgraImage, cv::COLOR_GRAY2BGRA);
			return bgraImage;
		}

		return {};
	}

	cv::Mat ReadBgraTile(const std::filesystem::path& path, uint32_t tileSize)
	{
		cv::Mat image = EnsureBgra(ReadImageUnchanged(path));
		if (image.empty())
		{
			return {};
		}

		if (image.cols == static_cast<int>(tileSize) &&
			image.rows == static_cast<int>(tileSize))
		{
			return image;
		}

		cv::Mat resizedImage;
		cv::resize(
			image,
			resizedImage,
			cv::Size(static_cast<int>(tileSize), static_cast<int>(tileSize)),
			0.,
			0.,
			cv::INTER_LINEAR);
		return resizedImage;
	}

	cv::Mat ResizeImageToSize(const cv::Mat& image, int width, int height)
	{
		if (image.empty() || width <= 0 || height <= 0)
		{
			return {};
		}

		if (image.cols == width && image.rows == height)
		{
			return image;
		}

		cv::Mat resizedImage;
		const cv::InterpolationFlags interpolation =
			(image.cols > width || image.rows > height) ?
			cv::INTER_AREA : cv::INTER_LINEAR;
		cv::resize(
			image,
			resizedImage,
			cv::Size(width, height),
			0.,
			0.,
			interpolation);
		return resizedImage;
	}

	cv::Vec4b SampleBilinear(const cv::Mat& image, double pixelX, double pixelY)
	{
		const double clampedX =
			std::clamp(pixelX, 0., static_cast<double>(image.cols - 1));
		const double clampedY =
			std::clamp(pixelY, 0., static_cast<double>(image.rows - 1));
		const int x0 = static_cast<int>(std::floor(clampedX));
		const int y0 = static_cast<int>(std::floor(clampedY));
		const int x1 = std::min(x0 + 1, image.cols - 1);
		const int y1 = std::min(y0 + 1, image.rows - 1);
		const double weightX = clampedX - x0;
		const double weightY = clampedY - y0;

		const cv::Vec4b c00 = image.at<cv::Vec4b>(y0, x0);
		const cv::Vec4b c10 = image.at<cv::Vec4b>(y0, x1);
		const cv::Vec4b c01 = image.at<cv::Vec4b>(y1, x0);
		const cv::Vec4b c11 = image.at<cv::Vec4b>(y1, x1);

		cv::Vec4b result;
		for (int channel = 0; channel < 4; channel++)
		{
			const double top =
				static_cast<double>(c00[channel]) * (1. - weightX) +
				static_cast<double>(c10[channel]) * weightX;
			const double bottom =
				static_cast<double>(c01[channel]) * (1. - weightX) +
				static_cast<double>(c11[channel]) * weightX;
			result[channel] = static_cast<unsigned char>(
				std::clamp(top * (1. - weightY) + bottom * weightY, 0., 255.));
		}

		return result;
	}

	bool CopyFileReplacing(
		const std::filesystem::path& sourcePath,
		const std::filesystem::path& targetPath)
	{
		std::error_code createDirectoryError;
		std::filesystem::create_directories(targetPath.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}

		std::error_code copyError;
		std::filesystem::copy_file(
			sourcePath,
			targetPath,
			std::filesystem::copy_options::overwrite_existing,
			copyError);
		return !copyError;
	}

	bool RemoveFileIfExists(const std::filesystem::path& path)
	{
		std::error_code removeError;
		std::filesystem::remove(path, removeError);
		return !removeError;
	}

	bool WriteTextFile(const std::filesystem::path& path, const std::string& content)
	{
		std::error_code createDirectoryError;
		std::filesystem::create_directories(path.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}

		std::ofstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		fileStream.write(content.data(), static_cast<std::streamsize>(content.size()));
		return fileStream.good();
	}

	bool WriteBinaryFile(
		const std::filesystem::path& path,
		const void* data,
		size_t dataSize)
	{
		std::error_code createDirectoryError;
		std::filesystem::create_directories(path.parent_path(), createDirectoryError);
		if (createDirectoryError)
		{
			return false;
		}

		std::ofstream fileStream(path, std::ios::binary);
		if (!fileStream.is_open())
		{
			return false;
		}

		fileStream.write(
			static_cast<const char*>(data), static_cast<std::streamsize>(dataSize));
		return fileStream.good();
	}

	std::string PathToUtf8String(const std::filesystem::path& path)
	{
		const std::u8string utf8Path = path.u8string();
		std::string result;
		result.reserve(utf8Path.size());
		for (char8_t character : utf8Path)
		{
			result.push_back(static_cast<char>(character));
		}

		return result;
	}

	double LongitudeToWebMercatorX(double longitude)
	{
		constexpr double earthRadius = 6378137.;
		constexpr double pi = 3.141592653589793238462643383279502884;
		return earthRadius * longitude * pi / 180.;
	}

	double LatitudeToWebMercatorY(double latitude)
	{
		constexpr double earthRadius = 6378137.;
		constexpr double pi = 3.141592653589793238462643383279502884;
		constexpr double maxMercatorLatitude = 85.05112877980659;
		const double clampedLatitude =
			std::clamp(latitude, -maxMercatorLatitude, maxMercatorLatitude);
		const double latitudeRadians = clampedLatitude * pi / 180.;
		return earthRadius * std::log(std::tan(pi / 4. + latitudeRadians / 2.));
	}

	DownTool::LonLat WebMercatorToLonLat(double mercatorX, double mercatorY)
	{
		constexpr double earthRadius = 6378137.;
		const double longitude = mercatorX / earthRadius * 180. / std::numbers::pi;
		const double latitude =
			(2. * std::atan(std::exp(mercatorY / earthRadius)) -
				std::numbers::pi / 2.) *
			180. / std::numbers::pi;
		return DownTool::LonLat(longitude, latitude);
	}

	bool IsPowerOfTwo(uint32_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	uint32_t CalculateRing(int32_t gridX, int32_t gridY)
	{
		const int64_t absX = std::llabs(static_cast<int64_t>(gridX));
		const int64_t absY = std::llabs(static_cast<int64_t>(gridY));
		return static_cast<uint32_t>(std::max(absX, absY));
	}

	uint32_t CalculateFbxOutputTextureSize(
		uint32_t sourceTextureSize,
		uint32_t distanceOffset)
	{
		uint32_t outputSize = sourceTextureSize;
		for (uint32_t index = 0; index < distanceOffset && outputSize > 128; index++)
		{
			outputSize /= 2;
		}

		return std::max(128u, outputSize);
	}

	double ConvertImageVToFbxV(double imageV)
	{
		return 1. - imageV;
	}

	std::string MakeFbxTextureName(int32_t gridX, int32_t gridY)
	{
		return fmt::format("{}_{}", gridX, gridY);
	}

	std::string MakeFbxMeshName(int32_t gridX, int32_t gridY)
	{
		return fmt::format("tile_{}_{}", gridX, gridY);
	}

	std::string ToPortableRelativePath(
		const std::filesystem::path& path,
		const std::filesystem::path& basePath)
	{
		std::error_code relativeError;
		std::filesystem::path relativePath =
			std::filesystem::relative(path, basePath, relativeError);
		if (relativeError)
		{
			relativePath = path;
		}

		std::string text = PathToUtf8String(relativePath);
		std::replace(text.begin(), text.end(), '\\', '/');
		return text;
	}

	FbxCoordinateContext BuildFbxCoordinateContext(const DownTool::LonLat& centerLonLat)
	{
		constexpr double semiMajorAxis = 6378137.;
		constexpr double firstEccentricitySquared = 6.69437999014e-3;
		const double centerLonRadians = centerLonLat.lon * std::numbers::pi / 180.;
		const double centerLatRadians = centerLonLat.lat * std::numbers::pi / 180.;
		const double sinCenterLat = std::sin(centerLatRadians);
		const double cosCenterLat = std::cos(centerLatRadians);
		const double sinCenterLon = std::sin(centerLonRadians);
		const double cosCenterLon = std::cos(centerLonRadians);
		const double primeVerticalRadius =
			semiMajorAxis /
			std::sqrt(1. - firstEccentricitySquared * sinCenterLat * sinCenterLat);

		FbxCoordinateContext context;
		context.centerLonLat = centerLonLat;
		context.centerMercatorX = LongitudeToWebMercatorX(centerLonLat.lon);
		context.centerMercatorY = LatitudeToWebMercatorY(centerLonLat.lat);
		context.centerEcef = {
			primeVerticalRadius * cosCenterLat * cosCenterLon,
			primeVerticalRadius * cosCenterLat * sinCenterLon,
			primeVerticalRadius * (1. - firstEccentricitySquared) * sinCenterLat
		};
		context.sinCenterLon = sinCenterLon;
		context.cosCenterLon = cosCenterLon;
		context.sinCenterLat = sinCenterLat;
		context.cosCenterLat = cosCenterLat;
		return context;
	}

	std::array<double, 3> LonLatToEcef(const DownTool::LonLat& lonLat)
	{
		constexpr double semiMajorAxis = 6378137.;
		constexpr double firstEccentricitySquared = 6.69437999014e-3;
		const double lonRadians = lonLat.lon * std::numbers::pi / 180.;
		const double latRadians = lonLat.lat * std::numbers::pi / 180.;
		const double sinLat = std::sin(latRadians);
		const double cosLat = std::cos(latRadians);
		const double cosLon = std::cos(lonRadians);
		const double sinLon = std::sin(lonRadians);
		const double primeVerticalRadius =
			semiMajorAxis / std::sqrt(1. - firstEccentricitySquared * sinLat * sinLat);
		return {
			primeVerticalRadius * cosLat * cosLon,
			primeVerticalRadius * cosLat * sinLon,
			primeVerticalRadius * (1. - firstEccentricitySquared) * sinLat
		};
	}

	FbxMeshVertex MakePlaneVertex(
		const DownTool::LonLat& lonLat,
		const FbxCoordinateContext& context,
		double u,
		double v)
	{
		const double mercatorX = LongitudeToWebMercatorX(lonLat.lon);
		const double mercatorY = LatitudeToWebMercatorY(lonLat.lat);
		return FbxMeshVertex{
			(mercatorX - context.centerMercatorX) * 100.,
			(mercatorY - context.centerMercatorY) * 100.,
			0.,
			u,
			v
		};
	}

	FbxMeshVertex MakeSphereVertex(
		const DownTool::LonLat& lonLat,
		const FbxCoordinateContext& context,
		double u,
		double v)
	{
		const std::array<double, 3> ecef = LonLatToEcef(lonLat);
		const double dx = ecef[0] - context.centerEcef[0];
		const double dy = ecef[1] - context.centerEcef[1];
		const double dz = ecef[2] - context.centerEcef[2];
		const double east =
			-context.sinCenterLon * dx + context.cosCenterLon * dy;
		const double north =
			-context.sinCenterLat * context.cosCenterLon * dx -
			context.sinCenterLat * context.sinCenterLon * dy +
			context.cosCenterLat * dz;
		const double up =
			context.cosCenterLat * context.cosCenterLon * dx +
			context.cosCenterLat * context.sinCenterLon * dy +
			context.sinCenterLat * dz;
		return FbxMeshVertex{ east * 100., north * 100., up * 100., u, v };
	}

	PostProcessBounds BoundsFromCorners(
		const DownTool::LonLat& northWest,
		const DownTool::LonLat& southEast)
	{
		return PostProcessBounds{
			std::min(northWest.lon, southEast.lon),
			std::max(northWest.lon, southEast.lon),
			std::min(northWest.lat, southEast.lat),
			std::max(northWest.lat, southEast.lat)
		};
	}

	DownTool::Tile PixelBoundsToTile(
		double globalPixelX,
		double globalPixelY,
		uint32_t zoom,
		uint32_t tileSize)
	{
		const uint64_t tileCount = uint64_t{ 1 } << zoom;
		const double maxPixel =
			static_cast<double>(tileCount) * static_cast<double>(tileSize);
		const double x = std::clamp(
			globalPixelX, 0., std::nextafter(maxPixel, 0.));
		const double y = std::clamp(
			globalPixelY, 0., std::nextafter(maxPixel, 0.));
		return DownTool::Tile(
			static_cast<uint32_t>(std::floor(x / tileSize)),
			static_cast<uint32_t>(std::floor(y / tileSize)),
			zoom);
	}

	const xmlChar* ToXmlChar(const char* text)
	{
		return reinterpret_cast<const xmlChar*>(text);
	}

	const xmlChar* ToXmlChar(const std::string& text)
	{
		return reinterpret_cast<const xmlChar*>(text.c_str());
	}

	std::string FormatMetadataDouble(double value)
	{
		return fmt::format("{:.15g}", value);
	}

	bool SetXmlProperty(xmlNodePtr node, const char* name, const std::string& value)
	{
		return xmlSetProp(node, ToXmlChar(name), ToXmlChar(value)) != nullptr;
	}

	bool SetXmlProperty(xmlNodePtr node, const char* name, const char* value)
	{
		return xmlSetProp(node, ToXmlChar(name), ToXmlChar(value)) != nullptr;
	}

	xmlNodePtr AddXmlChild(xmlNodePtr parent, const char* name, const char* content)
	{
		return xmlNewChild(parent, nullptr, ToXmlChar(name), ToXmlChar(content));
	}

	xmlNodePtr AddXmlChild(xmlNodePtr parent, const char* name)
	{
		return xmlNewChild(parent, nullptr, ToXmlChar(name), nullptr);
	}

	CPLJSONObject BuildBoundsJson(const PostProcessBounds& bounds)
	{
		CPLJSONObject jsonBounds;
		jsonBounds.Add("east", bounds.east);
		jsonBounds.Add("north", bounds.north);
		jsonBounds.Add("south", bounds.south);
		jsonBounds.Add("west", bounds.west);
		return jsonBounds;
	}

	std::string BuildMetaJson(const PostProcessMetadata& metadata)
	{
		CPLJSONObject root;
		root.Add("bounds", BuildBoundsJson(metadata.bounds));
		root.Add("contentType", "image/png");
		root.Add("generatetool", "DomDownTool");
		root.Add("latLonBounds", BuildBoundsJson(metadata.bounds));
		root.Add("maxzoom", static_cast<int>(metadata.maxZoom));
		root.Add("minzoom", 0);
		root.Add("proj", 4326);
		root.Add("tilesize", static_cast<int>(metadata.tileSize));
		root.Add("tiletrans", "google");
		root.Add("type", "image");
		root.Add("ziped", false);
		return root.Format(CPLJSONObject::PrettyFormat::Plain);
	}

	CPLJSONObject BuildFbxGlobalPixelBoundsJson(const FbxGlobalPixelBounds& bounds)
	{
		CPLJSONObject root;
		root.Add("maxx", bounds.maxX);
		root.Add("maxy", bounds.maxY);
		root.Add("minx", bounds.minX);
		root.Add("miny", bounds.minY);
		return root;
	}

	CPLJSONObject BuildFbxTileJson(const DownTool::Tile& tile)
	{
		CPLJSONObject root;
		root.Add("x", tile.X);
		root.Add("y", tile.Y);
		root.Add("z", static_cast<int>(tile.Zoom));
		return root;
	}

	CPLJSONObject BuildFbxTileRangeJson(const DownTool::TileRange& tileRange)
	{
		CPLJSONObject root;
		root.Add("max", BuildFbxTileJson(tileRange.MaxTile));
		root.Add("min", BuildFbxTileJson(tileRange.MinTile));
		return root;
	}

	CPLJSONObject BuildFbxTextureJson(
		const FbxTextureOutputInfo& textureInfo,
		const std::filesystem::path& basePath)
	{
		CPLJSONObject root;
		root.Add("bounds", BuildBoundsJson(textureInfo.bounds));
		root.Add("centerpixelx", textureInfo.centerPixelX);
		root.Add("centerpixely", textureInfo.centerPixelY);
		root.Add("centertilex", textureInfo.centerTileX);
		root.Add("centertiley", textureInfo.centerTileY);
		root.Add("distancecm", textureInfo.distanceCm);
		root.Add("distanceoffset", static_cast<int>(textureInfo.distanceOffset));
		root.Add("gridx", textureInfo.gridX);
		root.Add("gridy", textureInfo.gridY);
		root.Add("partial", textureInfo.partial);
		root.Add("path", ToPortableRelativePath(textureInfo.texturePath, basePath));
		root.Add("pixelbounds", BuildFbxGlobalPixelBoundsJson(textureInfo.pixelBounds));
		root.Add("ring", static_cast<int>(textureInfo.ring));
		root.Add("sourcetexturesize", static_cast<int>(textureInfo.sourceTextureSize));
		root.Add("outputtexturesize", static_cast<int>(textureInfo.outputTextureSize));
		root.Add("tilerange", BuildFbxTileRangeJson(textureInfo.tileRange));
		root.Add("tilesize", static_cast<int>(textureInfo.tileSize));
		root.Add("zoom", static_cast<int>(textureInfo.zoom));
		root.Add("version", 1);
		return root;
	}

	std::string BuildFbxTextureMetadataJson(
		const FbxTextureOutputInfo& textureInfo,
		const std::filesystem::path& basePath)
	{
		return BuildFbxTextureJson(textureInfo, basePath).Format(
			CPLJSONObject::PrettyFormat::Plain);
	}

	std::string BuildFbxManifestJson(
		const std::vector<FbxTextureOutputInfo>& textureInfos,
		const std::filesystem::path& basePath,
		const std::string& meshMode,
		uint32_t textureSize,
		uint32_t zoom,
		uint32_t tileSize)
	{
		CPLJSONArray textures;
		for (const FbxTextureOutputInfo& textureInfo : textureInfos)
		{
			textures.Add(BuildFbxTextureJson(textureInfo, basePath));
		}

		CPLJSONObject root;
		root.Add("meshmode", meshMode);
		root.Add("texturecount", static_cast<int>(textureInfos.size()));
		root.Add("textures", textures);
		root.Add("texturesize", static_cast<int>(textureSize));
		root.Add("tilesize", static_cast<int>(tileSize));
		root.Add("version", 1);
		root.Add("zoom", static_cast<int>(zoom));
		return root.Format(CPLJSONObject::PrettyFormat::Plain);
	}

	uint32_t SelectFbxDistanceOffset(
		double distanceCm,
		const std::vector<DownTool::FbxTextureDistanceOffset>& offsets)
	{
		uint32_t selectedOffset = 0;
		double selectedDistance = -std::numeric_limits<double>::infinity();
		for (const DownTool::FbxTextureDistanceOffset& offset : offsets)
		{
			if (offset.Distance <= distanceCm && offset.Distance >= selectedDistance)
			{
				selectedDistance = offset.Distance;
				selectedOffset = offset.Offset;
			}
		}

		return selectedOffset;
	}

	void FillFbxTextureFromTile(
		cv::Mat& textureImage,
		const cv::Mat& sourceTileImage,
		const FbxGlobalPixelBounds& textureBounds,
		const FbxGlobalPixelBounds& sourceTileBounds,
		const FbxGlobalPixelBounds& requestBounds,
		uint32_t tileSize)
	{
		const double overlapMinX = std::max({
			textureBounds.minX, sourceTileBounds.minX, requestBounds.minX });
		const double overlapMinY = std::max({
			textureBounds.minY, sourceTileBounds.minY, requestBounds.minY });
		const double overlapMaxX = std::min({
			textureBounds.maxX, sourceTileBounds.maxX, requestBounds.maxX });
		const double overlapMaxY = std::min({
			textureBounds.maxY, sourceTileBounds.maxY, requestBounds.maxY });
		if (overlapMaxX <= overlapMinX || overlapMaxY <= overlapMinY)
		{
			return;
		}

		const int destX0 = std::clamp(
			static_cast<int>(std::floor(overlapMinX - textureBounds.minX)),
			0,
			textureImage.cols - 1);
		const int destY0 = std::clamp(
			static_cast<int>(std::floor(overlapMinY - textureBounds.minY)),
			0,
			textureImage.rows - 1);
		const int destX1 = std::clamp(
			static_cast<int>(std::ceil(overlapMaxX - textureBounds.minX)),
			destX0 + 1,
			textureImage.cols);
		const int destY1 = std::clamp(
			static_cast<int>(std::ceil(overlapMaxY - textureBounds.minY)),
			destY0 + 1,
			textureImage.rows);

		const int sourceX0 = std::clamp(
			static_cast<int>(std::floor(overlapMinX - sourceTileBounds.minX)),
			0,
			static_cast<int>(tileSize) - 1);
		const int sourceY0 = std::clamp(
			static_cast<int>(std::floor(overlapMinY - sourceTileBounds.minY)),
			0,
			static_cast<int>(tileSize) - 1);
		const int sourceX1 = std::clamp(
			static_cast<int>(std::ceil(overlapMaxX - sourceTileBounds.minX)),
			sourceX0 + 1,
			static_cast<int>(tileSize));
		const int sourceY1 = std::clamp(
			static_cast<int>(std::ceil(overlapMaxY - sourceTileBounds.minY)),
			sourceY0 + 1,
			static_cast<int>(tileSize));

		const cv::Mat sourceRoi = sourceTileImage(cv::Rect(
			sourceX0,
			sourceY0,
			sourceX1 - sourceX0,
			sourceY1 - sourceY0));
		cv::Mat targetRoi = textureImage(cv::Rect(
			destX0,
			destY0,
			destX1 - destX0,
			destY1 - destY0));
		const cv::InterpolationFlags interpolation =
			(sourceRoi.cols > targetRoi.cols || sourceRoi.rows > targetRoi.rows) ?
			cv::INTER_AREA : cv::INTER_LINEAR;
		cv::resize(sourceRoi, targetRoi, targetRoi.size(), 0., 0., interpolation);
	}

	bool WriteFbxTextureImage(
		const FbxTextureOutputInfo& textureInfo,
		const FbxGlobalPixelBounds& requestBounds,
		uint32_t tileSize,
		const std::function<cv::Mat(const DownTool::Tile&)>& readTileImage)
	{
		const int textureSize = static_cast<int>(textureInfo.sourceTextureSize);
		cv::Mat textureImage(textureSize, textureSize, CV_8UC4, cv::Scalar(0, 0, 0, 0));
		bool hasPixel = false;

		for (int32_t x = textureInfo.tileRange.MinTile.X;
			x <= textureInfo.tileRange.MaxTile.X; x++)
		{
			for (int32_t y = textureInfo.tileRange.MinTile.Y;
				y <= textureInfo.tileRange.MaxTile.Y; y++)
			{
				const DownTool::Tile sourceTile(x, y, textureInfo.zoom);
				const cv::Mat sourceTileImage = readTileImage(sourceTile);
				if (sourceTileImage.empty())
				{
					return false;
				}

				const double tileMinX = static_cast<double>(x) * tileSize;
				const double tileMinY = static_cast<double>(y) * tileSize;
				const FbxGlobalPixelBounds sourceTileBounds{
					tileMinX,
					tileMinY,
					tileMinX + tileSize,
					tileMinY + tileSize
				};
				FillFbxTextureFromTile(
					textureImage,
					sourceTileImage,
					textureInfo.pixelBounds,
					sourceTileBounds,
					requestBounds,
					tileSize);
				hasPixel = true;
			}
		}

		if (!hasPixel)
		{
			return false;
		}

		const cv::Mat outputImage =
			textureInfo.outputTextureSize == textureInfo.sourceTextureSize ?
			textureImage :
			ResizeImageToSize(
				textureImage,
				static_cast<int>(textureInfo.outputTextureSize),
				static_cast<int>(textureInfo.outputTextureSize));
		return WriteImage(textureInfo.texturePath, outputImage);
	}

	std::vector<FbxMeshVertex> BuildFbxMeshVertices(
		const FbxTextureOutputInfo& textureInfo,
		const FbxCoordinateContext& coordinateContext,
		DownTool::FbxPlanetShape shape,
		uint32_t subdivision)
	{
		const uint32_t gridCount =
			shape == DownTool::FbxPlanetShape::Sphere ?
			std::max(1u, subdivision) : 1u;
		std::vector<FbxMeshVertex> vertices;
		vertices.reserve(static_cast<size_t>(gridCount + 1) * (gridCount + 1));

		for (uint32_t y = 0; y <= gridCount; y++)
		{
			const double imageV = static_cast<double>(y) / gridCount;
			const double fbxV = ConvertImageVToFbxV(imageV);
			const double lat =
				textureInfo.bounds.north +
				(textureInfo.bounds.south - textureInfo.bounds.north) * imageV;
			for (uint32_t x = 0; x <= gridCount; x++)
			{
				const double u = static_cast<double>(x) / gridCount;
				const double lon =
					textureInfo.bounds.west +
					(textureInfo.bounds.east - textureInfo.bounds.west) * u;
				const DownTool::LonLat lonLat(lon, lat);
				if (shape == DownTool::FbxPlanetShape::Sphere)
				{
					vertices.push_back(MakeSphereVertex(lonLat, coordinateContext, u, fbxV));
				}
				else
				{
					vertices.push_back(MakePlaneVertex(lonLat, coordinateContext, u, fbxV));
				}
			}
		}

		return vertices;
	}

	std::vector<uint32_t> BuildFbxMeshIndices(uint32_t subdivision)
	{
		const uint32_t gridCount = std::max(1u, subdivision);
		std::vector<uint32_t> indices;
		indices.reserve(static_cast<size_t>(gridCount) * gridCount * 6);
		const uint32_t stride = gridCount + 1;
		for (uint32_t y = 0; y < gridCount; y++)
		{
			for (uint32_t x = 0; x < gridCount; x++)
			{
				const uint32_t northWest = y * stride + x;
				const uint32_t northEast = northWest + 1;
				const uint32_t southWest = northWest + stride;
				const uint32_t southEast = southWest + 1;
				indices.push_back(northWest);
				indices.push_back(southWest);
				indices.push_back(northEast);
				indices.push_back(northEast);
				indices.push_back(southWest);
				indices.push_back(southEast);
			}
		}

		return indices;
	}

	aiMesh* CreateAssimpMesh(const FbxMeshData& meshData, uint32_t materialIndex)
	{
		aiMesh* mesh = new aiMesh();
		mesh->mName = aiString(meshData.name);
		mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
		mesh->mMaterialIndex = materialIndex;
		mesh->mNumVertices = static_cast<unsigned int>(meshData.vertices.size());
		mesh->mVertices = new aiVector3D[mesh->mNumVertices];
		mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
		mesh->mNumUVComponents[0] = 2;

		for (unsigned int index = 0; index < mesh->mNumVertices; index++)
		{
			const FbxMeshVertex& vertex = meshData.vertices[index];
			mesh->mVertices[index] = aiVector3D(
				static_cast<float>(vertex.x),
				static_cast<float>(vertex.y),
				static_cast<float>(vertex.z));
			mesh->mTextureCoords[0][index] = aiVector3D(
				static_cast<float>(vertex.u),
				static_cast<float>(vertex.v),
				0.f);
		}

		mesh->mNumFaces =
			static_cast<unsigned int>(meshData.indices.size() / 3);
		mesh->mFaces = new aiFace[mesh->mNumFaces];
		for (unsigned int faceIndex = 0; faceIndex < mesh->mNumFaces; faceIndex++)
		{
			aiFace& face = mesh->mFaces[faceIndex];
			face.mNumIndices = 3;
			face.mIndices = new unsigned int[3];
			for (unsigned int index = 0; index < 3; index++)
			{
				face.mIndices[index] =
					meshData.indices[static_cast<size_t>(faceIndex) * 3 + index];
			}
		}

		return mesh;
	}

	aiMaterial* CreateAssimpMaterial(
		const FbxMeshData& meshData,
		const std::filesystem::path& fbxPath)
	{
		aiMaterial* material = new aiMaterial();
		const aiString materialName(meshData.name);
		material->AddProperty(&materialName, AI_MATKEY_NAME);

		const std::string texturePath = ToPortableRelativePath(
			meshData.texturePath, fbxPath.parent_path());
		const aiString diffuseTexture(texturePath);
		material->AddProperty(&diffuseTexture, AI_MATKEY_TEXTURE_DIFFUSE(0));

		aiColor3D diffuseColor(1.f, 1.f, 1.f);
		material->AddProperty(&diffuseColor, 1, AI_MATKEY_COLOR_DIFFUSE);
		return material;
	}

	void SetAssimpFbxGlobalSettings(aiScene& scene)
	{
		scene.mMetaData = aiMetadata::Alloc(10);
		scene.mMetaData->Set(0, "UpAxis", int32_t{ 2 });
		scene.mMetaData->Set(1, "UpAxisSign", int32_t{ 1 });
		scene.mMetaData->Set(2, "FrontAxis", int32_t{ 1 });
		scene.mMetaData->Set(3, "FrontAxisSign", int32_t{ -1 });
		scene.mMetaData->Set(4, "CoordAxis", int32_t{ 0 });
		scene.mMetaData->Set(5, "CoordAxisSign", int32_t{ 1 });
		scene.mMetaData->Set(6, "OriginalUpAxis", int32_t{ 2 });
		scene.mMetaData->Set(7, "OriginalUpAxisSign", int32_t{ 1 });
		scene.mMetaData->Set(8, "UnitScaleFactor", 1.);
		scene.mMetaData->Set(9, "OriginalUnitScaleFactor", 1.);
	}

	bool ExportAssimpFbx(
		const std::vector<FbxMeshData>& meshDatas,
		const std::filesystem::path& fbxPath)
	{
		if (meshDatas.empty())
		{
			return false;
		}

		aiScene scene;
		SetAssimpFbxGlobalSettings(scene);
		scene.mRootNode = new aiNode("RootNode");
		scene.mNumMeshes = static_cast<unsigned int>(meshDatas.size());
		scene.mMeshes = new aiMesh * [scene.mNumMeshes];
		scene.mNumMaterials = scene.mNumMeshes;
		scene.mMaterials = new aiMaterial * [scene.mNumMaterials];

		scene.mRootNode->mNumChildren = scene.mNumMeshes;
		scene.mRootNode->mChildren = new aiNode * [scene.mRootNode->mNumChildren];

		for (unsigned int index = 0; index < scene.mNumMeshes; index++)
		{
			const FbxMeshData& meshData = meshDatas[index];
			scene.mMeshes[index] = CreateAssimpMesh(meshData, index);
			scene.mMaterials[index] = CreateAssimpMaterial(meshData, fbxPath);

			aiNode* childNode = new aiNode(meshData.name);
			childNode->mParent = scene.mRootNode;
			childNode->mNumMeshes = 1;
			childNode->mMeshes = new unsigned int[1];
			childNode->mMeshes[0] = index;
			scene.mRootNode->mChildren[index] = childNode;
		}

		Assimp::Exporter exporter;
		const aiExportDataBlob* blob = exporter.ExportToBlob(&scene, "fbx");
		if (!blob || !blob->data || blob->size == 0)
		{
			fmt::print("FBX 导出失败：{}\n", exporter.GetErrorString());
			return false;
		}

		if (!WriteBinaryFile(fbxPath, blob->data, blob->size))
		{
			fmt::print("FBX 文件写入失败：{}\n", PathToUtf8String(fbxPath));
			return false;
		}

		return true;
	}

	MergedImageOutputSize CalculateMergedImageOutputSize(
		uint64_t tileXCount,
		uint64_t tileYCount,
		uint32_t tileSize,
		uint32_t targetMaxResolution)
	{
		if (tileXCount == 0 || tileYCount == 0 || tileSize == 0 ||
			targetMaxResolution == 0)
		{
			return {};
		}

		const uint64_t maxUint64 = std::numeric_limits<uint64_t>::max();
		if (tileXCount > maxUint64 / tileSize || tileYCount > maxUint64 / tileSize)
		{
			return {};
		}

		const uint64_t sourceWidth = tileXCount * tileSize;
		const uint64_t sourceHeight = tileYCount * tileSize;
		const uint64_t maxSourceSize = std::max(sourceWidth, sourceHeight);
		if (maxSourceSize == 0)
		{
			return {};
		}

		if (maxSourceSize <= targetMaxResolution)
		{
			return {
				static_cast<uint32_t>(sourceWidth),
				static_cast<uint32_t>(sourceHeight)
			};
		}

		const auto scaleToTarget = [maxSourceSize, targetMaxResolution](
			uint64_t sourceSize)
		{
			const long double scaledSize =
				static_cast<long double>(sourceSize) * targetMaxResolution /
				static_cast<long double>(maxSourceSize);
			const long double roundedSize = std::round(scaledSize);
			const long double clampedSize = std::clamp(
				roundedSize,
				1.0L,
				static_cast<long double>(targetMaxResolution));
			return static_cast<uint32_t>(clampedSize);
		};

		return {
			scaleToTarget(sourceWidth),
			scaleToTarget(sourceHeight)
		};
	}

	std::string BuildTileMapResourceXml(const PostProcessMetadata& metadata)
	{
		using XmlDocPtr = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
		struct XmlBufferDeleter
		{
			void operator()(xmlChar* buffer) const
			{
				xmlFree(buffer);
			}
		};
		using XmlBufferPtr = std::unique_ptr<xmlChar, XmlBufferDeleter>;

		XmlDocPtr document(xmlNewDoc(ToXmlChar("1.0")), xmlFreeDoc);
		if (!document)
		{
			return {};
		}

		xmlNodePtr root = xmlNewNode(nullptr, ToXmlChar("TileMap"));
		if (!root)
		{
			return {};
		}
		xmlDocSetRootElement(document.get(), root);

		if (!SetXmlProperty(root, "version", "1.0.0") ||
			!SetXmlProperty(root, "tilemapservice", "http://tms.osgeo.org/1.0.0"))
		{
			return {};
		}

		if (!AddXmlChild(root, "Title", "WMTS") ||
			!AddXmlChild(root, "SRS", "EPSG:3857"))
		{
			return {};
		}

		const double westMercator = LongitudeToWebMercatorX(metadata.bounds.west);
		const double eastMercator = LongitudeToWebMercatorX(metadata.bounds.east);
		const double southMercator = LatitudeToWebMercatorY(metadata.bounds.south);
		const double northMercator = LatitudeToWebMercatorY(metadata.bounds.north);

		xmlNodePtr boundingBox = AddXmlChild(root, "BoundingBox");
		if (!boundingBox ||
			!SetXmlProperty(boundingBox, "miny", FormatMetadataDouble(southMercator)) ||
			!SetXmlProperty(boundingBox, "minx", FormatMetadataDouble(westMercator)) ||
			!SetXmlProperty(boundingBox, "maxy", FormatMetadataDouble(northMercator)) ||
			!SetXmlProperty(boundingBox, "maxx", FormatMetadataDouble(eastMercator)))
		{
			return {};
		}

		xmlNodePtr origin = AddXmlChild(root, "Origin");
		if (!origin ||
			!SetXmlProperty(origin, "y", FormatMetadataDouble(southMercator)) ||
			!SetXmlProperty(origin, "x", FormatMetadataDouble(westMercator)))
		{
			return {};
		}

		xmlNodePtr tileFormat = AddXmlChild(root, "TileFormat");
		if (!tileFormat ||
			!SetXmlProperty(tileFormat, "width", std::to_string(metadata.tileSize)) ||
			!SetXmlProperty(tileFormat, "height", std::to_string(metadata.tileSize)) ||
			!SetXmlProperty(tileFormat, "mime-type", "image/png") ||
			!SetXmlProperty(tileFormat, "extension", "png"))
		{
			return {};
		}

		xmlNodePtr tileSets = AddXmlChild(root, "TileSets");
		if (!tileSets || !SetXmlProperty(tileSets, "profile", "global-mercator"))
		{
			return {};
		}

		double unitsPerPixel = 156543.033928041;
		for (uint32_t zoom = 0; zoom <= metadata.maxZoom; zoom++)
		{
			xmlNodePtr tileSet = AddXmlChild(tileSets, "TileSet");
			if (!tileSet ||
				!SetXmlProperty(tileSet, "units-per-pixel", FormatMetadataDouble(unitsPerPixel)) ||
				!SetXmlProperty(tileSet, "order", std::to_string(zoom)))
			{
				return {};
			}
			unitsPerPixel /= 2.;
		}

		xmlChar* rawBuffer = nullptr;
		int bufferSize = 0;
		xmlDocDumpFormatMemory(document.get(), &rawBuffer, &bufferSize, 1);
		XmlBufferPtr buffer(rawBuffer);
		if (!buffer || bufferSize <= 0)
		{
			return {};
		}

		return std::string(
			reinterpret_cast<const char*>(buffer.get()), static_cast<size_t>(bufferSize));
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

	void ReleaseCurlContext(CURL* easy, DownTool::CurlMultiHandle& context)
	{
		if (context.fileStream.is_open())
		{
			context.fileStream.close();
		}
		if (context.requestHeaders)
		{
			curl_slist_free_all(context.requestHeaders);
			context.requestHeaders = nullptr;
		}
		curl_easy_cleanup(easy);
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
			bool responseHeaderUnavailable = false;
			if (handleIt != curlEasyHandles.end())
			{
				tile = handleIt->second.tile;
				tilePath = handleIt->second.filePath;
				url = handleIt->second.url;
				requestType = handleIt->second.requestType;
				repairTargetZoom = handleIt->second.repairTargetZoom;
				handleIt->second.fileStream.close();
				responseHeaderUnavailable = IsUnavailableTileResponseHeader(
					handleIt->second.responseHeaders, tile);
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
			if (handleIt != curlEasyHandles.end())
			{
				ReleaseCurlContext(easy, handleIt->second);
				curlEasyHandles.erase(handleIt);
			}
			else
			{
				curl_easy_cleanup(easy);
			}
			if (!hasContext)
			{
				continue;
			}

			if (requestType == CurlRequestType::Tile)
			{
				if (msg->data.result == CURLE_OK &&
					(responseHeaderUnavailable || IsUnavailableTileFile(tilePath, tile)))
				{
					if (responseHeaderUnavailable)
					{
						RemoveFileIfExists(tilePath);
					}
					if (!RepairTileFromLowerZoom(tile, tilePath))
					{
						fmt::print("\n无法从低级别瓦片修复占位图: {}\n", PathToUtf8String(tilePath));
					}
				}

				DowningCount += 1;
				hasProgressUpdate = true;
				continue;
			}

			if (msg->data.result == CURLE_OK && responseHeaderUnavailable)
			{
				RemoveFileIfExists(tilePath);
			}

			if (msg->data.result == CURLE_OK &&
				!responseHeaderUnavailable &&
				IsValidImageFile(tilePath) &&
				!IsUnavailableTileFile(tilePath, tile))
			{
				if (RepairTilesFromParent(tile, repairTargetZoom))
				{
					continue;
				}

				fmt::print("\n无法完整批量生成高级别修复瓦片: {}\n", PathToUtf8String(tilePath));
			}

			if (tile.Zoom > GetMinFallbackZoom() && repairTargetZoom > tile.Zoom)
			{
				const Tile retryTile = GetFirstChildTile(tile, repairTargetZoom);
				if (!RepairTileFromLowerZoom(
					retryTile, GetTileFilePath(retryTile), tile.Zoom - 1))
				{
					fmt::print("\n无法继续降级修复占位图: {}\n", PathToUtf8String(tilePath));
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

	for (auto& [easy, context] : curlEasyHandles)
	{
		if (curlMulti)
		{
			curl_multi_remove_handle(curlMulti, easy);
		}
		ReleaseCurlContext(easy, context);
	}
	curlEasyHandles.clear();

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
	else if (!bStop && (PostProcessEnabled || PostProcessMergedImageEnabled ||
		PostProcessFbxEnabled))
	{
		RunPostProcess();
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
	const uint64_t xCount = TileRangeCount(MinTile.X, MaxTile.X);
	const uint64_t yCount = TileRangeCount(MinTile.Y, MaxTile.Y);
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

	for (int32_t xBase = MinTile.X; xBase <= MaxTile.X; xBase++)
	{
		if (bStop) break;
		for (int32_t yBase = MinTile.Y; yBase <= MaxTile.Y; yBase++)
		{
			if (bStop) break;

			if (IsTileOffset())
			{
				const TileRange offsetTiles = GetOffsetTiles(Tile(xBase, yBase, Zoom));
				for (int32_t x = offsetTiles.MinTile.X; x <= offsetTiles.MaxTile.X; x++)
				{
					for (int32_t y = offsetTiles.MinTile.Y; y <= offsetTiles.MaxTile.Y; y++)
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

void DownTool::DownToolBase::BuildRequestedTiles(std::vector<Tile>& targetTiles) const
{
	targetTiles.clear();
	const uint64_t xCount = TileRangeCount(MinTile.X, MaxTile.X);
	const uint64_t yCount = TileRangeCount(MinTile.Y, MaxTile.Y);
	targetTiles.reserve(static_cast<size_t>(xCount * yCount));

	for (int32_t x = MinTile.X; x <= MaxTile.X; x++)
	{
		for (int32_t y = MinTile.Y; y <= MaxTile.Y; y++)
		{
			targetTiles.emplace_back(x, y, Zoom);
		}
	}
}

void DownTool::DownToolBase::BuildRequestedTilesAtZoom(
	uint32_t zoom, std::vector<Tile>& targetTiles) const
{
	targetTiles.clear();
	if (zoom > Zoom)
	{
		return;
	}

	const uint32_t zoomOffset = Zoom - zoom;
	if (zoomOffset >= 31)
	{
		return;
	}

	const uint64_t scale = uint64_t{ 1 } << zoomOffset;
	const int32_t scaleInt = static_cast<int32_t>(scale);
	const int32_t minX = FloorDiv(MinTile.X, scaleInt);
	const int32_t maxX = FloorDiv(MaxTile.X, scaleInt);
	const int32_t minY = FloorDiv(MinTile.Y, scaleInt);
	const int32_t maxY = FloorDiv(MaxTile.Y, scaleInt);
	const uint64_t xCount = TileRangeCount(minX, maxX);
	const uint64_t yCount = TileRangeCount(minY, maxY);
	targetTiles.reserve(static_cast<size_t>(xCount * yCount));

	for (int32_t x = minX; x <= maxX; x++)
	{
		for (int32_t y = minY; y <= maxY; y++)
		{
			targetTiles.emplace_back(x, y, zoom);
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
				PathToUtf8String(GetTileFilePath(tile)));
		}
	}

	if (updateProgress)
	{
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
	}

	return pendingTiles;
}

void DownTool::DownToolBase::DownloadTile(int32_t x, int32_t y)
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
		fmt::print("\n检测缓存文件失败，将重新下载: {}\n", PathToUtf8String(nativePath));
	}
	if (tileExists)
	{
		if (IsUnavailableTileFile(nativePath, tile))
		{
			if (!RepairTileFromLowerZoom(tile, nativePath))
			{
				fmt::print("\n无法从低级别瓦片修复占位图: {}\n", PathToUtf8String(nativePath));
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

	if (RepairTileFromExistingLowerZoom(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
	}

	if (IsCoveredByPendingLowerZoomRepair(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
		return;
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

std::filesystem::path DownTool::DownToolBase::GetPostProcessRootPath() const
{
	if (!PostProcessOutputPath.empty())
	{
		return std::filesystem::u8path(PostProcessOutputPath);
	}

	return std::filesystem::u8path(fmt::format("{}/Google_墨卡托", CachePath));
}

std::filesystem::path DownTool::DownToolBase::GetPostProcessTileFilePath(
	const Tile& tile) const
{
	return GetPostProcessRootPath() /
		std::to_string(tile.Zoom) /
		std::to_string(tile.X) /
		fmt::format("{}.png", tile.Y);
}

std::filesystem::path DownTool::DownToolBase::GetPostProcessMergedImagePath() const
{
	if (!PostProcessMergedImageOutputPath.empty())
	{
		return std::filesystem::u8path(PostProcessMergedImageOutputPath);
	}

	return GetPostProcessRootPath() / "merged.png";
}

std::filesystem::path DownTool::DownToolBase::GetFbxRootPath() const
{
	if (!FbxOutputPath.empty())
	{
		return std::filesystem::u8path(FbxOutputPath);
	}

	return GetPostProcessRootPath();
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
		fmt::print("无法打开文件进行写入，切片将会被跳过，解决文件占用问题后再重新下载一遍即可修复：{}\n", PathToUtf8String(tilePath));
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
	for (const std::string& header : GetRequestHeaders(multiHandle.tile))
	{
		curl_slist* nextHeaders = curl_slist_append(
			multiHandle.requestHeaders, header.c_str());
		if (!nextHeaders)
		{
			fmt::print("无法创建 CURL 请求头: {}\n", header);
			ReleaseCurlContext(easy, multiHandle);
			return false;
		}
		multiHandle.requestHeaders = nextHeaders;
	}
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
	curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, WriteTileHeaderData);
	curl_easy_setopt(easy, CURLOPT_HEADERDATA, &curlEasyHandles[easy]);
    if (!Proxy.empty())
    {
        curl_easy_setopt(easy, CURLOPT_PROXY, Proxy.c_str());
    }
	if (curlEasyHandles[easy].requestHeaders)
	{
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, curlEasyHandles[easy].requestHeaders);
	}
	if (curl_multi_add_handle(curlMulti, easy) != CURLM_OK)
	{
		fmt::print("无法将 CURL easy handle 添加到 multi handle\n");
		ReleaseCurlContext(easy, curlEasyHandles[easy]);
		curlEasyHandles.erase(easy);
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

bool DownTool::DownToolBase::IsCoveredByPendingLowerZoomRepair(const Tile& tile) const
{
	for (const auto& [easy, context] : curlEasyHandles)
	{
		(void)easy;
		if (context.requestType == CurlRequestType::RepairParent &&
			context.repairTargetZoom == tile.Zoom &&
			TileInParentRange(context.tile, tile, tile.Zoom))
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

	std::vector<CURL*> handlesToCancel;
	for (const auto& [easy, context] : curlEasyHandles)
	{
		if (context.requestType == CurlRequestType::Tile &&
			TileInParentRange(parentTile, context.tile, childZoom))
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

		curl_multi_remove_handle(curlMulti, easy);
		ReleaseCurlContext(easy, handleIt->second);
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

		const int32_t scale = static_cast<int32_t>(1u << zoomOffset);
		const Tile parentTile(
			FloorDiv(tile.X, scale),
			FloorDiv(tile.Y, scale),
			parentZoom);
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

bool DownTool::DownToolBase::RepairTileFromExistingLowerZoom(const Tile& tile)
{
	if (tile.Zoom == 0)
	{
		return false;
	}

	return RepairTileFromExistingLowerZoom(tile, tile.Zoom - 1);
}

bool DownTool::DownToolBase::RepairTileFromExistingLowerZoom(
	const Tile& tile,
	uint32_t startParentZoom)
{
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

		const int32_t scale = static_cast<int32_t>(1u << zoomOffset);
		const Tile parentTile(
			FloorDiv(tile.X, scale),
			FloorDiv(tile.Y, scale),
			parentZoom);
		const std::filesystem::path parentPath = GetTileFilePath(parentTile);
		std::error_code parentExistsError;
		const bool parentExists = std::filesystem::exists(parentPath, parentExistsError);
		if (!parentExistsError &&
			parentExists &&
			IsValidImageFile(parentPath) &&
			!IsUnavailableTileFile(parentPath, parentTile) &&
			RepairTilesFromParent(parentTile, tile.Zoom))
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

	auto writeRepairedTile = [&](const Tile& childTile, int32_t childX, int32_t childY)
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
	for (int32_t childX = 0; childX < static_cast<int32_t>(scale); childX++)
	{
		for (int32_t childY = 0; childY < static_cast<int32_t>(scale); childY++)
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
	ParallelForIndex(
		repairTasks.size(),
		MaxDownQueue,
		[&](size_t taskIndex)
		{
			const TileRepairTask& task = repairTasks[taskIndex];
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
		});

	const uint32_t readyTileCount = atomicReadyTileCount.load(std::memory_order_relaxed);
	const uint32_t repairedTileCount =
		atomicRepairedTileCount.load(std::memory_order_relaxed);

	fmt::print("\n使用低级别瓦片批量修复占位图: z{} ({}, {}) -> z{}，可用 {} / {} 张，写入 {} 张\n",
		parentTile.Zoom, parentTile.X, parentTile.Y, targetZoom,
		readyTileCount, batchTileCount, repairedTileCount);
	return readyTileCount == batchTileCount;
}

void DownTool::DownToolBase::RunPostProcess()
{
	const auto postProcessStartTime = std::chrono::steady_clock::now();
	std::vector<Tile> targetTiles;
	BuildRequestedTiles(targetTiles);
	if (targetTiles.empty())
	{
		fmt::print("后处理跳过：没有可处理的目标瓦片。\n");
		return;
	}

	fmt::print("开始瓦片后处理，输出目录：{}\n", PathToUtf8String(GetPostProcessRootPath()));
	bool maxZoomTilesReady = false;
	if (PostProcessEnabled || PostProcessFbxEnabled)
	{
		auto phaseStartTime = std::chrono::steady_clock::now();
		maxZoomTilesReady = GeneratePostProcessMaxZoomTiles(targetTiles);
		fmt::print("最高层级后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	if (PostProcessEnabled)
	{
		if (maxZoomTilesReady)
		{
			auto phaseStartTime = std::chrono::steady_clock::now();
			GeneratePostProcessLowerZoomTiles(targetTiles);
			fmt::print("低层级金字塔后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
			phaseStartTime = std::chrono::steady_clock::now();
			if (!WritePostProcessMetadata())
			{
				fmt::print("后处理警告：元数据写入失败。\n");
			}
			fmt::print("元数据写入耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
		}
		else
		{
			fmt::print("后处理警告：最高层级瓦片生成不完整，跳过低层级和元数据生成。\n");
		}
	}

	if (PostProcessMergedImageEnabled)
	{
		const auto phaseStartTime = std::chrono::steady_clock::now();
		const bool postProcessTilesAvailable = PostProcessEnabled && maxZoomTilesReady;
		if (!GeneratePostProcessMergedImage(postProcessTilesAvailable))
		{
			fmt::print("后处理警告：合并大图生成失败或不完整。\n");
		}
		fmt::print("合并大图后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	if (PostProcessFbxEnabled)
	{
		const auto phaseStartTime = std::chrono::steady_clock::now();
		if (!GeneratePostProcessFbx(maxZoomTilesReady))
		{
			fmt::print("后处理警告：FBX 导出失败或不完整。\n");
		}
		fmt::print("FBX 后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	fmt::print("瓦片后处理完成，总耗时：{:.2f}s\n", SecondsSince(postProcessStartTime));
}

bool DownTool::DownToolBase::GeneratePostProcessMaxZoomTiles(
	const std::vector<Tile>& targetTiles)
{
	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t failedCount = 0;

	ParallelForIndex(
		targetTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = targetTiles[index];
			const std::filesystem::path outputPath = GetPostProcessTileFilePath(tile);
			bool writeSuccess = false;
			try
			{
				writeSuccess = WritePostProcessMaxZoomTile(tile, outputPath);
			}
			catch (const std::exception&)
			{
				writeSuccess = false;
			}
			catch (...)
			{
				writeSuccess = false;
			}

			if (!writeSuccess)
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
			}
			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 1024 == 0 || currentCount == targetTiles.size())
			{
				fmt::print("\r最高层级后处理: {}/{}",
					currentCount, targetTiles.size());
			}
		});

	fmt::print("\r最高层级后处理: {}/{}，失败 {}\n",
		targetTiles.size(),
		targetTiles.size(),
		failedCount.load());
	return failedCount.load() == 0;
}

uint32_t DownTool::DownToolBase::SelectPostProcessMergedImageSampleZoom() const
{
	const double targetResolution =
		static_cast<double>(std::max(1u, PostProcessMergedImageResolution));
	uint32_t bestZoom = Zoom;
	double bestScore = std::numeric_limits<double>::max();

	for (uint32_t zoom = 0; zoom <= Zoom; zoom++)
	{
		const uint32_t zoomOffset = Zoom - zoom;
		if (zoomOffset >= 31)
		{
			continue;
		}

		const uint64_t scale = uint64_t{ 1 } << zoomOffset;
		const int32_t scaleInt = static_cast<int32_t>(scale);
		const int32_t minX = FloorDiv(MinTile.X, scaleInt);
		const int32_t maxX = FloorDiv(MaxTile.X, scaleInt);
		const int32_t minY = FloorDiv(MinTile.Y, scaleInt);
		const int32_t maxY = FloorDiv(MaxTile.Y, scaleInt);
		const uint64_t xCount = TileRangeCount(minX, maxX);
		const uint64_t yCount = TileRangeCount(minY, maxY);
		const double sourcePixels =
			static_cast<double>(std::max(xCount, yCount)) * TileSize;
		if (sourcePixels <= 0.)
		{
			continue;
		}

		const double score = std::abs(std::log(sourcePixels / targetResolution));
		if (score < bestScore || (score == bestScore && zoom < bestZoom))
		{
			bestScore = score;
			bestZoom = zoom;
		}
	}

	return bestZoom;
}

bool DownTool::DownToolBase::GeneratePostProcessTilesAtZoom(
	const std::vector<Tile>& targetTiles,
	uint32_t sourceZoom,
	const std::string& phaseName) const
{
	if (targetTiles.empty())
	{
		return true;
	}

	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t failedCount = 0;

	ParallelForIndex(
		targetTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = targetTiles[index];
			const std::filesystem::path outputPath = GetPostProcessTileFilePath(tile);
			bool writeSuccess = false;
			try
			{
				writeSuccess =
					WritePostProcessTileFromSourceZoom(tile, outputPath, sourceZoom);
			}
			catch (const std::exception&)
			{
				writeSuccess = false;
			}
			catch (...)
			{
				writeSuccess = false;
			}

			if (!writeSuccess)
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
			}
			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 1024 == 0 || currentCount == targetTiles.size())
			{
				fmt::print("\r{}: {}/{}",
					phaseName, currentCount, targetTiles.size());
			}
		});

	fmt::print("\r{}: {}/{}，失败 {}\n",
		phaseName,
		targetTiles.size(),
		targetTiles.size(),
		failedCount.load());
	return failedCount.load() == 0;
}

void DownTool::DownToolBase::GeneratePostProcessLowerZoomTiles(
	std::vector<Tile> currentLevelTiles)
{
	while (!currentLevelTiles.empty() && currentLevelTiles.front().Zoom > 0)
	{
		const std::unordered_set<Tile, TileHash, TileEqual> currentLevelTileSet(
			currentLevelTiles.begin(), currentLevelTiles.end());
		std::vector<Tile> parentTiles;
		parentTiles.reserve(currentLevelTiles.size() / 4 + 1);
		std::unordered_set<Tile, TileHash, TileEqual> uniqueParents;
		uniqueParents.reserve(currentLevelTiles.size() / 4 + 1);

		for (const Tile& childTile : currentLevelTiles)
		{
			Tile parentTile(
				FloorDiv(childTile.X, 2),
				FloorDiv(childTile.Y, 2),
				childTile.Zoom - 1);
			if (uniqueParents.insert(parentTile).second)
			{
				parentTiles.push_back(parentTile);
			}
		}

		std::atomic_uint32_t writtenCount = 0;
		ParallelForIndex(
			parentTiles.size(),
			MaxDownQueue,
			[&](size_t index)
			{
				const int tileSize = static_cast<int>(TileSize);
				const Tile& parentTile = parentTiles[index];
				cv::Mat mergeImage(
					tileSize * 2, tileSize * 2, CV_8UC4, cv::Scalar(0, 0, 0, 0));
				bool hasChildImage = false;

				for (int32_t childX = 0; childX < 2; childX++)
				{
					for (int32_t childY = 0; childY < 2; childY++)
					{
						const Tile childTile(
							parentTile.X * 2 + childX,
							parentTile.Y * 2 + childY,
							parentTile.Zoom + 1);
						if (!currentLevelTileSet.contains(childTile))
						{
							continue;
						}
						const cv::Mat childImage =
							ReadBgraTile(GetPostProcessTileFilePath(childTile), TileSize);
						if (childImage.empty())
						{
							continue;
						}

						childImage.copyTo(mergeImage(cv::Rect(
							static_cast<int>(childX) * tileSize,
							static_cast<int>(childY) * tileSize,
							tileSize,
							tileSize)));
						hasChildImage = true;
					}
				}

				if (!hasChildImage)
				{
					return;
				}

				cv::Mat targetImage;
				cv::resize(
					mergeImage,
					targetImage,
					cv::Size(tileSize, tileSize),
					0.,
					0.,
					cv::INTER_AREA);
				if (WriteImage(GetPostProcessTileFilePath(parentTile), targetImage))
				{
					writtenCount.fetch_add(1, std::memory_order_relaxed);
				}
			});

		fmt::print("生成低层级 z{}：写入 {}，总 {} 张\n",
			parentTiles.front().Zoom,
			writtenCount.load(),
			parentTiles.size());
		currentLevelTiles = std::move(parentTiles);
	}
}

cv::Mat DownTool::DownToolBase::ReadPostProcessMaxZoomTile(
	const Tile& targetTile) const
{
	const int tileSize = static_cast<int>(TileSize);
	return ReadPostProcessTileRegion(targetTile, tileSize, tileSize);
}

cv::Mat DownTool::DownToolBase::ReadPostProcessTileRegion(
	const Tile& targetTile,
	int outputWidth,
	int outputHeight) const
{
	return ReadPostProcessTileRegion(targetTile, targetTile.Zoom, outputWidth, outputHeight);
}

cv::Mat DownTool::DownToolBase::ReadPostProcessTileRegion(
	const Tile& targetTile,
	uint32_t sourceZoom,
	int outputWidth,
	int outputHeight) const
{
	if (sourceZoom < targetTile.Zoom)
	{
		return {};
	}

	if (CanCopyPostProcessTileDirectly() && sourceZoom == targetTile.Zoom)
	{
		const std::filesystem::path sourcePath =
			GetPostProcessSourceTileFilePath(targetTile);
		if (!IsValidImageFile(sourcePath) || IsUnavailableTileFile(sourcePath, targetTile))
		{
			return {};
		}

		return ResizeImageToSize(ReadBgraTile(sourcePath, TileSize), outputWidth, outputHeight);
	}

	const int tileSize = static_cast<int>(TileSize);
	if (tileSize <= 0 || outputWidth <= 0 || outputHeight <= 0)
	{
		return {};
	}

	std::vector<TileSample> samples(
		static_cast<size_t>(outputWidth) * static_cast<size_t>(outputHeight));
	std::unordered_set<Tile, TileHash, TileEqual> sourceTiles;
	sourceTiles.reserve(8);

	for (int y = 0; y < outputHeight; y++)
	{
		for (int x = 0; x < outputWidth; x++)
		{
			const double targetPixelX =
				(static_cast<double>(x) + 0.5) * tileSize / outputWidth;
			const double targetPixelY =
				(static_cast<double>(y) + 0.5) * tileSize / outputHeight;
			LonLat targetLonLat;
			if (!TryGetLonLatForTilePixel(
				targetTile, targetPixelX, targetPixelY, targetLonLat))
			{
				continue;
			}

			const LonLat sourceLonLat = ConvertTargetLonLatToSourceLonLat(targetLonLat);
			Tile sourceTile;
			double sourcePixelX = 0.;
			double sourcePixelY = 0.;
			if (!TryGetSourceTilePixelForLonLat(
				sourceLonLat, sourceZoom, sourceTile, sourcePixelX, sourcePixelY))
			{
				continue;
			}

			const size_t sampleIndex = static_cast<size_t>(y * outputWidth + x);
			samples[sampleIndex] = TileSample{
				sourceTile,
				sourcePixelX,
				sourcePixelY,
				true
			};
			sourceTiles.insert(sourceTile);
		}
	}

	if (sourceTiles.empty())
	{
		return {};
	}

	std::unordered_map<Tile, cv::Mat, TileHash, TileEqual> sourceImages;
	sourceImages.reserve(sourceTiles.size());
	for (const Tile& sourceTile : sourceTiles)
	{
		const std::filesystem::path sourcePath =
			GetPostProcessSourceTileFilePath(sourceTile);
		if (!IsValidImageFile(sourcePath) || IsUnavailableTileFile(sourcePath, sourceTile))
		{
			continue;
		}

		cv::Mat sourceImage = ReadBgraTile(sourcePath, TileSize);
		if (!sourceImage.empty())
		{
			sourceImages.emplace(sourceTile, std::move(sourceImage));
		}
	}

	if (sourceImages.empty())
	{
		return {};
	}

	cv::Mat outputImage(outputHeight, outputWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));
	bool hasPixel = false;
	for (int y = 0; y < outputHeight; y++)
	{
		for (int x = 0; x < outputWidth; x++)
		{
			const TileSample& sample = samples[static_cast<size_t>(y * outputWidth + x)];
			if (!sample.valid)
			{
				continue;
			}

			const auto imageIt = sourceImages.find(sample.sourceTile);
			if (imageIt == sourceImages.end())
			{
				continue;
			}

			outputImage.at<cv::Vec4b>(y, x) =
				SampleBilinear(imageIt->second, sample.pixelX, sample.pixelY);
			hasPixel = true;
		}
	}

	if (!hasPixel)
	{
		return {};
	}

	return outputImage;
}

bool DownTool::DownToolBase::GeneratePostProcessMergedImage(
	bool postProcessTilesAvailable) const
{
	const auto totalStartTime = std::chrono::steady_clock::now();
	const uint32_t targetMaxResolution = PostProcessMergedImageResolution;
	if (targetMaxResolution == 0 ||
		targetMaxResolution > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		fmt::print("合并大图失败：分辨率参数无效。\n");
		return false;
	}

	const uint64_t xCount = TileRangeCount(MinTile.X, MaxTile.X);
	const uint64_t yCount = TileRangeCount(MinTile.Y, MaxTile.Y);
	if (xCount == 0 || yCount == 0)
	{
		fmt::print("合并大图失败：瓦片范围为空。\n");
		return false;
	}

	const MergedImageOutputSize outputSize = CalculateMergedImageOutputSize(
		xCount,
		yCount,
		TileSize,
		targetMaxResolution);
	if (outputSize.width == 0 || outputSize.height == 0 ||
		outputSize.width > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		outputSize.height > static_cast<uint32_t>(std::numeric_limits<int>::max()))
	{
		fmt::print("合并大图失败：输出尺寸计算失败。\n");
		return false;
	}

	const uint32_t sampleZoom = SelectPostProcessMergedImageSampleZoom();
	std::vector<Tile> sampleTiles;
	BuildRequestedTilesAtZoom(sampleZoom, sampleTiles);
	if (sampleTiles.empty())
	{
		fmt::print("合并大图失败：采样层级瓦片范围为空。\n");
		return false;
	}

	fmt::print("合并大图采样层级：z{}，瓦片数 {}，目标最大边 {}，输出分辨率 {}x{}\n",
		sampleZoom,
		sampleTiles.size(),
		targetMaxResolution,
		outputSize.width,
		outputSize.height);

	const std::filesystem::path outputPath = GetPostProcessMergedImagePath();
	const uint32_t zoomOffset = Zoom - sampleZoom;
	const int64_t sampleScale = static_cast<int64_t>(uint64_t{ 1 } << zoomOffset);
	const int64_t requestMinX = MinTile.X;
	const int64_t requestMinY = MinTile.Y;
	const int64_t requestMaxX = static_cast<int64_t>(MaxTile.X) + 1;
	const int64_t requestMaxY = static_cast<int64_t>(MaxTile.Y) + 1;
	const int tileSize = static_cast<int>(TileSize);

	auto isSampleTileFullyCovered = [&](const Tile& tile)
	{
		const int64_t sampleMinX = static_cast<int64_t>(tile.X) * sampleScale;
		const int64_t sampleMinY = static_cast<int64_t>(tile.Y) * sampleScale;
		const int64_t sampleMaxX = sampleMinX + sampleScale;
		const int64_t sampleMaxY = sampleMinY + sampleScale;
		return sampleMinX >= requestMinX &&
			sampleMinY >= requestMinY &&
			sampleMaxX <= requestMaxX &&
			sampleMaxY <= requestMaxY;
	};

	std::vector<Tile> generatedSampleTiles;
	generatedSampleTiles.reserve(sampleTiles.size());
	for (const Tile& tile : sampleTiles)
	{
		if (isSampleTileFullyCovered(tile))
		{
			generatedSampleTiles.push_back(tile);
		}
	}

	fmt::print("大图采样层生成瓦片：{} / {}\n",
		generatedSampleTiles.size(), sampleTiles.size());
	auto phaseStartTime = std::chrono::steady_clock::now();
	if (postProcessTilesAvailable)
	{
		fmt::print("大图采样层使用本次生成的金字塔瓦片。\n");
	}
	else
	{
		phaseStartTime = std::chrono::steady_clock::now();
		if (!GeneratePostProcessTilesAtZoom(
			generatedSampleTiles, Zoom, "大图采样层后处理"))
		{
			return false;
		}
		fmt::print("大图采样层后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	const int outputWidth = static_cast<int>(outputSize.width);
	const int outputHeight = static_cast<int>(outputSize.height);
	const double imageMemoryMb =
		static_cast<double>(outputSize.width) * outputSize.height * 4. / 1024. / 1024.;
	fmt::print("合并大图内存估算：{:.1f} MB\n", imageMemoryMb);
	phaseStartTime = std::chrono::steady_clock::now();
	cv::Mat mergedImage(
		outputHeight, outputWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));
	fmt::print("合并大图内存分配耗时：{:.2f}s\n", SecondsSince(phaseStartTime));

	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t failedCount = 0;

	phaseStartTime = std::chrono::steady_clock::now();
	ParallelForIndex(
		sampleTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = sampleTiles[index];
			const int64_t sampleMinX = static_cast<int64_t>(tile.X) * sampleScale;
			const int64_t sampleMinY = static_cast<int64_t>(tile.Y) * sampleScale;
			const int64_t sampleMaxX = sampleMinX + sampleScale;
			const int64_t sampleMaxY = sampleMinY + sampleScale;
			const int64_t overlapMinX = std::max(sampleMinX, requestMinX);
			const int64_t overlapMinY = std::max(sampleMinY, requestMinY);
			const int64_t overlapMaxX = std::min(sampleMaxX, requestMaxX);
			const int64_t overlapMaxY = std::min(sampleMaxY, requestMaxY);

			if (overlapMaxX <= overlapMinX || overlapMaxY <= overlapMinY)
			{
				const uint32_t currentCount =
					processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
				{
					fmt::print("\r大图合并: {}/{}",
						currentCount, sampleTiles.size());
				}
				return;
			}

			const int x0 = static_cast<int>(
				((overlapMinX - requestMinX) * outputSize.width) / xCount);
			const int x1 = static_cast<int>(
				((overlapMaxX - requestMinX) * outputSize.width) / xCount);
			const int y0 = static_cast<int>(
				((overlapMinY - requestMinY) * outputSize.height) / yCount);
			const int y1 = static_cast<int>(
				((overlapMaxY - requestMinY) * outputSize.height) / yCount);

			if (x1 <= x0 || y1 <= y0)
			{
				const uint32_t currentCount =
					processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
				{
					fmt::print("\r大图合并: {}/{}",
						currentCount, sampleTiles.size());
				}
				return;
			}

			const bool useGeneratedTile =
				postProcessTilesAvailable || isSampleTileFullyCovered(tile);
			const cv::Mat tileImage = useGeneratedTile ?
				ReadBgraTile(GetPostProcessTileFilePath(tile), TileSize) :
				ReadPostProcessTileRegion(tile, Zoom, tileSize, tileSize);
			if (tileImage.empty())
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
				const uint32_t currentCount =
					processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
				if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
				{
					fmt::print("\r大图合并: {}/{}",
						currentCount, sampleTiles.size());
				}
				return;
			}

			const double cropX0 =
				static_cast<double>(overlapMinX - sampleMinX) *
				tileSize / static_cast<double>(sampleScale);
			const double cropY0 =
				static_cast<double>(overlapMinY - sampleMinY) *
				tileSize / static_cast<double>(sampleScale);
			const double cropX1 =
				static_cast<double>(overlapMaxX - sampleMinX) *
				tileSize / static_cast<double>(sampleScale);
			const double cropY1 =
				static_cast<double>(overlapMaxY - sampleMinY) *
				tileSize / static_cast<double>(sampleScale);
			const int cropLeft =
				std::clamp(static_cast<int>(std::floor(cropX0)), 0, tileSize - 1);
			const int cropTop =
				std::clamp(static_cast<int>(std::floor(cropY0)), 0, tileSize - 1);
			const int cropRight =
				std::clamp(static_cast<int>(std::ceil(cropX1)), cropLeft + 1, tileSize);
			const int cropBottom =
				std::clamp(static_cast<int>(std::ceil(cropY1)), cropTop + 1, tileSize);
			const cv::Mat croppedTile = tileImage(cv::Rect(
				cropLeft,
				cropTop,
				cropRight - cropLeft,
				cropBottom - cropTop));
			cv::Mat targetRoi = mergedImage(cv::Rect(x0, y0, x1 - x0, y1 - y0));
			const cv::InterpolationFlags interpolation =
				(croppedTile.cols > targetRoi.cols || croppedTile.rows > targetRoi.rows) ?
				cv::INTER_AREA : cv::INTER_LINEAR;
			cv::resize(croppedTile, targetRoi, targetRoi.size(), 0., 0., interpolation);

			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
			{
				fmt::print("\r大图合并: {}/{}",
					currentCount, sampleTiles.size());
			}
		});

	fmt::print("\r大图合并: {}/{}，失败 {}\n",
		sampleTiles.size(), sampleTiles.size(), failedCount.load());
	fmt::print("大图像素合并耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	if (failedCount.load() != 0)
	{
		return false;
	}

	phaseStartTime = std::chrono::steady_clock::now();
	const bool writeSuccess = WriteImage(outputPath, mergedImage);
	fmt::print("合并大图写入耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	if (writeSuccess)
	{
		fmt::print("合并大图输出：{}\n", PathToUtf8String(outputPath));
	}
	fmt::print("合并大图函数总耗时：{:.2f}s\n", SecondsSince(totalStartTime));
	return writeSuccess;
}

bool DownTool::DownToolBase::GeneratePostProcessFbx(bool postProcessTilesAvailable) const
{
	const auto totalStartTime = std::chrono::steady_clock::now();
	if (!postProcessTilesAvailable)
	{
		fmt::print("FBX 导出失败：最高层级后处理瓦片不可用。\n");
		return false;
	}

	if (!IsPowerOfTwo(FbxTextureSize) || FbxTextureSize > 16384 ||
		FbxTextureSize < TileSize || FbxTextureSize % TileSize != 0)
	{
		fmt::print(
			"FBX 导出失败：贴图尺寸必须是 2 的次幂、不能超过 16384，且必须是瓦片尺寸的整数倍。\n");
		return false;
	}

	Tile centerTile;
	double centerTilePixelX = 0.;
	double centerTilePixelY = 0.;
	if (!TryGetTilePixelForLonLat(
		CenterLonLat, Zoom, centerTile, centerTilePixelX, centerTilePixelY))
	{
		fmt::print("FBX 导出失败：无法把项目中心转换为瓦片像素坐标。\n");
		return false;
	}

	const double tileSize = static_cast<double>(TileSize);
	const double centerPixelX = static_cast<double>(centerTile.X) * tileSize +
		centerTilePixelX;
	const double centerPixelY = static_cast<double>(centerTile.Y) * tileSize +
		centerTilePixelY;
	const FbxGlobalPixelBounds requestBounds{
		static_cast<double>(MinTile.X) * tileSize,
		static_cast<double>(MinTile.Y) * tileSize,
		(static_cast<double>(MaxTile.X) + 1.) * tileSize,
		(static_cast<double>(MaxTile.Y) + 1.) * tileSize
	};

	const double textureSize = static_cast<double>(FbxTextureSize);
	const int32_t minGridX = static_cast<int32_t>(
		std::floor((requestBounds.minX - centerPixelX - textureSize / 2.) /
			textureSize)) - 1;
	const int32_t maxGridX = static_cast<int32_t>(
		std::ceil((requestBounds.maxX - centerPixelX + textureSize / 2.) /
			textureSize)) + 1;
	const int32_t minGridY = static_cast<int32_t>(
		std::floor((requestBounds.minY - centerPixelY - textureSize / 2.) /
			textureSize)) - 1;
	const int32_t maxGridY = static_cast<int32_t>(
		std::ceil((requestBounds.maxY - centerPixelY + textureSize / 2.) /
			textureSize)) + 1;

	const std::filesystem::path outputRoot = GetFbxRootPath();
	const std::filesystem::path textureRoot =
		outputRoot / "textures" / "fbx" / std::to_string(FbxTextureSize);
	const FbxCoordinateContext coordinateContext =
		BuildFbxCoordinateContext(CenterLonLat);

	std::vector<FbxTextureOutputInfo> textureInfos;
	textureInfos.reserve(static_cast<size_t>(std::max(0, maxGridX - minGridX + 1)) *
		static_cast<size_t>(std::max(0, maxGridY - minGridY + 1)));

	for (int32_t gridY = minGridY; gridY <= maxGridY; gridY++)
	{
		for (int32_t gridX = minGridX; gridX <= maxGridX; gridX++)
		{
			const double minPixelX =
				centerPixelX + static_cast<double>(gridX) * textureSize -
				textureSize / 2.;
			const double minPixelY =
				centerPixelY + static_cast<double>(gridY) * textureSize -
				textureSize / 2.;
			const FbxGlobalPixelBounds textureBounds{
				minPixelX,
				minPixelY,
				minPixelX + textureSize,
				minPixelY + textureSize
			};
			if (textureBounds.maxX <= requestBounds.minX ||
				textureBounds.maxY <= requestBounds.minY ||
				textureBounds.minX >= requestBounds.maxX ||
				textureBounds.minY >= requestBounds.maxY)
			{
				continue;
			}

			Tile northWestTile;
			double northWestPixelX = 0.;
			double northWestPixelY = 0.;
			northWestTile = PixelBoundsToTile(
				textureBounds.minX, textureBounds.minY, Zoom, TileSize);
			northWestPixelX = textureBounds.minX -
				static_cast<double>(northWestTile.X) * tileSize;
			northWestPixelY = textureBounds.minY -
				static_cast<double>(northWestTile.Y) * tileSize;

			Tile southEastTile;
			double southEastPixelX = 0.;
			double southEastPixelY = 0.;
			southEastTile = PixelBoundsToTile(
				std::nextafter(textureBounds.maxX, textureBounds.minX),
				std::nextafter(textureBounds.maxY, textureBounds.minY),
				Zoom,
				TileSize);
			southEastPixelX = textureBounds.maxX -
				static_cast<double>(southEastTile.X) * tileSize;
			southEastPixelY = textureBounds.maxY -
				static_cast<double>(southEastTile.Y) * tileSize;

			LonLat northWestLonLat;
			LonLat southEastLonLat;
			if (!TryGetLonLatForTilePixel(
				northWestTile,
				northWestPixelX,
				northWestPixelY,
				northWestLonLat) ||
				!TryGetLonLatForTilePixel(
					southEastTile,
					southEastPixelX,
					southEastPixelY,
					southEastLonLat))
			{
				continue;
			}

			const double centerMercatorX =
				(LongitudeToWebMercatorX(northWestLonLat.lon) +
					LongitudeToWebMercatorX(southEastLonLat.lon)) /
				2.;
			const double centerMercatorY =
				(LatitudeToWebMercatorY(northWestLonLat.lat) +
					LatitudeToWebMercatorY(southEastLonLat.lat)) /
				2.;
			const LonLat textureCenterLonLat =
				WebMercatorToLonLat(centerMercatorX, centerMercatorY);
			const FbxMeshVertex textureCenterPosition =
				MakePlaneVertex(textureCenterLonLat, coordinateContext, 0.5, 0.5);
			const double distanceCm = std::hypot(
				textureCenterPosition.x, textureCenterPosition.y);
			const uint32_t distanceOffset =
				SelectFbxDistanceOffset(distanceCm, FbxTextureDistanceOffsets);
			const uint32_t outputTextureSize =
				CalculateFbxOutputTextureSize(FbxTextureSize, distanceOffset);
			const std::string textureName = MakeFbxTextureName(gridX, gridY);
			const int32_t minTileX = static_cast<int32_t>(std::max<double>(
				static_cast<double>(MinTile.X),
				std::floor(textureBounds.minX / tileSize)));
			const int32_t minTileY = static_cast<int32_t>(std::max<double>(
				static_cast<double>(MinTile.Y),
				std::floor(textureBounds.minY / tileSize)));
			const int32_t maxTileX = static_cast<int32_t>(std::min<double>(
				static_cast<double>(MaxTile.X),
				std::floor(std::nextafter(textureBounds.maxX, textureBounds.minX) /
					tileSize)));
			const int32_t maxTileY = static_cast<int32_t>(std::min<double>(
				static_cast<double>(MaxTile.Y),
				std::floor(std::nextafter(textureBounds.maxY, textureBounds.minY) /
					tileSize)));

			FbxTextureOutputInfo textureInfo;
			textureInfo.gridX = gridX;
			textureInfo.gridY = gridY;
			textureInfo.ring = CalculateRing(gridX, gridY);
			textureInfo.zoom = Zoom;
			textureInfo.tileSize = TileSize;
			textureInfo.sourceTextureSize = FbxTextureSize;
			textureInfo.outputTextureSize = outputTextureSize;
			textureInfo.distanceOffset = distanceOffset;
			textureInfo.centerTileX = centerTile.X;
			textureInfo.centerTileY = centerTile.Y;
			textureInfo.centerPixelX = centerPixelX;
			textureInfo.centerPixelY = centerPixelY;
			textureInfo.distanceCm = distanceCm;
			textureInfo.partial =
				textureBounds.minX < requestBounds.minX ||
				textureBounds.minY < requestBounds.minY ||
				textureBounds.maxX > requestBounds.maxX ||
				textureBounds.maxY > requestBounds.maxY;
			textureInfo.pixelBounds = textureBounds;
			textureInfo.tileRange = TileRange(
				Tile(minTileX, minTileY, Zoom),
				Tile(maxTileX, maxTileY, Zoom));
			textureInfo.bounds = BoundsFromCorners(northWestLonLat, southEastLonLat);
			textureInfo.texturePath = textureRoot / fmt::format("{}.png", textureName);
			textureInfo.metadataPath = textureRoot / fmt::format("{}.json", textureName);
			textureInfos.push_back(std::move(textureInfo));
		}
	}

	std::sort(
		textureInfos.begin(),
		textureInfos.end(),
		[](const FbxTextureOutputInfo& left, const FbxTextureOutputInfo& right)
		{
			if (left.ring != right.ring)
			{
				return left.ring < right.ring;
			}
			if (left.gridY != right.gridY)
			{
				return left.gridY < right.gridY;
			}
			return left.gridX < right.gridX;
		});

	if (textureInfos.empty())
	{
		fmt::print("FBX 导出失败：没有任何贴图与下载范围相交。\n");
		return false;
	}

	fmt::print("FBX 贴图生成：{} 张，贴图尺寸 {}，输出目录 {}\n",
		textureInfos.size(),
		FbxTextureSize,
		PathToUtf8String(textureRoot));

	auto readTileImage = [&](const Tile& tile)
	{
		return ReadBgraTile(GetPostProcessTileFilePath(tile), TileSize);
	};

	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t failedCount = 0;
	const uint32_t fbxTextureWorkerCount =
		GetFbxTextureWorkerCount(FbxTextureSize, MaxDownQueue);
	fmt::print("FBX 贴图生成 worker：{}\n", fbxTextureWorkerCount);
	ParallelForIndex(
		textureInfos.size(),
		fbxTextureWorkerCount,
		[&](size_t index)
		{
			const FbxTextureOutputInfo& textureInfo = textureInfos[index];
			const std::string metadata =
				BuildFbxTextureMetadataJson(textureInfo, textureRoot);
			bool success = WriteFbxTextureImage(
				textureInfo,
				requestBounds,
				TileSize,
				readTileImage);
			if (success && !WriteTextFile(textureInfo.metadataPath, metadata))
			{
				success = false;
			}

			if (!success)
			{
				failedCount.fetch_add(1, std::memory_order_relaxed);
			}
			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			if (currentCount % 64 == 0 || currentCount == textureInfos.size())
			{
				fmt::print("\rFBX 贴图生成: {}/{}",
					currentCount,
					textureInfos.size());
			}
		});
	fmt::print("\rFBX 贴图生成: {}/{}，失败 {}\n",
		textureInfos.size(),
		textureInfos.size(),
		failedCount.load());
	if (failedCount.load() != 0)
	{
		return false;
	}

	if (!WriteTextFile(
		textureRoot / "manifest.json",
		BuildFbxManifestJson(
			textureInfos,
			textureRoot,
			FbxShape == FbxPlanetShape::Plane ? "plane" :
				FbxShape == FbxPlanetShape::Sphere ? "sphere" : "both",
			FbxTextureSize,
			Zoom,
			TileSize)))
	{
		fmt::print("FBX 警告：manifest.json 写入失败。\n");
	}

	auto buildMeshDatas = [&](FbxPlanetShape shape)
	{
		const uint32_t subdivision =
			shape == FbxPlanetShape::Sphere ? FbxMeshSubdivision : 1u;
		std::vector<FbxMeshData> meshDatas;
		meshDatas.reserve(textureInfos.size());
		const std::vector<uint32_t> indices = BuildFbxMeshIndices(subdivision);
		for (const FbxTextureOutputInfo& textureInfo : textureInfos)
		{
			FbxMeshData meshData;
			meshData.name = MakeFbxMeshName(textureInfo.gridX, textureInfo.gridY);
			meshData.texturePath = textureInfo.texturePath;
			meshData.vertices = BuildFbxMeshVertices(
				textureInfo,
				coordinateContext,
				shape,
				subdivision);
			meshData.indices = indices;
			meshDatas.push_back(std::move(meshData));
		}
		return meshDatas;
	};

	bool meshSuccess = true;
	if (FbxShape == FbxPlanetShape::Plane || FbxShape == FbxPlanetShape::Both)
	{
		const std::filesystem::path planePath = outputRoot / "mesh" / "plane" / "tiles.fbx";
		const auto phaseStartTime = std::chrono::steady_clock::now();
		meshSuccess = ExportAssimpFbx(buildMeshDatas(FbxPlanetShape::Plane), planePath) &&
			meshSuccess;
		fmt::print("Plane FBX 输出：{}，耗时 {:.2f}s\n",
			PathToUtf8String(planePath),
			SecondsSince(phaseStartTime));
	}
	if (FbxShape == FbxPlanetShape::Sphere || FbxShape == FbxPlanetShape::Both)
	{
		const std::filesystem::path spherePath = outputRoot / "mesh" / "sphere" / "tiles.fbx";
		const auto phaseStartTime = std::chrono::steady_clock::now();
		meshSuccess = ExportAssimpFbx(buildMeshDatas(FbxPlanetShape::Sphere), spherePath) &&
			meshSuccess;
		fmt::print("Sphere FBX 输出：{}，耗时 {:.2f}s\n",
			PathToUtf8String(spherePath),
			SecondsSince(phaseStartTime));
	}

	fmt::print("FBX 后处理总耗时：{:.2f}s\n", SecondsSince(totalStartTime));
	return meshSuccess;
}

bool DownTool::DownToolBase::WritePostProcessTileFromSourceZoom(
	const Tile& targetTile,
	const std::filesystem::path& outputPath,
	uint32_t sourceZoom) const
{
	if (CanCopyPostProcessTileDirectly() && sourceZoom == targetTile.Zoom)
	{
		const std::filesystem::path sourcePath =
			GetPostProcessSourceTileFilePath(targetTile);
		if (!IsValidImageFile(sourcePath) || IsUnavailableTileFile(sourcePath, targetTile))
		{
			return false;
		}

		return CopyFileReplacing(sourcePath, outputPath);
	}

	const int tileSize = static_cast<int>(TileSize);
	return WriteImage(
		outputPath,
		ReadPostProcessTileRegion(targetTile, sourceZoom, tileSize, tileSize));
}

bool DownTool::DownToolBase::WritePostProcessMaxZoomTile(
	const Tile& targetTile,
	const std::filesystem::path& outputPath) const
{
	return WritePostProcessTileFromSourceZoom(targetTile, outputPath, targetTile.Zoom);
}

bool DownTool::DownToolBase::WritePostProcessMetadata() const
{
	const PostProcessMetadata metadata{
		PostProcessBounds{
			MinLonLat.lon,
			MaxLonLat.lon,
			MinLonLat.lat,
			MaxLonLat.lat },
		Zoom,
		TileSize
	};
	const std::filesystem::path outputRoot = GetPostProcessRootPath();

	const std::string json = BuildMetaJson(metadata);
	const std::string xml = BuildTileMapResourceXml(metadata);
	if (json.empty() || xml.empty())
	{
		return false;
	}

	const bool jsonSuccess = WriteTextFile(outputRoot / "meta.json", json);
	const bool xmlSuccess = WriteTextFile(outputRoot / "tilemapresource.xml", xml);
	return jsonSuccess && xmlSuccess;
}

std::string DownTool::DownToolBase::MakeUri(const Tile& tile)
{
	const int32_t xValue = tile.X;
	const int32_t yValue = tile.Y;
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
