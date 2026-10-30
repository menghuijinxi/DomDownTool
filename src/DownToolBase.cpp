#include "DownToolBase.h"
#include "FbxTerrainTriangulator.h"
#include "FbxTerrainRegionClipper.h"

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
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <set>
#include <span>
#include <stdexcept>
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
#include "fmt/format.h"
#include "libxml/tree.h"
#include "libxml/xmlmemory.h"
#include "nlohmann/json.hpp"
#include "opencv2/core/utility.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"

namespace
{
    using Json = nlohmann::json;

	enum class ImageType { Unknown, Png, Jpeg };

	enum class TileRepairWriteResult { Failed, Existing, Written };

	constexpr uint64_t MaxBatchRepairedTileCount = 4096;
	constexpr size_t CacheScanBatchSize = 128;
	constexpr size_t MaxCacheIssueSampleCount = 20;
	constexpr uint32_t CacheScanConcurrencyMultiplier = 4;
	constexpr long TileRequestTimeoutSeconds = 20;
	constexpr long TileConnectTimeoutSeconds = 10;
    constexpr uint32_t kMaximumDownloadRetryRounds = 3;
    constexpr uint32_t kMaximumFbxTerrainGridSize = 1024;
	constexpr double kMinimumFbxTerrainGeometricErrorMeters = 1.;
    constexpr double kMaximumSphereGeometricErrorMeters = .5;
    constexpr double kMaximumSphereSampleStepMeters = 1000.;

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
        uint32_t terrainMaximumTriangles = 0;
		bool partial = false;
		FbxGlobalPixelBounds pixelBounds;
		DownTool::TileRange tileRange;
		PostProcessBounds bounds;
		std::string imageFormat = "png";
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
		std::array<double, 3> normal = { 0., 0., 1. };
	};

	struct FbxMeshData
	{
		std::string name;
		std::filesystem::path texturePath;
		std::vector<FbxMeshVertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct FbxTerrainCell
	{
		uint32_t minX = 0;
		uint32_t minY = 0;
		uint32_t width = 1;
		uint32_t height = 1;
	};

	struct FbxTerrainMeshPlan
	{
		const FbxTextureOutputInfo* textureInfo = nullptr;
		double clippedMinX = 0.;
		double clippedMinY = 0.;
		double clippedMaxX = 0.;
		double clippedMaxY = 0.;
		double pixelWidth = 0.;
		double pixelHeight = 0.;
		double mercatorNorth = 0.;
		double mercatorSouth = 0.;
		double longitudeSpan = 0.;
		uint32_t gridWidth = 1;
		uint32_t gridHeight = 1;
		double geometricErrorMeters = 1.;
		std::vector<double> heights;
        std::vector<std::array<double, 3>> spherePositionsMeters;
        std::vector<FbxTerrainCell> leaves;
		std::unordered_map<uint32_t, std::vector<uint32_t>> horizontalPoints;
		std::unordered_map<uint32_t, std::vector<uint32_t>> verticalPoints;
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

	template<typename Function, typename StopRequested>
	void ParallelForIndex(
		size_t itemCount,
		uint32_t configuredQueueCount,
		Function function,
		StopRequested stopRequested)
	{
		const uint32_t workerCount = GetParallelWorkerCount(itemCount, configuredQueueCount);
		if (workerCount <= 1)
		{
			for (size_t index = 0; index < itemCount; index++)
			{
				if (stopRequested())
				{
					break;
				}
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
						if (stopRequested())
						{
							break;
						}
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

	template<typename Function>
	void ParallelForIndex(size_t itemCount, uint32_t configuredQueueCount, Function function)
	{
		ParallelForIndex(
			itemCount,
			configuredQueueCount,
			std::move(function),
			[] { return false; });
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

    std::string NormalizeFbxTextureFormat(std::string value)
    {
        value = ToLowerAscii(TrimAsciiWhitespace(value));
        if (value == ".png")
        {
            return "png";
        }
        if (value == ".jpg" || value == ".jpeg" || value == "jpeg")
        {
            return "jpg";
        }
        return value;
    }

    std::string FbxTextureContentType(const std::string& format)
    {
        return format == "jpg" ? "image/jpeg" : "image/png";
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

	bool CanBatchRepairFromParentZoom(uint32_t parentZoom, uint32_t targetZoom)
	{
		if (targetZoom <= parentZoom)
		{
			return false;
		}

		const uint32_t zoomOffset = targetZoom - parentZoom;
		if (zoomOffset >= 31)
		{
			return false;
		}

		const uint64_t scale = uint64_t{ 1 } << zoomOffset;
		return scale * scale <= MaxBatchRepairedTileCount;
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

    ImageType DetectImageType(const std::filesystem::path& path)
    {
        std::ifstream fileStream(path, std::ios::binary);
        if (!fileStream.is_open())
        {
            return ImageType::Unknown;
        }

        unsigned char header[8];
        fileStream.read(reinterpret_cast<char*>(header), sizeof(header));
        return DetectImageType(header, fileStream.gcount());
    }

	size_t WriteTileData(void* ptr, size_t size, size_t nmemb, void* userdata)
	{
		DownTool::CurlMultiHandle* context = static_cast<DownTool::CurlMultiHandle*>(userdata);
		size_t totalSize = size * nmemb;
		if (context->fileStream.is_open())
		{
			context->fileStream.write(static_cast<char*>(ptr), totalSize);
		}
        return context->fileStream.good() ? totalSize : 0;
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
        return DetectImageType(path) != ImageType::Unknown;
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

    std::array<double, 3> LonLatToEcef(const DownTool::LonLat& lonLat, double heightMeters)
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
        return {(primeVerticalRadius + heightMeters) * cosLat * cosLon,
                (primeVerticalRadius + heightMeters) * cosLat * sinLon,
                (primeVerticalRadius * (1. - firstEccentricitySquared) + heightMeters) * sinLat};
    }

	FbxMeshVertex MakePlaneVertex(
		const DownTool::LonLat& lonLat,
		const FbxCoordinateContext& context,
		double u,
		double v,
		double elevationOffsetMeters = 0.)
	{
		const double mercatorX = LongitudeToWebMercatorX(lonLat.lon);
		const double mercatorY = LatitudeToWebMercatorY(lonLat.lat);
		return FbxMeshVertex{
			(mercatorX - context.centerMercatorX) * 100.,
			(mercatorY - context.centerMercatorY) * 100.,
			elevationOffsetMeters * 100.,
			u,
			v
		};
	}

	FbxMeshVertex MakeSphereVertex(
		const DownTool::LonLat& lonLat,
		const FbxCoordinateContext& context,
		double u,
		double v,
		double elevationOffsetMeters = 0.)
	{
        // 高度沿该经纬度的椭球法线，不能沿任务中心的固定 Z 方向叠加。
        const std::array<double, 3> ecef = LonLatToEcef(lonLat, elevationOffsetMeters);
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
        return FbxMeshVertex{east * 100., north * 100., up * 100., u, v};
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

    Json BuildBoundsJson(const PostProcessBounds& bounds)
    {
        return {{"east", bounds.east},
                {"north", bounds.north},
                {"south", bounds.south},
                {"west", bounds.west}};
    }

    std::string BuildMetaJson(const PostProcessMetadata& metadata)
    {
        const Json root = {
            {"bounds", BuildBoundsJson(metadata.bounds)},
            {"contentType", "image/png"},
            {"generatetool", "DomDownTool"},
            {"latLonBounds", BuildBoundsJson(metadata.bounds)},
            {"maxzoom", static_cast<int>(metadata.maxZoom)},
            {"minzoom", 0},
            {"proj", 3857},
            {"tilesize", static_cast<int>(metadata.tileSize)},
            {"tiletrans", "google"},
            {"type", "image"},
            {"ziped", false}};
        return root.dump();
    }

    Json BuildFbxGlobalPixelBoundsJson(const FbxGlobalPixelBounds& bounds)
    {
        return {{"maxx", bounds.maxX},
                {"maxy", bounds.maxY},
                {"minx", bounds.minX},
                {"miny", bounds.minY}};
    }

    Json BuildFbxTileJson(const DownTool::Tile& tile)
    {
        return {{"x", tile.X},
                {"y", tile.Y},
                {"z", static_cast<int>(tile.Zoom)}};
    }

    Json BuildFbxTileRangeJson(const DownTool::TileRange& tileRange)
    {
        return {{"max", BuildFbxTileJson(tileRange.MaxTile)},
                {"min", BuildFbxTileJson(tileRange.MinTile)}};
    }

    Json BuildFbxTextureJson(
        const FbxTextureOutputInfo& textureInfo,
        const std::filesystem::path& basePath)
    {
        return {
            {"bounds", BuildBoundsJson(textureInfo.bounds)},
            {"centerpixelx", textureInfo.centerPixelX},
            {"centerpixely", textureInfo.centerPixelY},
            {"centertilex", textureInfo.centerTileX},
            {"centertiley", textureInfo.centerTileY},
            {"distancecm", textureInfo.distanceCm},
            {"distancemeters", textureInfo.distanceCm / 100.},
            {"terrainmaximumtriangles", textureInfo.terrainMaximumTriangles},
            {"distanceoffset", static_cast<int>(textureInfo.distanceOffset)},
            {"gridx", textureInfo.gridX},
            {"gridy", textureInfo.gridY},
            {"contenttype", FbxTextureContentType(textureInfo.imageFormat)},
            {"format", textureInfo.imageFormat},
            {"partial", textureInfo.partial},
            {"path", ToPortableRelativePath(textureInfo.texturePath, basePath)},
            {"pixelbounds", BuildFbxGlobalPixelBoundsJson(textureInfo.pixelBounds)},
            {"ring", static_cast<int>(textureInfo.ring)},
            {"sourcetexturesize", static_cast<int>(textureInfo.sourceTextureSize)},
            {"outputtexturesize", static_cast<int>(textureInfo.outputTextureSize)},
            {"tilerange", BuildFbxTileRangeJson(textureInfo.tileRange)},
            {"tilesize", static_cast<int>(textureInfo.tileSize)},
            {"zoom", static_cast<int>(textureInfo.zoom)},
            {"version", 1}};
    }

    std::string BuildFbxTextureMetadataJson(
        const FbxTextureOutputInfo& textureInfo,
        const std::filesystem::path& basePath)
    {
        return BuildFbxTextureJson(textureInfo, basePath).dump();
    }

    std::string BuildFbxManifestJson(
        const std::vector<FbxTextureOutputInfo>& textureInfos,
        const std::filesystem::path& basePath, const std::string& meshMode, uint32_t textureSize,
        uint32_t zoom, uint32_t tileSize, const std::string& textureFormat,
        bool terrainHeightEnabled, double elevationReferenceMeters, double terrainSampleRateMeters,
        uint32_t terrainMaximumTriangles, const DownTool::FbxDistanceLod& distanceLod, bool perTile)
    {
        Json textures = Json::array();
        for (const FbxTextureOutputInfo& textureInfo : textureInfos)
        {
            textures.push_back(BuildFbxTextureJson(textureInfo, basePath));
        }

        const Json root = {{"meshmode", meshMode},
                           {"filemode", perTile ? "per-tile" : "merged"},
                           {"texturecount", static_cast<int>(textureInfos.size())},
                           {"texturecontenttype", FbxTextureContentType(textureFormat)},
                           {"textureformat", textureFormat},
                           {"textures", std::move(textures)},
                           {"texturesize", static_cast<int>(textureSize)},
                           {"tilesize", static_cast<int>(tileSize)},
                           {"terrainheight", terrainHeightEnabled},
                           {"elevationreference", elevationReferenceMeters},
                           {"terrainsamplerate", terrainSampleRateMeters},
                           {"terrainmaximumtriangles", terrainMaximumTriangles},
                           {"distancelod", DownTool::FbxDistanceLodToJson(distanceLod)},
                           {"terrainalgorithm", "hmm-progressive-fixed-boundary"},
                           {"version", 1},
                           {"zoom", static_cast<int>(zoom)}};
        return root.dump();
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

	bool WriteFbxTextureImage(
		const FbxTextureOutputInfo& textureInfo,
		const FbxGlobalPixelBounds& requestBounds,
		uint32_t tileSize,
		const std::function<cv::Mat(const DownTool::Tile&)>& readTileImage)
	{
		const int textureSize = static_cast<int>(textureInfo.sourceTextureSize);
		const uint64_t sourceTileXCount = TileRangeCount(
			textureInfo.tileRange.MinTile.X,
			textureInfo.tileRange.MaxTile.X);
		const uint64_t sourceTileYCount = TileRangeCount(
			textureInfo.tileRange.MinTile.Y,
			textureInfo.tileRange.MaxTile.Y);
		if (textureSize <= 0 || tileSize == 0 || sourceTileXCount == 0 ||
			sourceTileYCount == 0 ||
			sourceTileXCount >
				static_cast<uint64_t>(std::numeric_limits<int>::max()) / tileSize ||
			sourceTileYCount >
				static_cast<uint64_t>(std::numeric_limits<int>::max()) / tileSize)
		{
			return false;
		}

		const int sourceWidth = static_cast<int>(sourceTileXCount * tileSize);
		const int sourceHeight = static_cast<int>(sourceTileYCount * tileSize);
		cv::Mat sourceCanvas(
			sourceHeight,
			sourceWidth,
			CV_8UC4,
			cv::Scalar(0, 0, 0, 0));
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
				const int canvasX = (x - textureInfo.tileRange.MinTile.X) *
					static_cast<int>(tileSize);
				const int canvasY = (y - textureInfo.tileRange.MinTile.Y) *
					static_cast<int>(tileSize);
				sourceTileImage.copyTo(sourceCanvas(cv::Rect(
					canvasX,
					canvasY,
					static_cast<int>(tileSize),
					static_cast<int>(tileSize))));
				hasPixel = true;
			}
		}

		if (!hasPixel)
		{
			return false;
		}

		cv::Mat textureImage(
			textureSize,
			textureSize,
			CV_8UC4,
			cv::Scalar(0, 0, 0, 0));
		const cv::Mat transform = (cv::Mat_<double>(2, 3) <<
			1., 0.,
			static_cast<double>(textureInfo.tileRange.MinTile.X) * tileSize -
				textureInfo.pixelBounds.minX,
			0., 1.,
			static_cast<double>(textureInfo.tileRange.MinTile.Y) * tileSize -
				textureInfo.pixelBounds.minY);
		try
		{
			cv::warpAffine(
				sourceCanvas,
				textureImage,
				transform,
				textureImage.size(),
				cv::INTER_LINEAR,
				cv::BORDER_CONSTANT,
				cv::Scalar(0, 0, 0, 0));
		}
		catch (const cv::Exception&)
		{
			return false;
		}

		const double validMinX = std::max(
			textureInfo.pixelBounds.minX,
			requestBounds.minX);
		const double validMinY = std::max(
			textureInfo.pixelBounds.minY,
			requestBounds.minY);
		const double validMaxX = std::min(
			textureInfo.pixelBounds.maxX,
			requestBounds.maxX);
		const double validMaxY = std::min(
			textureInfo.pixelBounds.maxY,
			requestBounds.maxY);
		if (validMaxX > validMinX && validMaxY > validMinY)
		{
			cv::Mat validMask(textureSize, textureSize, CV_8UC1, cv::Scalar(0));
			const int validX0 = std::clamp(
				static_cast<int>(std::floor(validMinX - textureInfo.pixelBounds.minX)),
				0,
				textureSize);
			const int validY0 = std::clamp(
				static_cast<int>(std::floor(validMinY - textureInfo.pixelBounds.minY)),
				0,
				textureSize);
			const int validX1 = std::clamp(
				static_cast<int>(std::ceil(validMaxX - textureInfo.pixelBounds.minX)),
				validX0,
				textureSize);
			const int validY1 = std::clamp(
				static_cast<int>(std::ceil(validMaxY - textureInfo.pixelBounds.minY)),
				validY0,
				textureSize);
			if (validX1 > validX0 && validY1 > validY0)
			{
				cv::rectangle(
					validMask,
					cv::Rect(validX0, validY0, validX1 - validX0, validY1 - validY0),
					cv::Scalar(255),
					cv::FILLED);
			}
			cv::Mat invalidMask;
			cv::bitwise_not(validMask, invalidMask);
			std::vector<cv::Mat> channels;
			cv::split(textureImage, channels);
			channels[3].setTo(0, invalidMask);
			cv::merge(channels, textureImage);
		}
		else
		{
			textureImage.setTo(cv::Scalar(0, 0, 0, 0));
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
		uint32_t subdivision,
		const FbxGlobalPixelBounds& requestBounds,
		bool terrainHeightEnabled,
		double elevationReferenceMeters,
		const std::function<std::optional<double>(const DownTool::LonLat&)>&
			elevationSampler)
	{
		const double clippedMinX =
			std::max(textureInfo.pixelBounds.minX, requestBounds.minX);
		const double clippedMinY =
			std::max(textureInfo.pixelBounds.minY, requestBounds.minY);
		const double clippedMaxX =
			std::min(textureInfo.pixelBounds.maxX, requestBounds.maxX);
		const double clippedMaxY =
			std::min(textureInfo.pixelBounds.maxY, requestBounds.maxY);
		const double pixelWidth = textureInfo.pixelBounds.maxX -
			textureInfo.pixelBounds.minX;
		const double pixelHeight = textureInfo.pixelBounds.maxY -
			textureInfo.pixelBounds.minY;

		if (clippedMinX >= clippedMaxX || clippedMinY >= clippedMaxY ||
			pixelWidth <= 0. || pixelHeight <= 0.)
		{
			return {};
		}

		const uint32_t gridCount =
			shape == DownTool::FbxPlanetShape::Sphere || terrainHeightEnabled ?
			std::max(1u, subdivision) : 1u;
		std::vector<FbxMeshVertex> vertices;
		vertices.reserve(static_cast<size_t>(gridCount + 1) * (gridCount + 1));

		for (uint32_t y = 0; y <= gridCount; y++)
		{
			const double localV = static_cast<double>(y) / gridCount;
			const double pixelY = clippedMinY +
				(clippedMaxY - clippedMinY) * localV;
			const double imageV = (pixelY - textureInfo.pixelBounds.minY) /
				pixelHeight;
			const double fbxV = ConvertImageVToFbxV(imageV);
			const double mercatorNorth =
				LatitudeToWebMercatorY(textureInfo.bounds.north);
			const double mercatorSouth =
				LatitudeToWebMercatorY(textureInfo.bounds.south);
			const double mercatorY = mercatorNorth +
				(mercatorSouth - mercatorNorth) * imageV;
			const double lat = WebMercatorToLonLat(
				0., mercatorY).lat;
			for (uint32_t x = 0; x <= gridCount; x++)
			{
				const double localU = static_cast<double>(x) / gridCount;
				const double pixelX = clippedMinX +
					(clippedMaxX - clippedMinX) * localU;
				const double u = (pixelX - textureInfo.pixelBounds.minX) /
					pixelWidth;
				const double lon = textureInfo.bounds.west +
					(textureInfo.bounds.east - textureInfo.bounds.west) * u;
				const DownTool::LonLat lonLat(lon, lat);
				double elevationOffsetMeters = 0.;
				if (terrainHeightEnabled)
				{
					const std::optional<double> elevation = elevationSampler(lonLat);
					if (elevation)
					{
						elevationOffsetMeters =
							*elevation - elevationReferenceMeters;
					}
					// DEM 边缘缺失采样时保持在高程零面，避免让单个贴图块导出失败。
				}
				if (shape == DownTool::FbxPlanetShape::Sphere)
				{
					vertices.push_back(MakeSphereVertex(
						lonLat, coordinateContext, u, fbxV, elevationOffsetMeters));
				}
				else
				{
					vertices.push_back(MakePlaneVertex(
						lonLat, coordinateContext, u, fbxV, elevationOffsetMeters));
				}
			}
		}

		return vertices;
	}

	uint32_t CalculateFbxTerrainGridSize(
		double distanceMeters, double sampleRateMeters)
	{
		const double effectiveSampleRate = std::isfinite(sampleRateMeters) ?
			std::max(1., sampleRateMeters) : 30.;
		const uint32_t requiredSegments = static_cast<uint32_t>(std::clamp(
			std::ceil(distanceMeters / effectiveSampleRate),
			1.,
			static_cast<double>(kMaximumFbxTerrainGridSize)));
		uint32_t gridSize = 1;
		while (gridSize < requiredSegments &&
			gridSize < kMaximumFbxTerrainGridSize)
		{
			gridSize *= 2;
		}
		return gridSize;
	}

	void SortAndDeduplicateFbxTerrainPoints(
		std::unordered_map<uint32_t, std::vector<uint32_t>>& pointMap)
	{
		for (auto& [coordinate, points] : pointMap)
		{
			(void)coordinate;
			std::sort(points.begin(), points.end());
			points.erase(std::unique(points.begin(), points.end()), points.end());
		}
	}

	double GetFbxTerrainHeight(
		const FbxTerrainMeshPlan& plan, uint32_t x, uint32_t y)
	{
		return plan.heights[
			static_cast<size_t>(y) * (plan.gridWidth + 1) + x];
	}

    std::array<double, 3> GetFbxTerrainErrorPosition(const FbxTerrainMeshPlan& plan, uint32_t x,
                                                     uint32_t y)
    {
        if (plan.spherePositionsMeters.empty())
        {
            return {0., 0., GetFbxTerrainHeight(plan, x, y)};
        }
        return plan.spherePositionsMeters[static_cast<size_t>(y) * (plan.gridWidth + 1) + x];
    }

    DownTool::LonLat FbxTerrainGridToLonLat(
		const FbxTerrainMeshPlan& plan, double gridX, double gridY)
	{
		const double normalizedU = gridX / plan.gridWidth;
		const double normalizedV = gridY / plan.gridHeight;
		const FbxTextureOutputInfo& textureInfo = *plan.textureInfo;
		const double imageU = (plan.clippedMinX +
			(plan.clippedMaxX - plan.clippedMinX) * normalizedU -
			textureInfo.pixelBounds.minX) / plan.pixelWidth;
		const double imageV = (plan.clippedMinY +
			(plan.clippedMaxY - plan.clippedMinY) * normalizedV -
			textureInfo.pixelBounds.minY) / plan.pixelHeight;
		const double mercatorY = plan.mercatorNorth +
			(plan.mercatorSouth - plan.mercatorNorth) * imageV;
		return DownTool::LonLat(
			textureInfo.bounds.west + plan.longitudeSpan * imageU,
			WebMercatorToLonLat(0., mercatorY).lat);
	}

    std::optional<FbxTerrainMeshPlan> BuildAdaptiveFbxTerrainMeshPlan(
        const FbxTextureOutputInfo& textureInfo, const FbxGlobalPixelBounds& requestBounds,
        double elevationReferenceMeters, double sampleRateMeters,
        const std::function<std::optional<double>(const DownTool::LonLat&)>& elevationSampler,
        const FbxCoordinateContext* sphereCoordinates)
    {
		FbxTerrainMeshPlan plan;
		plan.textureInfo = &textureInfo;
		plan.clippedMinX =
			std::max(textureInfo.pixelBounds.minX, requestBounds.minX);
		plan.clippedMinY =
			std::max(textureInfo.pixelBounds.minY, requestBounds.minY);
		plan.clippedMaxX =
			std::min(textureInfo.pixelBounds.maxX, requestBounds.maxX);
		plan.clippedMaxY =
			std::min(textureInfo.pixelBounds.maxY, requestBounds.maxY);
		plan.pixelWidth = textureInfo.pixelBounds.maxX -
			textureInfo.pixelBounds.minX;
		plan.pixelHeight = textureInfo.pixelBounds.maxY -
			textureInfo.pixelBounds.minY;
		if (plan.clippedMinX >= plan.clippedMaxX ||
			plan.clippedMinY >= plan.clippedMaxY ||
			plan.pixelWidth <= 0. || plan.pixelHeight <= 0.)
		{
			return std::nullopt;
		}

		plan.mercatorNorth = LatitudeToWebMercatorY(textureInfo.bounds.north);
		plan.mercatorSouth = LatitudeToWebMercatorY(textureInfo.bounds.south);
		plan.longitudeSpan = textureInfo.bounds.east - textureInfo.bounds.west;
		const double fullWidthMeters =
			std::abs(LongitudeToWebMercatorX(textureInfo.bounds.east) -
				LongitudeToWebMercatorX(textureInfo.bounds.west));
		const double fullHeightMeters =
			std::abs(plan.mercatorNorth - plan.mercatorSouth);
        const double effectiveSampleRate =
            sphereCoordinates ? std::min(sampleRateMeters, kMaximumSphereSampleStepMeters)
                              : sampleRateMeters;
        const uint32_t fullGridWidth =
            CalculateFbxTerrainGridSize(fullWidthMeters, effectiveSampleRate);
        const uint32_t fullGridHeight =
            CalculateFbxTerrainGridSize(fullHeightMeters, effectiveSampleRate);
        if (sphereCoordinates &&
            (fullWidthMeters / fullGridWidth > kMaximumSphereSampleStepMeters ||
             fullHeightMeters / fullGridHeight > kMaximumSphereSampleStepMeters))
        {
            fmt::print("FBX 球面块范围超过采样网格能力，请提高导出层级或减小贴图块尺寸。\n");
            return std::nullopt;
        }
        plan.gridWidth = std::max(
			1u,
			static_cast<uint32_t>(std::ceil(
				fullGridWidth * (plan.clippedMaxX - plan.clippedMinX) /
				plan.pixelWidth)));
		plan.gridHeight = std::max(
			1u,
			static_cast<uint32_t>(std::ceil(
				fullGridHeight * (plan.clippedMaxY - plan.clippedMinY) /
				plan.pixelHeight)));
		plan.heights.resize(
			static_cast<size_t>(plan.gridWidth + 1) * (plan.gridHeight + 1));
        if (sphereCoordinates)
        {
            plan.spherePositionsMeters.resize(plan.heights.size());
        }
        for (uint32_t y = 0; y <= plan.gridHeight; y++)
		{
			for (uint32_t x = 0; x <= plan.gridWidth; x++)
			{
                const auto lonLat = FbxTerrainGridToLonLat(plan, x, y);
                const auto elevation = elevationSampler(lonLat);
                const size_t index = static_cast<size_t>(y) * (plan.gridWidth + 1) + x;
                plan.heights[index] = elevation ? *elevation - elevationReferenceMeters : 0.;
                if (sphereCoordinates)
                {
                    const auto position =
                        MakeSphereVertex(lonLat, *sphereCoordinates, 0., 0., plan.heights[index]);
                    plan.spherePositionsMeters[index] = {position.x * .01, position.y * .01,
                                                         position.z * .01};
                }
            }
		}

        const double heightErrorMeters =
            std::max(kMinimumFbxTerrainGeometricErrorMeters, std::max(1., sampleRateMeters) * 0.1);
        const double geometricErrorMeters =
            sphereCoordinates ? kMaximumSphereGeometricErrorMeters : heightErrorMeters;
        plan.geometricErrorMeters = geometricErrorMeters;
		const auto calculateCellError = [&plan](const FbxTerrainCell& cell)
		{
			const uint32_t maxX = cell.minX + cell.width;
			const uint32_t maxY = cell.minY + cell.height;
			const double northWest =
				GetFbxTerrainHeight(plan, cell.minX, cell.minY);
			const double northEast = GetFbxTerrainHeight(plan, maxX, cell.minY);
			const double southWest = GetFbxTerrainHeight(plan, cell.minX, maxY);
			const double southEast = GetFbxTerrainHeight(plan, maxX, maxY);
			double maximumError = 0.;
			for (uint32_t y = cell.minY; y <= maxY; y++)
			{
				const double v = static_cast<double>(y - cell.minY) / cell.height;
				for (uint32_t x = cell.minX; x <= maxX; x++)
				{
					const double u =
						static_cast<double>(x - cell.minX) / cell.width;
					const double interpolated =
						northWest * (1. - u) * (1. - v) +
						northEast * u * (1. - v) +
						southWest * (1. - u) * v + southEast * u * v;
					maximumError = std::max(
						maximumError,
						std::abs(GetFbxTerrainHeight(plan, x, y) - interpolated));
                    if (!plan.spherePositionsMeters.empty())
                    {
                        const auto& positions = plan.spherePositionsMeters;
                        const auto at = [&](uint32_t px, uint32_t py) -> const auto& {
                            return positions[static_cast<size_t>(py) * (plan.gridWidth + 1) + px];
                        };
                        double squaredError = 0.;
                        for (size_t axis = 0; axis < 3; ++axis)
                        {
                            const double value =
                                at(cell.minX, cell.minY)[axis] * (1. - u) * (1. - v) +
                                at(maxX, cell.minY)[axis] * u * (1. - v) +
                                at(cell.minX, maxY)[axis] * (1. - u) * v +
                                at(maxX, maxY)[axis] * u * v;
                            const double difference = at(x, y)[axis] - value;
                            squaredError += difference * difference;
                        }
                        maximumError = std::max(maximumError, std::sqrt(squaredError));
                    }
                }
			}
			return maximumError;
		};

		std::function<void(const FbxTerrainCell&)> buildNode;
		buildNode = [&](const FbxTerrainCell& cell)
		{
			const bool canSplitX = cell.width > 1;
			const bool canSplitY = cell.height > 1;
			if ((!canSplitX && !canSplitY) ||
				calculateCellError(cell) <= geometricErrorMeters)
			{
				plan.leaves.push_back(cell);
				return;
			}

			const uint32_t leftWidth = canSplitX ? cell.width / 2 : cell.width;
			const uint32_t topHeight = canSplitY ? cell.height / 2 : cell.height;
			if (canSplitX && canSplitY)
			{
				buildNode({cell.minX, cell.minY, leftWidth, topHeight});
				buildNode({cell.minX + leftWidth, cell.minY,
					cell.width - leftWidth, topHeight});
				buildNode({cell.minX, cell.minY + topHeight,
					leftWidth, cell.height - topHeight});
				buildNode({cell.minX + leftWidth, cell.minY + topHeight,
					cell.width - leftWidth, cell.height - topHeight});
			}
			else if (canSplitX)
			{
				buildNode({cell.minX, cell.minY, leftWidth, cell.height});
				buildNode({cell.minX + leftWidth, cell.minY,
					cell.width - leftWidth, cell.height});
			}
			else
			{
				buildNode({cell.minX, cell.minY, cell.width, topHeight});
				buildNode({cell.minX, cell.minY + topHeight,
					cell.width, cell.height - topHeight});
			}
		};
		buildNode({0, 0, plan.gridWidth, plan.gridHeight});

		for (const FbxTerrainCell& cell : plan.leaves)
		{
			const uint32_t maxX = cell.minX + cell.width;
			const uint32_t maxY = cell.minY + cell.height;
			plan.horizontalPoints[cell.minY].insert(
				plan.horizontalPoints[cell.minY].end(), {cell.minX, maxX});
			plan.horizontalPoints[maxY].insert(
				plan.horizontalPoints[maxY].end(), {cell.minX, maxX});
			plan.verticalPoints[cell.minX].insert(
				plan.verticalPoints[cell.minX].end(), {cell.minY, maxY});
			plan.verticalPoints[maxX].insert(
				plan.verticalPoints[maxX].end(), {cell.minY, maxY});
		}
		SortAndDeduplicateFbxTerrainPoints(plan.horizontalPoints);
		SortAndDeduplicateFbxTerrainPoints(plan.verticalPoints);
		return plan;
	}

	void MergeFbxTerrainEdgePoints(
		std::vector<uint32_t>& first, std::vector<uint32_t>& second)
	{
		first.insert(first.end(), second.begin(), second.end());
		std::sort(first.begin(), first.end());
		first.erase(std::unique(first.begin(), first.end()), first.end());
		second = first;
	}

    void LimitFbxTerrainEdge(std::vector<uint32_t>& points, uint32_t maximumTriangles,
                             const std::function<std::array<double, 3>(uint32_t)>& positionAt)
    {
        if (maximumTriangles == 0)
        {
            return;
        }
        // 最多约一半预算分给边界，剩余预算用于峰谷；四角始终保留。
        const size_t maximumPoints = 2 + (maximumTriangles - 2) / 8;
        if (points.size() <= maximumPoints)
        {
            return;
        }
        std::set<size_t> selected{0, points.size() - 1};
        while (selected.size() < maximumPoints)
        {
            double maximumError = -1.;
            size_t candidate = 0;
            for (auto first = selected.begin(), second = std::next(first);
                 second != selected.end(); ++first, ++second)
            {
                const uint32_t a = points[*first];
                const uint32_t b = points[*second];
                const auto firstPosition = positionAt(a);
                const auto lastPosition = positionAt(b);
                for (size_t i = *first + 1; i < *second; ++i)
                {
                    const double t = double(points[i] - a) / (b - a);
                    const auto position = positionAt(points[i]);
                    double squaredError = 0.;
                    for (size_t axis = 0; axis < 3; ++axis)
                    {
                        const double difference = position[axis] - firstPosition[axis] * (1. - t) -
                                                  lastPosition[axis] * t;
                        squaredError += difference * difference;
                    }
                    const double error = std::sqrt(squaredError);
                    if (error > maximumError)
                    {
                        maximumError = error;
                        candidate = i;
                    }
                }
            }
            if (maximumError <= 1e-9)
            {
                break;
            }
            selected.insert(candidate);
        }
        std::vector<uint32_t> simplified;
        for (size_t index : selected)
        {
            simplified.push_back(points[index]);
        }
        points = std::move(simplified);
    }

    uint32_t SharedFbxTerrainBudget(uint32_t first, uint32_t second)
    {
        if (first == 0)
        {
            return second;
        }
        if (second == 0)
        {
            return first;
        }
        return std::min(first, second);
    }

	bool SynchronizeFbxTerrainMeshPlanEdges(std::vector<FbxTerrainMeshPlan>& plans)
	{
		std::map<std::pair<int32_t, int32_t>, size_t> planByGrid;
		for (size_t index = 0; index < plans.size(); index++)
		{
			const FbxTextureOutputInfo& textureInfo = *plans[index].textureInfo;
			planByGrid[{textureInfo.gridX, textureInfo.gridY}] = index;
		}

		for (FbxTerrainMeshPlan& plan : plans)
		{
			const FbxTextureOutputInfo& textureInfo = *plan.textureInfo;
            const auto limitHorizontal = [&](uint32_t y) {
                LimitFbxTerrainEdge(
                    plan.horizontalPoints[y], textureInfo.terrainMaximumTriangles,
                    [&](uint32_t x) { return GetFbxTerrainErrorPosition(plan, x, y); });
            };
            const auto limitVertical = [&](uint32_t x) {
                LimitFbxTerrainEdge(
                    plan.verticalPoints[x], textureInfo.terrainMaximumTriangles,
                    [&](uint32_t y) { return GetFbxTerrainErrorPosition(plan, x, y); });
            };
            if (!planByGrid.contains({textureInfo.gridX - 1, textureInfo.gridY}))
            {
                limitVertical(0);
            }
            if (!planByGrid.contains({textureInfo.gridX, textureInfo.gridY - 1}))
            {
                limitHorizontal(0);
            }
			const auto east = planByGrid.find(
				{textureInfo.gridX + 1, textureInfo.gridY});
			const auto south = planByGrid.find(
				{textureInfo.gridX, textureInfo.gridY + 1});
			if (east != planByGrid.end())
			{
				FbxTerrainMeshPlan& neighbor = plans[east->second];
				if (plan.gridHeight != neighbor.gridHeight)
				{
					return false;
				}
				MergeFbxTerrainEdgePoints(
					plan.verticalPoints[plan.gridWidth],
					neighbor.verticalPoints[0]);
                // 共享边只简化一次，采用两侧较小的非零预算，防止跨档位裂缝。
                LimitFbxTerrainEdge(
                    plan.verticalPoints[plan.gridWidth],
                    SharedFbxTerrainBudget(textureInfo.terrainMaximumTriangles,
                                           neighbor.textureInfo->terrainMaximumTriangles),
                    [&](uint32_t y) {
                        return GetFbxTerrainErrorPosition(plan, plan.gridWidth, y);
                    });
                neighbor.verticalPoints[0] = plan.verticalPoints[plan.gridWidth];
			}
            else
            {
                limitVertical(plan.gridWidth);
            }

			if (south != planByGrid.end())
			{
				FbxTerrainMeshPlan& neighbor = plans[south->second];
				if (plan.gridWidth != neighbor.gridWidth)
				{
					return false;
				}
				MergeFbxTerrainEdgePoints(
					plan.horizontalPoints[plan.gridHeight],
					neighbor.horizontalPoints[0]);
                LimitFbxTerrainEdge(
                    plan.horizontalPoints[plan.gridHeight],
                    SharedFbxTerrainBudget(textureInfo.terrainMaximumTriangles,
                                           neighbor.textureInfo->terrainMaximumTriangles),
                    [&](uint32_t x) {
                        return GetFbxTerrainErrorPosition(plan, x, plan.gridHeight);
                    });
                neighbor.horizontalPoints[0] = plan.horizontalPoints[plan.gridHeight];
			}
            else
            {
                limitHorizontal(plan.gridHeight);
            }
		}
		return true;
	}

	// 先完整采样高度场并生成叶节点，再统一相邻块边界的分割点。
	// 这样既不会漏掉窄山脊，也不会在块间留下 T 型接缝。
	std::vector<FbxMeshVertex> BuildAdaptiveFbxTerrainVertices(
		const FbxTerrainMeshPlan& plan,
		const FbxCoordinateContext& coordinateContext,
		DownTool::FbxPlanetShape shape,
		std::vector<uint32_t>& indices)
	{
		const FbxTextureOutputInfo& textureInfo = *plan.textureInfo;
        const uint32_t maximumTriangles = textureInfo.terrainMaximumTriangles;
		std::vector<DownTool::FbxTerrainGridPoint> boundary;
		const auto appendBoundaryPoint = [&boundary](uint32_t x, uint32_t y)
		{
			const DownTool::FbxTerrainGridPoint point{x, y};
			if (boundary.empty() || boundary.back().x != x || boundary.back().y != y)
			{
				boundary.push_back(point);
			}
		};
		const auto appendHorizontal = [&](uint32_t y, bool reverse)
		{
			const auto found = plan.horizontalPoints.find(y);
			if (found == plan.horizontalPoints.end())
			{
				return;
			}
			if (reverse)
			{
				for (auto point = found->second.rbegin(); point != found->second.rend(); ++point)
				{
					appendBoundaryPoint(*point, y);
				}
			}
			else
			{
				for (uint32_t x : found->second)
				{
					appendBoundaryPoint(x, y);
				}
			}
		};
		const auto appendVertical = [&](uint32_t x, bool reverse)
		{
			const auto found = plan.verticalPoints.find(x);
			if (found == plan.verticalPoints.end())
			{
				return;
			}
			if (reverse)
			{
				for (auto point = found->second.rbegin(); point != found->second.rend(); ++point)
				{
					appendBoundaryPoint(x, *point);
				}
			}
			else
			{
				for (uint32_t y : found->second)
				{
					appendBoundaryPoint(x, y);
				}
			}
		};
		appendHorizontal(0, false);
		appendVertical(plan.gridWidth, false);
		appendHorizontal(plan.gridHeight, true);
		appendVertical(0, true);
		if (boundary.size() > 1 && boundary.front().x == boundary.back().x &&
			boundary.front().y == boundary.back().y)
		{
			boundary.pop_back();
		}
		if (boundary.size() < 3)
		{
			return {};
		}

        const std::vector<std::array<double, 3>> noPositions;
        const bool sphere = shape == DownTool::FbxPlanetShape::Sphere;
        const DownTool::FbxTerrainTinResult tin = DownTool::BuildFbxTerrainTin(
            plan.gridWidth, plan.gridHeight, plan.heights, boundary, plan.geometricErrorMeters,
            maximumTriangles, sphere ? plan.spherePositionsMeters : noPositions);
        if (tin.points.empty() || tin.triangles.empty())
		{
            throw std::runtime_error("FBX 地形三角化失败，未回退到其他算法。");
		}
        if (shape == DownTool::FbxPlanetShape::Sphere &&
            tin.maximumErrorMeters > kMaximumSphereGeometricErrorMeters + 1.e-6)
        {
            throw std::runtime_error(
                fmt::format("球面地形块 {} 的三维采样误差 {:.3f}m 超过 {:.3f}m；"
                            "请提高面数上限（0 为不限）或减小采样间距。",
                            MakeFbxMeshName(textureInfo.gridX, textureInfo.gridY),
                            tin.maximumErrorMeters, kMaximumSphereGeometricErrorMeters));
        }
        fmt::print("地形块 {}：{} 顶点，{} 三角面，上限 {}，最大采样位置误差 {:.3f}m\n",
                   MakeFbxMeshName(textureInfo.gridX, textureInfo.gridY), tin.points.size(),
                   tin.triangles.size(), maximumTriangles, tin.maximumErrorMeters);

        std::vector<FbxMeshVertex> vertices;
		vertices.reserve(tin.points.size());
		for (const DownTool::FbxTerrainGridPoint& point : tin.points)
		{
			const double normalizedU = static_cast<double>(point.x) / plan.gridWidth;
			const double normalizedV = static_cast<double>(point.y) / plan.gridHeight;
			const double imageU = (plan.clippedMinX +
				(plan.clippedMaxX - plan.clippedMinX) * normalizedU -
				textureInfo.pixelBounds.minX) / plan.pixelWidth;
			const double imageV = (plan.clippedMinY +
				(plan.clippedMaxY - plan.clippedMinY) * normalizedV -
				textureInfo.pixelBounds.minY) / plan.pixelHeight;
			const DownTool::LonLat lonLat = FbxTerrainGridToLonLat(
				plan, point.x, point.y);
			const double height = GetFbxTerrainHeight(plan, point.x, point.y);
			vertices.push_back(shape == DownTool::FbxPlanetShape::Sphere ?
				MakeSphereVertex(lonLat, coordinateContext, imageU,
					ConvertImageVToFbxV(imageV), height) :
				MakePlaneVertex(lonLat, coordinateContext, imageU,
					ConvertImageVToFbxV(imageV), height));
		}
		for (const DownTool::FbxTerrainTriangle& triangle : tin.triangles)
		{
			// 网格坐标的 Y 轴向南，反转 CDT 的二维逆时针顺序以保持 FBX 法线朝上。
			indices.insert(indices.end(), {
				triangle.first, triangle.third, triangle.second});
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

    bool ClipFbxTerrainMesh(
        FbxMeshData& mesh, const FbxTextureOutputInfo& texture,
        const DownTool::FbxTerrainRegionClipper& clipper, std::string& error)
    {
        if (!clipper.IsActive())
        {
            return true;
        }
        const double west = LongitudeToWebMercatorX(texture.bounds.west);
        const double south = LatitudeToWebMercatorY(texture.bounds.south);
        const double width = LongitudeToWebMercatorX(texture.bounds.east) - west;
        const double height = LatitudeToWebMercatorY(texture.bounds.north) - south;
        const auto sourceVertices = std::move(mesh.vertices);
        const auto sourceIndices = std::move(mesh.indices);
        mesh.vertices.clear();
        mesh.indices.clear();
        std::vector<DownTool::FbxTerrainClipPoint> projected;
        projected.reserve(sourceVertices.size());
        for (const auto& vertex : sourceVertices)
        {
            projected.emplace_back(west + width * vertex.u, south + height * vertex.v);
        }
        constexpr uint32_t kUnmapped = std::numeric_limits<uint32_t>::max();
        std::vector<uint32_t> originalIndices(sourceVertices.size(), kUnmapped);
        std::map<std::pair<int64_t, int64_t>, uint32_t> intersections;
        mesh.indices.reserve(sourceIndices.size());
        const auto appendOriginal = [&](uint32_t index)
        {
            auto& mapped = originalIndices[index];
            if (mapped == kUnmapped)
            {
                mapped = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(sourceVertices[index]);
            }
            mesh.indices.push_back(mapped);
        };
        std::vector<DownTool::FbxTerrainClipTriangle> triangles;
        for (size_t index = 0; index < sourceIndices.size(); index += 3)
        {
            const std::array<uint32_t, 3> indices = {
                sourceIndices[index], sourceIndices[index + 1], sourceIndices[index + 2]};
            const DownTool::FbxTerrainClipTriangle source = {
                projected[indices[0]], projected[indices[1]], projected[indices[2]]};
            if (!clipper.ClipTriangle(source, triangles, error))
            {
                return false;
            }
            const double firstX = source[1].GetX() - source[0].GetX();
            const double firstY = source[1].GetY() - source[0].GetY();
            const double secondX = source[2].GetX() - source[0].GetX();
            const double secondY = source[2].GetY() - source[0].GetY();
            const double determinant = firstX * secondY - firstY * secondX;
            for (const auto& triangle : triangles)
            {
                for (const auto& point : triangle)
                {
                    bool original = false;
                    for (size_t corner = 0; corner < 3; ++corner)
                    {
                        if (std::abs(point.GetX() - source[corner].GetX()) < 1.1e-5 &&
                            std::abs(point.GetY() - source[corner].GetY()) < 1.1e-5)
                        {
                            appendOriginal(indices[corner]);
                            original = true;
                            break;
                        }
                    }
                    if (original)
                    {
                        continue;
                    }
                    const auto key = std::pair(
                        static_cast<int64_t>(std::llround(point.GetX() * 100000.)),
                        static_cast<int64_t>(std::llround(point.GetY() * 100000.)));
                    if (const auto found = intersections.find(key); found != intersections.end())
                    {
                        mesh.indices.push_back(found->second);
                        continue;
                    }
                    const double offsetX = point.GetX() - source[0].GetX();
                    const double offsetY = point.GetY() - source[0].GetY();
                    const double secondWeight =
                        (offsetX * secondY - offsetY * secondX) / determinant;
                    const double thirdWeight =
                        (firstX * offsetY - firstY * offsetX) / determinant;
                    const std::array<double, 3> weights = {
                        1. - secondWeight - thirdWeight, secondWeight, thirdWeight};
                    FbxMeshVertex vertex;
                    for (size_t corner = 0; corner < 3; ++corner)
                    {
                        const auto& input = sourceVertices[indices[corner]];
                        vertex.x += input.x * weights[corner];
                        vertex.y += input.y * weights[corner];
                        vertex.z += input.z * weights[corner];
                        vertex.u += input.u * weights[corner];
                        vertex.v += input.v * weights[corner];
                    }
                    const auto mapped = static_cast<uint32_t>(mesh.vertices.size());
                    intersections.emplace(key, mapped);
                    mesh.vertices.push_back(vertex);
                    mesh.indices.push_back(mapped);
                }
            }
        }
        return true;
    }

    struct FbxTerrainFlattenContext
    {
        const FbxCoordinateContext& coordinates;
        DownTool::FbxPlanetShape shape;
        double elevationReference = 0.;
    };

    class FbxTerrainFlattenRegion
    {
      public:
        bool Initialize(const DownTool::TerrainFlattenRegion& region, std::string& error,
                        const std::function<bool()>& canceled)
        {
            elevation_ = region.GetElevationMeters();
            return inside_.Initialize({region.GetPolygon()}, {}, error, canceled) &&
                   outside_.Initialize({}, {region.GetPolygon()}, error, canceled);
        }

        bool Apply(FbxMeshData& mesh, const FbxTextureOutputInfo& texture,
                   const FbxTerrainFlattenContext& context, std::string& error) const
        {
            bool visible = false;
            if (!inside_.HasVisibleArea({texture.bounds.west, texture.bounds.east,
                                         texture.bounds.south, texture.bounds.north},
                                        visible, error))
            {
                return false;
            }
            if (!visible)
            {
                return true;
            }
            FbxMeshData flattened = mesh;
            if (!ClipFbxTerrainMesh(flattened, texture, inside_, error))
            {
                return false;
            }
            if (flattened.indices.empty())
            {
                return true;
            }
            const double south = LatitudeToWebMercatorY(texture.bounds.south);
            const double north = LatitudeToWebMercatorY(texture.bounds.north);
            const double west = LongitudeToWebMercatorX(texture.bounds.west);
            const double east = LongitudeToWebMercatorX(texture.bounds.east);
            const auto coordinate = [&](const FbxMeshVertex& vertex) {
                return DownTool::LonLat(
                    texture.bounds.west + (texture.bounds.east - texture.bounds.west) * vertex.u,
                    WebMercatorToLonLat(0., south + (north - south) * vertex.v).lat);
            };
            const auto key = [&](const FbxMeshVertex& vertex) {
                return std::pair(
                    static_cast<int64_t>(std::llround((west + (east - west) * vertex.u) * 100000.)),
                    static_cast<int64_t>(
                        std::llround((south + (north - south) * vertex.v) * 100000.)));
            };
            std::set<std::pair<int64_t, int64_t>> flattenedPoints;
            for (const auto& vertex : flattened.vertices)
            {
                flattenedPoints.insert(key(vertex));
            }
            if (!ClipFbxTerrainMesh(mesh, texture, outside_, error))
            {
                return false;
            }
            const auto flatten = [&](FbxMeshVertex& vertex) {
                const auto point = coordinate(vertex);
                vertex = context.shape == DownTool::FbxPlanetShape::Plane
                             ? MakePlaneVertex(point, context.coordinates, vertex.u, vertex.v,
                                               elevation_ - context.elevationReference)
                             : MakeSphereVertex(point, context.coordinates, vertex.u, vertex.v,
                                                elevation_ - context.elevationReference);
            };
            // 区外三角面连接到同高的边界，避免裁切后出现垂直裂缝。
            for (auto& vertex : mesh.vertices)
            {
                if (flattenedPoints.contains(key(vertex)))
                {
                    flatten(vertex);
                }
            }
            for (auto& vertex : flattened.vertices)
            {
                flatten(vertex);
            }
            const auto offset = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.insert(mesh.vertices.end(), flattened.vertices.begin(),
                                 flattened.vertices.end());
            for (const auto index : flattened.indices)
            {
                mesh.indices.push_back(index + offset);
            }
            return true;
        }

      private:
        double elevation_ = 0.;
        DownTool::FbxTerrainRegionClipper inside_;
        DownTool::FbxTerrainRegionClipper outside_;
    };

    using FbxNormalKey = std::array<int64_t, 2>;

    FbxNormalKey MakeFbxNormalKey(const FbxMeshVertex& vertex)
	{
		// 网格边界由相邻瓦片分别计算，浮点坐标可能存在极小误差。
		// 只按平面位置归并，确保同一地形位置跨瓦片共享法线。
		constexpr double keyScale = 1000.;
		return {
			static_cast<int64_t>(std::llround(vertex.x * keyScale)),
			static_cast<int64_t>(std::llround(vertex.y * keyScale))};
	}

	void AssignFbxMeshNormals(std::vector<FbxMeshData>& meshDatas)
	{
		std::map<FbxNormalKey, std::array<double, 3>> accumulatedNormals;
		const auto addFaceNormal = [&](const FbxMeshVertex& first,
			const FbxMeshVertex& second, const FbxMeshVertex& third)
		{
			const std::array<double, 3> firstEdge{
				second.x - first.x,
				second.y - first.y,
				second.z - first.z};
			const std::array<double, 3> secondEdge{
				third.x - first.x,
				third.y - first.y,
				third.z - first.z};
			const std::array<double, 3> faceNormal{
				firstEdge[1] * secondEdge[2] - firstEdge[2] * secondEdge[1],
				firstEdge[2] * secondEdge[0] - firstEdge[0] * secondEdge[2],
				firstEdge[0] * secondEdge[1] - firstEdge[1] * secondEdge[0]};
			const double length = std::sqrt(
				faceNormal[0] * faceNormal[0] +
				faceNormal[1] * faceNormal[1] +
				faceNormal[2] * faceNormal[2]);
			if (length <= std::numeric_limits<double>::epsilon())
			{
				return;
			}

			const std::array<double, 3> unitFaceNormal{
				faceNormal[0] / length,
				faceNormal[1] / length,
				faceNormal[2] / length};
			for (const FbxMeshVertex* vertex : { &first, &second, &third })
			{
				std::array<double, 3>& accumulated =
					accumulatedNormals[MakeFbxNormalKey(*vertex)];
				// UE 默认按单位面法线等权混合，避免自适应 TIN 的大三角形主导顶点法线。
				accumulated[0] += unitFaceNormal[0];
				accumulated[1] += unitFaceNormal[1];
				accumulated[2] += unitFaceNormal[2];
			}
		};

		for (const FbxMeshData& meshData : meshDatas)
		{
			for (size_t index = 0; index + 2 < meshData.indices.size(); index += 3)
			{
				addFaceNormal(
					meshData.vertices[meshData.indices[index]],
					meshData.vertices[meshData.indices[index + 1]],
					meshData.vertices[meshData.indices[index + 2]]);
			}
		}

		for (FbxMeshData& meshData : meshDatas)
		{
			for (FbxMeshVertex& vertex : meshData.vertices)
			{
				const auto normal = accumulatedNormals.find(MakeFbxNormalKey(vertex));
				if (normal == accumulatedNormals.end())
				{
					vertex.normal = { 0., 0., 1. };
					continue;
				}

				const double length = std::sqrt(
					normal->second[0] * normal->second[0] +
					normal->second[1] * normal->second[1] +
					normal->second[2] * normal->second[2]);
				if (length <= std::numeric_limits<double>::epsilon())
				{
					vertex.normal = { 0., 0., 1. };
					continue;
				}
				vertex.normal = {
					normal->second[0] / length,
					normal->second[1] / length,
					normal->second[2] / length};
			}
		}
	}

	aiMesh* CreateAssimpMesh(const FbxMeshData& meshData, uint32_t materialIndex)
	{
		aiMesh* mesh = new aiMesh();
		mesh->mName = aiString(meshData.name);
		mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
		mesh->mMaterialIndex = materialIndex;
		mesh->mNumVertices = static_cast<unsigned int>(meshData.vertices.size());
		mesh->mVertices = new aiVector3D[mesh->mNumVertices];
		mesh->mNormals = new aiVector3D[mesh->mNumVertices];
		mesh->mTextureCoords[0] = new aiVector3D[mesh->mNumVertices];
		mesh->mNumUVComponents[0] = 2;

		for (unsigned int index = 0; index < mesh->mNumVertices; index++)
		{
			const FbxMeshVertex& vertex = meshData.vertices[index];
			mesh->mVertices[index] = aiVector3D(
				static_cast<float>(vertex.x),
				static_cast<float>(vertex.y),
				static_cast<float>(vertex.z));
			mesh->mNormals[index] = aiVector3D(
				static_cast<float>(vertex.normal[0]),
				static_cast<float>(vertex.normal[1]),
				static_cast<float>(vertex.normal[2]));
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

    bool ExportAssimpFbx(std::span<const FbxMeshData> meshDatas,
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

    bool WriteEncodedImage(
        const std::filesystem::path& path,
        const cv::Mat& image,
        std::string extension)
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
        if (extension.empty())
        {
            extension = ".png";
        }
        std::transform(
            extension.begin(),
            extension.end(),
            extension.begin(),
            [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });

        cv::Mat convertedImage;
        const cv::Mat* imageToEncode = &image;
        if ((extension == ".jpg" || extension == ".jpeg") && image.channels() == 4)
        {
            cv::cvtColor(image, convertedImage, cv::COLOR_BGRA2BGR);
            imageToEncode = &convertedImage;
        }

        std::vector<unsigned char> data;
        if (!cv::imencode(extension, *imageToEncode, data))
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

    bool WriteImage(const std::filesystem::path& path, const cv::Mat& image)
    {
        return WriteEncodedImage(path, image, path.extension().string());
    }

    bool HasTransparentPixels(const cv::Mat& image)
    {
        if (image.channels() != 4)
        {
            return false;
        }
        if (image.depth() != CV_8U)
        {
            return true;
        }

        cv::Mat alphaChannel;
        cv::extractChannel(image, alphaChannel, 3);
        double minimumAlpha = 255.;
        cv::minMaxLoc(alphaChannel, &minimumAlpha, nullptr);
        return minimumAlpha < 255.;
    }

    bool WriteXyzTileImage(const std::filesystem::path& path, const cv::Mat& image)
    {
        const std::string extension = HasTransparentPixels(image) ? ".png" : ".jpg";
        return WriteEncodedImage(path, image, extension);
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
		if (IsStopRequested())
		{
			break;
		}

		if (msg->msg == CURLMSG_DONE)
		{
			CURL* easy = msg->easy_handle;
            // 完成消息在移除或释放句柄后失效，先保存后续判断需要的结果。
            const CURLcode requestResult = msg->data.result;
            long responseCode = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &responseCode);
            const bool requestSucceeded =
                requestResult == CURLE_OK &&
                (responseCode == 0 || (responseCode >= 200 && responseCode < 300));
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
            if (requestResult != CURLE_OK)
            {
				if (hasContext)
				{
                    fmt::print("\nRequest failed: {}, url: {}\n", curl_easy_strerror(requestResult),
                               url);
                }
				else
				{
                    fmt::print("\nRequest failed: {}\n", curl_easy_strerror(requestResult));
                }
			}
            else if (!requestSucceeded)
            {
                fmt::print("\nHTTP request failed: {}, url: {}\n", responseCode, url);
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
            if (!requestSucceeded)
            {
                // 失败传输可能已写入图片头，清理残片后才能被缓存复查识别并重试。
                RemoveFileIfExists(tilePath);
            }

            if (requestType == CurlRequestType::Tile)
			{
                if (requestSucceeded &&
                    (responseHeaderUnavailable || IsUnavailableTileFile(tilePath, tile)))
                {
					RemoveFileIfExists(tilePath);
					if (!RepairTileFromLowerZoom(tile, tilePath))
					{
						fmt::print("\n无法从低级别瓦片修复占位图: {}\n", PathToUtf8String(tilePath));
					}
				}

				DowningCount += 1;
				hasProgressUpdate = true;
				continue;
			}

            if (requestSucceeded && responseHeaderUnavailable)
            {
				RemoveFileIfExists(tilePath);
			}

            if (requestSucceeded && !responseHeaderUnavailable && IsValidImageFile(tilePath) &&
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
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount);
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

DownTool::DownToolBase::DownToolBase() = default;

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

	bStop = false;
    downloadStage_ = "下载瓦片";
    downloadError_.clear();
    if (IsStopRequested())
	{
		return;
	}
    if (!PrepareFbxDownloadRange())
    {
        return;
    }

	bDowning = true;
	DowningCount = 0;
	TotalDownCount = 0;

	curlMulti = curl_multi_init();

	// 初始化共享句柄，设置共享DNS缓存
	share = curl_share_init();
	curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

	CalculateDownRange();
	std::vector<Tile> targetTiles;
	BuildTargetTiles(targetTiles);
	if (IsStopRequested())
	{
		bDowning = false;
		return;
	}
	TotalDownCount = static_cast<uint32_t>(targetTiles.size());
    ReportProgress(downloadStage_, 0, TotalDownCount);

    fmt::print("下载范围：\n左上角经纬度：({}, {})\n右下角经纬度：({}, {})\n左上角瓦片坐标：({}, {})\n右下角瓦片坐标：({}, {})\n缓存路径：{}\n总下载切片数量预估：{}\n",
		MinLonLat.lon, MaxLonLat.lat, MaxLonLat.lon, MinLonLat.lat,
		MinTile.X, MinTile.Y, MaxTile.X, MaxTile.Y, CachePath, TotalDownCount);

	const std::vector<Tile> pendingTiles =
		ScanCacheAndCollectPendingTiles(
			targetTiles, "下载前缓存检测", true, false);

    DownloadTileBatch(pendingTiles);

    std::vector<Tile> failedTiles;
	if (!IsStopRequested())
	{
		failedTiles =
			ScanCacheAndCollectPendingTiles(
				targetTiles, "下载后缓存检测", false, true);
	}
    RetryFailedTiles(failedTiles);
    if (!IsStopRequested() && !failedTiles.empty())
    {
        const Tile& first = failedTiles.front();
        downloadError_ = fmt::format(
            "瓦片下载失败：已重试 {} 轮，仍有 {} 个瓦片不可用；首个 z{} ({}, {})，缓存：{}",
            kMaximumDownloadRetryRounds, failedTiles.size(), first.Zoom, first.X, first.Y,
            PathToUtf8String(GetTileFilePath(first)));
        fmt::print("{}\n", downloadError_);
        DowningCount = static_cast<int32_t>(targetTiles.size() - failedTiles.size());
        ReportProgress("下载失败", GetDowningCount(), TotalDownCount, failedTiles.size());
    }
	else if (!IsStopRequested() && (PostProcessEnabled ||
		PostProcessMaxZoomOnlyEnabled || PostProcessMergedImageEnabled ||
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

void DownTool::DownToolBase::DownloadTileBatch(const std::vector<Tile>& tiles)
{
    for (const Tile& tile : tiles)
    {
        if (IsStopRequested())
        {
            break;
        }
        DownloadTile(tile.X, tile.Y);
    }
    while (!curlEasyHandles.empty() && !IsStopRequested())
    {
        WaitDown();
    }
    fmt::print("\n");
}

bool DownTool::DownToolBase::WaitForDownloadRetry(uint32_t retryRound) const
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(retryRound);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (IsStopRequested())
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return !IsStopRequested();
}

void DownTool::DownToolBase::RetryFailedTiles(std::vector<Tile>& failedTiles)
{
    for (uint32_t round = 1;
         round <= kMaximumDownloadRetryRounds && !failedTiles.empty() && !IsStopRequested();
         ++round)
    {
        DowningCount = static_cast<int32_t>(TotalDownCount - failedTiles.size());
        downloadStage_ = fmt::format("等待重试下载瓦片 {}/{}", round, kMaximumDownloadRetryRounds);
        fmt::print("{}：剩余 {} 个，等待 {} 秒。\n", downloadStage_, failedTiles.size(), round);
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount, failedTiles.size());
        if (!WaitForDownloadRetry(round))
        {
            return;
        }

        downloadStage_ = fmt::format("重试下载瓦片 {}/{}", round, kMaximumDownloadRetryRounds);
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount, failedTiles.size());
        DownloadTileBatch(failedTiles);
        if (IsStopRequested())
        {
            return;
        }
        // 只复查本轮失败项，避免每轮重新扫描大批量任务的全部缓存。
        failedTiles = ScanCacheAndCollectPendingTiles(failedTiles, "重试后缓存检测", false, true);
        DowningCount = static_cast<int32_t>(TotalDownCount - failedTiles.size());
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount, failedTiles.size());
    }
}

bool DownTool::DownToolBase::IsStopRequested() const
{
	return bStop.load(std::memory_order_relaxed) ||
		(CancellationRequestedCallback && CancellationRequestedCallback());
}

void DownTool::DownToolBase::ReportProgress(
	std::string_view stage, uint64_t completed, uint64_t total, uint64_t failed) const
{
	if (ProgressCallback)
	{
		ProgressCallback(DownToolProgress{std::string(stage), completed, total, failed});
	}
}

void DownTool::DownToolBase::BuildTargetTiles(std::vector<Tile>& targetTiles)
{
    std::vector<Tile> requested;
    BuildRequestedTiles(requested);
    if (!IsTileOffset())
    {
        targetTiles = std::move(requested);
        return;
    }
    targetTiles.clear();
    std::unordered_set<Tile, TileHash, TileEqual> uniqueTiles;
    for (const Tile& target : requested)
    {
        if (IsStopRequested())
        {
            break;
        }
        const TileRange offset = GetOffsetTiles(target);
        for (int32_t x = offset.MinTile.X; x <= offset.MaxTile.X; ++x)
        {
            for (int32_t y = offset.MinTile.Y; y <= offset.MaxTile.Y; ++y)
            {
                const Tile tile(x, y, Zoom);
                if (uniqueTiles.insert(tile).second)
                {
                    targetTiles.push_back(tile);
                }
            }
        }
    }
}

void DownTool::DownToolBase::BuildRequestedTiles(std::vector<Tile>& targetTiles) const
{
    BuildRequestedTilesAtZoom(Zoom, targetTiles);
}

std::vector<DownTool::TileRange>
DownTool::DownToolBase::BuildCandidateTileRanges(uint32_t zoom) const
{
    const int32_t scale = static_cast<int32_t>(uint64_t{1} << (Zoom - zoom));
    const Tile minimum(FloorDiv(MinTile.X, scale), FloorDiv(MinTile.Y, scale), zoom);
    const Tile maximum(FloorDiv(MaxTile.X, scale), FloorDiv(MaxTile.Y, scale), zoom);
    if (DownloadPolygonsWgs84.empty())
    {
        return {{minimum, maximum}};
    }
    std::vector<TileRange> ranges;
    for (const Wgs84Ring& ring : DownloadPolygonsWgs84)
    {
        const auto bounds = CalculateGeographicBounds({ring});
        if (!bounds)
        {
            continue;
        }
        Tile first;
        Tile last;
        double pixelX = 0.;
        double pixelY = 0.;
        if (!TryGetTilePixelForLonLat({bounds->west, bounds->north},
                zoom, first, pixelX, pixelY) ||
            !TryGetTilePixelForLonLat({bounds->east, bounds->south},
                zoom, last, pixelX, pixelY))
        {
            return {{minimum, maximum}};
        }
        ranges.emplace_back(
            Tile(std::max(minimum.X, std::min(first.X, last.X) - 1),
                std::max(minimum.Y, std::min(first.Y, last.Y) - 1), zoom),
            Tile(std::min(maximum.X, std::max(first.X, last.X) + 1),
                std::min(maximum.Y, std::max(first.Y, last.Y) + 1), zoom));
    }
    return ranges;
}

bool DownTool::DownToolBase::TargetTileIntersectsDownloadRange(
	const Tile& tile) const
{
	if (DownloadPolygonsWgs84.empty() && !fbxDownloadClipper_)
	{
		return true;
	}

	LonLat northWest;
	LonLat southEast;
	if (!TryGetLonLatForTilePixel(tile, 0., 0., northWest) ||
		!TryGetLonLatForTilePixel(
			tile,
			static_cast<double>(TileSize),
			static_cast<double>(TileSize),
			southEast))
	{
		// 无法换算时保守保留，避免因为图商投影异常漏掉边缘瓦片。
		return true;
	}
	const GeographicBounds bounds{
		std::min(northWest.lon, southEast.lon),
		std::max(northWest.lon, southEast.lon),
		std::min(northWest.lat, southEast.lat),
		std::max(northWest.lat, southEast.lat)
	};
    if (!GeographicPolygonsIntersectBounds(DownloadPolygonsWgs84, bounds))
    {
        return false;
    }
    if (fbxDownloadClipper_)
    {
        bool visible = false;
        std::string error;
        if (!fbxDownloadClipper_->HasVisibleArea(bounds, visible, error))
        {
            if (IsStopRequested())
            {
                return false;
            }
            throw std::runtime_error(error);
        }
        return visible;
    }
    return true;
}

void DownTool::DownToolBase::BuildRequestedTilesAtZoom(
	uint32_t zoom, std::vector<Tile>& targetTiles) const
{
	targetTiles.clear();
	if (IsStopRequested() || zoom > Zoom)
	{
		return;
	}

	const uint32_t zoomOffset = Zoom - zoom;
	if (zoomOffset >= 31)
	{
		return;
	}

    std::unordered_set<Tile, TileHash, TileEqual> selected;
    for (const TileRange& range : BuildCandidateTileRanges(zoom))
    {
        for (int32_t x = range.MinTile.X; x <= range.MaxTile.X; ++x)
        {
            if (IsStopRequested())
            {
                return;
            }
            for (int32_t y = range.MinTile.Y; y <= range.MaxTile.Y; ++y)
            {
                if (IsStopRequested())
                {
                    return;
                }
                const Tile tile(x, y, zoom);
                if (TargetTileIntersectsDownloadRange(tile) && selected.insert(tile).second)
                {
                    targetTiles.push_back(tile);
                }
            }
        }
    }
}

DownTool::TileCacheState DownTool::DownToolBase::GetTileCacheState(
	const Tile& tile, bool includeFallbackTiles) const
{
	const std::filesystem::path tilePath = GetTileFilePath(tile);
	std::error_code existsError;
	const bool tileExists = std::filesystem::exists(tilePath, existsError);
	if (existsError)
	{
		return TileCacheState::Invalid;
	}

	if (tileExists && !IsUnavailableTileFile(tilePath, tile) && IsValidImageFile(tilePath))
	{
		return TileCacheState::Ready;
	}

	if (includeFallbackTiles && IsValidImageFile(GetFallbackTileFilePath(tile)))
	{
		return TileCacheState::Ready;
	}

	if (!tileExists)
	{
		return TileCacheState::Missing;
	}

	if (IsUnavailableTileFile(tilePath, tile))
	{
		return TileCacheState::Unavailable;
	}

	return TileCacheState::Invalid;
}

std::vector<DownTool::Tile> DownTool::DownToolBase::ScanCacheAndCollectPendingTiles(
	const std::vector<Tile>& targetTiles,
	const std::string& phaseName,
	bool updateProgress,
	bool includeFallbackTiles)
{
	if (IsStopRequested())
	{
		return {};
	}

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
			states[index] = GetTileCacheState(targetTiles[index], includeFallbackTiles);
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
						if (IsStopRequested())
						{
							break;
						}
						const size_t blockBegin = nextTileIndex.fetch_add(
							CacheScanBatchSize, std::memory_order_relaxed);
						if (blockBegin >= targetTiles.size())
						{
							break;
						}

						const size_t blockEnd =
							std::min(blockBegin + CacheScanBatchSize, targetTiles.size());
						size_t processedCount = 0;
						for (size_t index = blockBegin; index < blockEnd; index++)
						{
							if (IsStopRequested())
							{
								break;
							}
							scanTile(index);
							processedCount++;
						}
						scannedTileCount.fetch_add(processedCount, std::memory_order_relaxed);
						if (processedCount != blockEnd - blockBegin)
						{
							break;
						}
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
			if (IsStopRequested())
			{
				break;
			}
			scanTile(index);
			scannedTileCount.fetch_add(1, std::memory_order_relaxed);
		}
	}

	size_t lastPrintedCount = 0;
	while (lastPrintedCount < targetTiles.size())
	{
		if (IsStopRequested())
		{
			break;
		}
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
	const size_t finalScannedCount = scannedTileCount.load(std::memory_order_relaxed);
	fmt::print("\r{}: {}/{}，worker {}\n",
		phaseName, finalScannedCount, targetTiles.size(), scanWorkers.size());
	if (IsStopRequested())
	{
		return {};
	}
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
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount);
    }

	return pendingTiles;
}

void DownTool::DownToolBase::DownloadTile(int32_t x, int32_t y)
{
	// 等待至少一个下载完成
	while (!IsStopRequested() &&
		curlEasyHandles.size() >= GetEffectiveQueueCount(MaxDownQueue))
	{
		WaitDown();
	}
	if (IsStopRequested())
	{
		return;
	}

	Tile tile(x, y, GetZoom());
	std::filesystem::path nativePath = GetTileFilePath(tile);
	while (!IsStopRequested() && IsCoveredByPendingLowerZoomRepair(tile))
	{
		WaitDown();
	}
	if (IsStopRequested())
	{
		return;
	}

	if (IsTileDownloading(tile))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount);
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
			RemoveFileIfExists(nativePath);
			if (!RepairTileFromLowerZoom(tile, nativePath))
			{
				fmt::print("\n无法从低级别瓦片修复占位图: {}\n", PathToUtf8String(nativePath));
			}

			DowningCount++;
			fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
            ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount);
            return;
		}
		else if (IsValidImageFile(nativePath))
		{
			DowningCount++;
			fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
            ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount);
            return;
		}
	}

	if (!QueueTileDownload(tile, nativePath, CurlRequestType::Tile, tile.Zoom))
	{
		DowningCount++;
		fmt::print("\r下载进度: {}/{}", GetDowningCount(), TotalDownCount);
        ReportProgress(downloadStage_, GetDowningCount(), TotalDownCount);
    }
}

std::filesystem::path DownTool::DownToolBase::GetTileFilePath(const Tile& tile) const
{
	std::string filePath = fmt::format("{}/{}/{}/{}.png", CachePath, tile.Zoom, tile.X, tile.Y);
	return std::filesystem::u8path(filePath);
}

std::filesystem::path DownTool::DownToolBase::GetFallbackTileFilePath(const Tile& tile) const
{
	std::string filePath = fmt::format(
		"{}/fallback/{}/{}/{}.png", CachePath, tile.Zoom, tile.X, tile.Y);
	return std::filesystem::u8path(filePath);
}

std::filesystem::path DownTool::DownToolBase::GetPostProcessSourceTileFilePath(
	const Tile& tile) const
{
	const std::filesystem::path tilePath = GetTileFilePath(tile);
	if (IsValidImageFile(tilePath) && !IsUnavailableTileFile(tilePath, tile))
	{
		return tilePath;
	}

	return GetFallbackTileFilePath(tile);
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
	curl_easy_setopt(easy, CURLOPT_TIMEOUT, TileRequestTimeoutSeconds);
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, TileConnectTimeoutSeconds);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	// 吊销分发点离线时允许继续验证证书链，仍拒绝无效及已知吊销的证书。
	curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
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
			CanBatchRepairFromParentZoom(context.tile.Zoom, tile.Zoom) &&
			TileInParentRange(context.tile, tile, tile.Zoom))
		{
			return true;
		}
	}

	return false;
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
		if (!CanBatchRepairFromParentZoom(parentZoom, tile.Zoom))
		{
			break;
		}
		const uint32_t zoomOffset = tile.Zoom - parentZoom;
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

bool DownTool::DownToolBase::RepairTilesFromParent(
	const Tile& parentTile, uint32_t targetZoom)
{
	if (!CanBatchRepairFromParentZoom(parentTile.Zoom, targetZoom))
	{
		return false;
	}

	const uint32_t zoomOffset = targetZoom - parentTile.Zoom;
	const uint32_t scale = 1u << zoomOffset;
	const uint64_t batchTileCount = static_cast<uint64_t>(scale) * scale;

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
		if (IsValidImageFile(childPath) && !IsUnavailableTileFile(childPath, childTile))
		{
			return TileRepairWriteResult::Existing;
		}
		const std::filesystem::path fallbackChildPath = GetFallbackTileFilePath(childTile);
		if (IsValidImageFile(fallbackChildPath))
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
		if (!WriteImage(fallbackChildPath, resized))
		{
			return TileRepairWriteResult::Failed;
		}

		return TileRepairWriteResult::Written;
	};

	std::vector<TileRepairTask> repairTasks;
	repairTasks.reserve(static_cast<size_t>(batchTileCount));
	for (int32_t childX = 0; childX < static_cast<int32_t>(scale); childX++)
	{
		if (IsStopRequested())
		{
			return false;
		}
		for (int32_t childY = 0; childY < static_cast<int32_t>(scale); childY++)
		{
			if (IsStopRequested())
			{
				return false;
			}
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
		},
		[this] { return IsStopRequested(); });

	if (IsStopRequested())
	{
		return false;
	}

	const uint32_t readyTileCount = atomicReadyTileCount.load(std::memory_order_relaxed);
	const uint32_t repairedTileCount =
		atomicRepairedTileCount.load(std::memory_order_relaxed);

	fmt::print("\n使用低级别瓦片批量修复占位图: z{} ({}, {}) -> z{}，可用 {} / {} 张，写入 {} 张\n",
		parentTile.Zoom, parentTile.X, parentTile.Y, targetZoom,
		readyTileCount, batchTileCount, repairedTileCount);
	return readyTileCount == batchTileCount;
}

void DownTool::DownToolBase::SetFbxExcludedPolygonsWgs84(const Wgs84Polygons& value)
{
    fbxExcludedPolygonsWgs84_ = value;
    fbxDownloadClipper_.reset();
}

bool DownTool::DownToolBase::PrepareFbxDownloadRange()
{
    fbxDownloadClipper_.reset();
    // 只有纯底板导出可跳过内圈影像；XYZ、大图等其他产物仍使用完整下载范围。
    if (!PostProcessFbxEnabled || PostProcessEnabled || PostProcessMaxZoomOnlyEnabled ||
        PostProcessMergedImageEnabled || fbxExcludedPolygonsWgs84_.empty())
    {
        return true;
    }
    auto clipper = std::make_unique<FbxTerrainRegionClipper>();
    std::string error;
    if (!clipper->Initialize({}, fbxExcludedPolygonsWgs84_, error,
                             [this] { return IsStopRequested(); }))
    {
        fmt::print("底板下载范围准备失败：{}\n", error);
        return false;
    }
    fbxDownloadClipper_ = std::move(clipper);
    return true;
}

void DownTool::DownToolBase::RunPostProcess()
{
    if (!PrepareFbxDownloadRange())
    {
        return;
    }
	const auto postProcessStartTime = std::chrono::steady_clock::now();
	std::vector<Tile> targetTiles;
	BuildRequestedTiles(targetTiles);
	if (IsStopRequested())
	{
		return;
	}
	if (targetTiles.empty())
	{
		fmt::print("后处理跳过：没有可处理的目标瓦片。\n");
		return;
	}

	fmt::print("开始瓦片后处理，输出目录：{}\n", PathToUtf8String(GetPostProcessRootPath()));
	bool maxZoomTilesReady = false;
	if (!IsStopRequested() && (PostProcessEnabled ||
		PostProcessMaxZoomOnlyEnabled || PostProcessFbxEnabled))
	{
		auto phaseStartTime = std::chrono::steady_clock::now();
		maxZoomTilesReady = GeneratePostProcessMaxZoomTiles(targetTiles);
		fmt::print("最高层级后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	if (!IsStopRequested() && PostProcessEnabled)
	{
		if (maxZoomTilesReady)
		{
			auto phaseStartTime = std::chrono::steady_clock::now();
			GeneratePostProcessLowerZoomTiles(targetTiles);
			fmt::print("低层级金字塔后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
			if (IsStopRequested())
			{
				return;
			}
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

	if (!IsStopRequested() && PostProcessMergedImageEnabled)
	{
		const auto phaseStartTime = std::chrono::steady_clock::now();
		const bool postProcessTilesAvailable = PostProcessEnabled && maxZoomTilesReady;
		if (!GeneratePostProcessMergedImage(postProcessTilesAvailable))
		{
			fmt::print("后处理警告：合并大图生成失败或不完整。\n");
		}
		fmt::print("合并大图后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	if (!IsStopRequested() && PostProcessFbxEnabled)
	{
		const auto phaseStartTime = std::chrono::steady_clock::now();
		if (!GeneratePostProcessFbx(maxZoomTilesReady))
		{
			fmt::print("后处理警告：FBX 导出失败或不完整。\n");
		}
		fmt::print("FBX 后处理耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	}

	if (!IsStopRequested())
	{
		fmt::print("瓦片后处理完成，总耗时：{:.2f}s\n",
			SecondsSince(postProcessStartTime));
	}
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
			if (currentCount % 64 == 0 || currentCount == targetTiles.size())
			{
				ReportProgress("转换 XYZ 瓦片", currentCount, targetTiles.size(),
					failedCount.load(std::memory_order_relaxed));
			}
		},
		[this] { return IsStopRequested(); });

	if (IsStopRequested())
	{
		return false;
	}

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
		if (score < bestScore || (score == bestScore && zoom > bestZoom))
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
			if (currentCount % 64 == 0 || currentCount == targetTiles.size())
			{
				ReportProgress(phaseName, currentCount, targetTiles.size(),
					failedCount.load(std::memory_order_relaxed));
			}
		},
		[this] { return IsStopRequested(); });

	if (IsStopRequested())
	{
		return false;
	}

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
	while (!IsStopRequested() && !currentLevelTiles.empty() &&
		currentLevelTiles.front().Zoom > 0)
	{
		const std::unordered_set<Tile, TileHash, TileEqual> currentLevelTileSet(
			currentLevelTiles.begin(), currentLevelTiles.end());
		std::vector<Tile> parentTiles;
		parentTiles.reserve(currentLevelTiles.size() / 4 + 1);
		std::unordered_set<Tile, TileHash, TileEqual> uniqueParents;
		uniqueParents.reserve(currentLevelTiles.size() / 4 + 1);

		for (const Tile& childTile : currentLevelTiles)
		{
			if (IsStopRequested())
			{
				return;
			}
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
                if (WriteXyzTileImage(GetPostProcessTileFilePath(parentTile), targetImage))
				{
					writtenCount.fetch_add(1, std::memory_order_relaxed);
				}
			},
			[this] { return IsStopRequested(); });

		if (IsStopRequested())
		{
			return;
		}

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
	if (IsStopRequested())
	{
		return {};
	}

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
		if (IsStopRequested())
		{
			return {};
		}
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
		if (IsStopRequested())
		{
			return {};
		}
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
		if (IsStopRequested())
		{
			return {};
		}
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
	if (IsStopRequested())
	{
		return false;
	}
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
		if (IsStopRequested())
		{
			return false;
		}
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
	if (IsStopRequested())
	{
		return false;
	}

	const int outputWidth = static_cast<int>(outputSize.width);
	const int outputHeight = static_cast<int>(outputSize.height);
	int32_t sampleTileMinX = sampleTiles.front().X;
	int32_t sampleTileMaxX = sampleTiles.front().X;
	int32_t sampleTileMinY = sampleTiles.front().Y;
	int32_t sampleTileMaxY = sampleTiles.front().Y;
	for (const Tile& tile : sampleTiles)
	{
		sampleTileMinX = std::min(sampleTileMinX, tile.X);
		sampleTileMaxX = std::max(sampleTileMaxX, tile.X);
		sampleTileMinY = std::min(sampleTileMinY, tile.Y);
		sampleTileMaxY = std::max(sampleTileMaxY, tile.Y);
	}
	const uint64_t sampleTileXCount = TileRangeCount(sampleTileMinX, sampleTileMaxX);
	const uint64_t sampleTileYCount = TileRangeCount(sampleTileMinY, sampleTileMaxY);
	if (TileSize == 0 || sampleTileXCount == 0 || sampleTileYCount == 0 ||
		sampleTileXCount >
			static_cast<uint64_t>(std::numeric_limits<int>::max()) / TileSize ||
		sampleTileYCount >
			static_cast<uint64_t>(std::numeric_limits<int>::max()) / TileSize)
	{
		fmt::print("合并大图失败：采样画布尺寸过大。\n");
		return false;
	}
	const int sampleCanvasWidth = static_cast<int>(sampleTileXCount * TileSize);
	const int sampleCanvasHeight = static_cast<int>(sampleTileYCount * TileSize);
	const double outputImageMemoryMb =
		static_cast<double>(outputSize.width) * outputSize.height * 4. / 1024. / 1024.;
	const double sampleCanvasMemoryMb =
		static_cast<double>(sampleCanvasWidth) * sampleCanvasHeight * 4. /
		1024. / 1024.;
	fmt::print("合并大图内存估算：采样画布 {:.1f} MB，输出 {:.1f} MB\n",
		sampleCanvasMemoryMb,
		outputImageMemoryMb);
	phaseStartTime = std::chrono::steady_clock::now();
	cv::Mat sampleCanvas(
		sampleCanvasHeight,
		sampleCanvasWidth,
		CV_8UC4,
		cv::Scalar(0, 0, 0, 0));
	fmt::print("合并大图内存分配耗时：{:.2f}s\n", SecondsSince(phaseStartTime));

	std::atomic_uint32_t processedCount = 0;
	std::atomic_uint32_t failedCount = 0;
	const auto reportSampleProgress = [&](uint32_t currentCount)
	{
		if (currentCount % 1024 == 0 || currentCount == sampleTiles.size())
		{
			fmt::print("\r大图合并: {}/{}",
				currentCount,
				sampleTiles.size());
		}
		if (currentCount % 64 == 0 || currentCount == sampleTiles.size())
		{
			ReportProgress("合并大图", currentCount, sampleTiles.size(),
				failedCount.load(std::memory_order_relaxed));
		}
	};

	phaseStartTime = std::chrono::steady_clock::now();
	ParallelForIndex(
		sampleTiles.size(),
		MaxDownQueue,
		[&](size_t index)
		{
			const Tile& tile = sampleTiles[index];
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
				reportSampleProgress(currentCount);
				return;
			}

			const int canvasX = (tile.X - sampleTileMinX) * tileSize;
			const int canvasY = (tile.Y - sampleTileMinY) * tileSize;
			tileImage.copyTo(sampleCanvas(cv::Rect(
				canvasX, canvasY, tileSize, tileSize)));

			const uint32_t currentCount =
				processedCount.fetch_add(1, std::memory_order_relaxed) + 1;
			reportSampleProgress(currentCount);
		},
		[this] { return IsStopRequested(); });

	if (IsStopRequested())
	{
		return false;
	}

	fmt::print("\r大图合并: {}/{}，失败 {}\n",
		sampleTiles.size(), sampleTiles.size(), failedCount.load());
	ReportProgress("合并大图", sampleTiles.size(), sampleTiles.size(), failedCount.load());
	fmt::print("大图像素合并耗时：{:.2f}s\n", SecondsSince(phaseStartTime));
	if (failedCount.load() != 0)
	{
		return false;
	}

	phaseStartTime = std::chrono::steady_clock::now();
	const double requestMinSamplePixelX =
		static_cast<double>(requestMinX) * tileSize / sampleScale;
	const double requestMinSamplePixelY =
		static_cast<double>(requestMinY) * tileSize / sampleScale;
	const double requestMaxSamplePixelX =
		static_cast<double>(requestMaxX) * tileSize / sampleScale;
	const double requestMaxSamplePixelY =
		static_cast<double>(requestMaxY) * tileSize / sampleScale;
	const double canvasOriginX = static_cast<double>(sampleTileMinX) * tileSize;
	const double canvasOriginY = static_cast<double>(sampleTileMinY) * tileSize;
	const double cropX0 = requestMinSamplePixelX - canvasOriginX;
	const double cropY0 = requestMinSamplePixelY - canvasOriginY;
	const double cropWidth = requestMaxSamplePixelX - requestMinSamplePixelX;
	const double cropHeight = requestMaxSamplePixelY - requestMinSamplePixelY;
	if (cropX0 < 0. || cropY0 < 0. || cropWidth <= 0. || cropHeight <= 0. ||
		cropX0 + cropWidth > sampleCanvas.cols ||
		cropY0 + cropHeight > sampleCanvas.rows)
	{
		fmt::print("合并大图失败：请求范围超出采样画布。\n");
		return false;
	}

	const double scaleX = static_cast<double>(outputWidth) / cropWidth;
	const double scaleY = static_cast<double>(outputHeight) / cropHeight;
	const bool canUseSourceCropDirectly =
		std::abs(scaleX - 1.) <= 1.e-9 && std::abs(scaleY - 1.) <= 1.e-9 &&
		std::abs(cropX0 - std::round(cropX0)) <= 1.e-9 &&
		std::abs(cropY0 - std::round(cropY0)) <= 1.e-9;
	cv::Mat mergedImage;
	if (canUseSourceCropDirectly)
	{
		mergedImage = sampleCanvas(cv::Rect(
			static_cast<int>(std::round(cropX0)),
			static_cast<int>(std::round(cropY0)),
			outputWidth,
			outputHeight));
	}
	else
	{
		mergedImage = cv::Mat(
			outputHeight, outputWidth, CV_8UC4, cv::Scalar(0, 0, 0, 0));
		const cv::Mat transform = (cv::Mat_<double>(2, 3) <<
			scaleX, 0., -cropX0 * scaleX,
			0., scaleY, -cropY0 * scaleY);
		try
		{
			cv::warpAffine(
				sampleCanvas,
				mergedImage,
				transform,
				mergedImage.size(),
				cv::INTER_LINEAR,
				cv::BORDER_REPLICATE);
		}
		catch (const cv::Exception& exception)
		{
			fmt::print("合并大图失败：采样画布缩放失败：{}\n", exception.what());
			return false;
		}
	}
	fmt::print("大图采样画布拼接与缩放耗时：{:.2f}s\n",
		SecondsSince(phaseStartTime));

	if (IsStopRequested())
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
    FbxTerrainRegionClipper regionClipper;
    std::string regionError;
    if (!regionClipper.Initialize(DownloadPolygonsWgs84, fbxExcludedPolygonsWgs84_,
                                 regionError, [this] { return IsStopRequested(); }))
    {
        fmt::print("FBX 导出失败：{}\n", regionError);
        return false;
    }
    std::vector<std::unique_ptr<FbxTerrainFlattenRegion>> flattenRegions;
    if (FbxTerrainHeightEnabled)
    {
        for (const auto& region : fbxFlattenRegions_)
        {
            auto flattened = std::make_unique<FbxTerrainFlattenRegion>();
            if (!flattened->Initialize(region, regionError, [this] { return IsStopRequested(); }))
            {
                fmt::print("FBX 平整区域初始化失败：{}\n", regionError);
                return false;
            }
            flattenRegions.push_back(std::move(flattened));
        }
    }
    const std::string distanceError = fbxDistanceLod_.Validate();
    if (!distanceError.empty())
    {
        fmt::print("FBX 导出失败：{}\n", distanceError);
        return false;
    }
    if (FbxTerrainHeightEnabled && FbxTerrainMaximumTriangles == 1)
    {
        fmt::print("FBX 导出失败：每块三角面上限必须为 0（不限）或至少 2。\n");
        return false;
    }
	const auto totalStartTime = std::chrono::steady_clock::now();
	if (IsStopRequested())
	{
		return false;
	}
	if (!postProcessTilesAvailable)
	{
		fmt::print("FBX 导出失败：最高层级后处理瓦片不可用。\n");
		return false;
	}
	if (FbxTerrainHeightEnabled && !FbxElevationSampler)
	{
		fmt::print("FBX 导出失败：已启用地形高度，但没有可用的高程采样器。\n");
		return false;
	}

	if (!IsPowerOfTwo(FbxTextureSize) || FbxTextureSize > 16384 ||
		FbxTextureSize < TileSize || FbxTextureSize % TileSize != 0)
	{
		fmt::print(
			"FBX 导出失败：贴图尺寸必须是 2 的次幂、不能超过 16384，且必须是瓦片尺寸的整数倍。\n");
		return false;
	}

    const std::string textureFormat = NormalizeFbxTextureFormat(FbxTextureFormat);
    if (textureFormat != "png" && textureFormat != "jpg")
    {
        fmt::print("FBX 导出失败：贴图格式只支持 png 或 jpg。\n");
        return false;
    }
    const std::string textureExtension = "." + textureFormat;

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
	const std::filesystem::path textureRoot = outputRoot;
	const FbxCoordinateContext coordinateContext =
		BuildFbxCoordinateContext(CenterLonLat);

	std::vector<FbxTextureOutputInfo> textureInfos;
	textureInfos.reserve(static_cast<size_t>(std::max(0, maxGridX - minGridX + 1)) *
		static_cast<size_t>(std::max(0, maxGridY - minGridY + 1)));

	for (int32_t gridY = minGridY; gridY <= maxGridY; gridY++)
	{
		if (IsStopRequested())
		{
			return false;
		}
		for (int32_t gridX = minGridX; gridX <= maxGridX; gridX++)
		{
			if (IsStopRequested())
			{
				return false;
			}
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

			const GeographicBounds textureGeographicBounds{
				std::min(northWestLonLat.lon, southEastLonLat.lon),
				std::max(northWestLonLat.lon, southEastLonLat.lon),
				std::min(northWestLonLat.lat, southEastLonLat.lat),
				std::max(northWestLonLat.lat, southEastLonLat.lat)
			};
			if (!GeographicPolygonsIntersectBounds(
					DownloadPolygonsWgs84, textureGeographicBounds))
			{
				continue;
			}
            bool visible = false;
            if (!regionClipper.HasVisibleArea(textureGeographicBounds, visible, regionError))
            {
                fmt::print("FBX 导出失败：{}\n", regionError);
                return false;
            }
            if (!visible)
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
                fbxDistanceLod_.ResolveTextureSize(distanceCm / 100.,
                    CalculateFbxOutputTextureSize(FbxTextureSize, distanceOffset));
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
            textureInfo.terrainMaximumTriangles =
                fbxDistanceLod_.ResolveMaximumTriangles(distanceCm / 100.,
                    FbxTerrainMaximumTriangles);
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
			textureInfo.imageFormat = textureFormat;
			textureInfo.texturePath = textureRoot / fmt::format(
				"{}{}", textureName, textureExtension);
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

	fmt::print("FBX 贴图生成：{} 张，贴图尺寸 {}，格式 {}，输出目录 {}\n",
		textureInfos.size(),
		FbxTextureSize,
		textureFormat,
		PathToUtf8String(textureRoot));

	auto readTileImage = [&](const Tile& tile)
	{
		if (IsStopRequested())
		{
			return cv::Mat{};
		}
		if (!TargetTileIntersectsDownloadRange(tile))
		{
			return cv::Mat(
				static_cast<int>(TileSize),
				static_cast<int>(TileSize),
				CV_8UC4,
				cv::Scalar(0, 0, 0, 0));
		}
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
			ReportProgress("生成 FBX 贴图", currentCount, textureInfos.size(),
				failedCount.load(std::memory_order_relaxed));
		},
		[this] { return IsStopRequested(); });
	if (IsStopRequested())
	{
		return false;
	}
	fmt::print("\rFBX 贴图生成: {}/{}，失败 {}\n",
		textureInfos.size(),
		textureInfos.size(),
		failedCount.load());
	if (failedCount.load() != 0)
	{
		return false;
	}

	if (IsStopRequested())
	{
		return false;
	}
    if (!WriteTextFile(textureRoot / "manifest.json",
                       BuildFbxManifestJson(textureInfos, textureRoot,
                                            FbxShape == FbxPlanetShape::Plane    ? "plane"
                                            : FbxShape == FbxPlanetShape::Sphere ? "sphere"
                                                                                 : "both",
                                            FbxTextureSize, Zoom, TileSize, textureFormat,
                                            FbxTerrainHeightEnabled, FbxElevationReferenceMeters,
                                            FbxTerrainSampleRateMeters, FbxTerrainMaximumTriangles,
                                            fbxDistanceLod_, fbxPerTile_)))
    {
		fmt::print("FBX 警告：manifest.json 写入失败。\n");
	}

    auto buildMeshDatas = [&](FbxPlanetShape shape) {
        std::vector<FbxTerrainMeshPlan> terrainPlans;
        if (FbxTerrainHeightEnabled)
        {
            const auto terrainPlanStartTime = std::chrono::steady_clock::now();
            terrainPlans.reserve(textureInfos.size());
            for (const FbxTextureOutputInfo& textureInfo : textureInfos)
            {
                if (IsStopRequested())
                {
                    return std::vector<FbxMeshData>{};
                }
                auto plan = BuildAdaptiveFbxTerrainMeshPlan(
                    textureInfo, requestBounds, FbxElevationReferenceMeters,
                    FbxTerrainSampleRateMeters, FbxElevationSampler,
                    shape == FbxPlanetShape::Sphere ? &coordinateContext : nullptr);
                if (!plan)
                {
                    fmt::print("FBX 导出失败：无法生成地形采样网格。\n");
                    return std::vector<FbxMeshData>{};
                }
                terrainPlans.push_back(std::move(*plan));
            }
            if (!SynchronizeFbxTerrainMeshPlanEdges(terrainPlans))
            {
                fmt::print("FBX 导出失败：相邻地形块的采样网格无法对齐。\n");
                return std::vector<FbxMeshData>{};
            }
            fmt::print("FBX 高程采样与网格简化耗时：{:.2f}s\n", SecondsSince(terrainPlanStartTime));
        }
        const uint32_t subdivision =
			shape == FbxPlanetShape::Sphere || FbxTerrainHeightEnabled ?
			FbxMeshSubdivision : 1u;
		std::vector<FbxMeshData> meshDatas;
		meshDatas.reserve(textureInfos.size());
		for (size_t textureIndex = 0; textureIndex < textureInfos.size();
			textureIndex++)
		{
			const FbxTextureOutputInfo& textureInfo = textureInfos[textureIndex];
			if (IsStopRequested())
			{
				break;
			}
			FbxMeshData meshData;
			meshData.name = MakeFbxMeshName(textureInfo.gridX, textureInfo.gridY);
			meshData.texturePath = textureInfo.texturePath;
			if (FbxTerrainHeightEnabled)
			{
				meshData.vertices = BuildAdaptiveFbxTerrainVertices(
					terrainPlans[textureIndex],
					coordinateContext,
					shape,
					meshData.indices);
			}
			else
			{
                uint32_t meshSubdivision = subdivision;
                if (shape == FbxPlanetShape::Sphere)
                {
                    // 无 DEM 也需细分零高程椭球，不能让一个大平面跨过地球曲率。
                    const double span =
                        std::max(std::abs(LongitudeToWebMercatorX(textureInfo.bounds.east) -
                                          LongitudeToWebMercatorX(textureInfo.bounds.west)),
                                 std::abs(LatitudeToWebMercatorY(textureInfo.bounds.north) -
                                          LatitudeToWebMercatorY(textureInfo.bounds.south)));
                    if (span > kMaximumFbxTerrainGridSize * kMaximumSphereSampleStepMeters)
                    {
                        fmt::print("FBX 球面块范围过大，请提高导出层级或减小贴图块尺寸。\n");
                        return std::vector<FbxMeshData>{};
                    }
                    meshSubdivision =
                        std::max(meshSubdivision,
                                 CalculateFbxTerrainGridSize(span, kMaximumSphereSampleStepMeters));
                }
                meshData.vertices = BuildFbxMeshVertices(
                    textureInfo, coordinateContext, shape, meshSubdivision, requestBounds, false,
                    FbxElevationReferenceMeters, FbxElevationSampler);
                meshData.indices = BuildFbxMeshIndices(meshSubdivision);
            }
			if (meshData.vertices.empty())
			{
				return std::vector<FbxMeshData>{};
			}
            if (!ClipFbxTerrainMesh(meshData, textureInfo, regionClipper, regionError))
            {
                fmt::print("FBX 导出失败：{}\n", regionError);
                return std::vector<FbxMeshData>{};
            }
            if (meshData.indices.empty())
            {
                continue;
            }
            for (const auto& region : flattenRegions)
            {
                if (!region->Apply(meshData, textureInfo,
                                   {coordinateContext, shape, FbxElevationReferenceMeters},
                                   regionError))
                {
                    fmt::print("FBX 平整区域处理失败：{}\n", regionError);
                    return std::vector<FbxMeshData>{};
                }
            }
            meshDatas.push_back(std::move(meshData));
        }
		AssignFbxMeshNormals(meshDatas);
		return meshDatas;
    };

    const auto exportMeshes = [&](FbxPlanetShape shape, std::string_view name) {
        const auto phaseStartTime = std::chrono::steady_clock::now();
        const std::vector<FbxMeshData> meshDatas = buildMeshDatas(shape);
        if (meshDatas.empty() || IsStopRequested())
        {
            return false;
        }
        if (!fbxPerTile_)
        {
            const auto path = outputRoot / fmt::format("{}.fbx", name);
            if (!ExportAssimpFbx(meshDatas, path))
            {
                return false;
            }
        }
        else
        {
            // 先统一计算相邻块法线，再拆分文件；各块继续使用任务中心和海拔零面。
            for (size_t index = 0; index < meshDatas.size(); ++index)
            {
                if (IsStopRequested())
                {
                    return false;
                }
                const auto& mesh = meshDatas[index];
                const auto path = outputRoot / fmt::format("{}_{}.fbx", name, mesh.name);
                if (!ExportAssimpFbx(std::span<const FbxMeshData>(&mesh, 1), path))
                {
                    return false;
                }
                ReportProgress("生成独立底板 FBX", index + 1, meshDatas.size());
            }
        }
        fmt::print("{} FBX 输出：{}，{} 个文件，耗时 {:.2f}s\n", name, PathToUtf8String(outputRoot),
                   fbxPerTile_ ? meshDatas.size() : 1, SecondsSince(phaseStartTime));
        return true;
    };

    bool meshSuccess = true;
    if (FbxShape == FbxPlanetShape::Plane || FbxShape == FbxPlanetShape::Both)
	{
		if (IsStopRequested())
		{
			return false;
		}
        meshSuccess = exportMeshes(FbxPlanetShape::Plane, "plane") && meshSuccess;
    }
	if (FbxShape == FbxPlanetShape::Sphere || FbxShape == FbxPlanetShape::Both)
	{
		if (IsStopRequested())
		{
			return false;
		}
        meshSuccess = exportMeshes(FbxPlanetShape::Sphere, "sphere") && meshSuccess;
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
        const ImageType sourceImageType = DetectImageType(sourcePath);
        if (sourceImageType == ImageType::Unknown ||
            IsUnavailableTileFile(sourcePath, targetTile))
        {
            return false;
        }

        const cv::Mat sourceImage = ReadImageUnchanged(sourcePath);
        if (sourceImage.empty())
        {
            return false;
        }
        const int tileSize = static_cast<int>(TileSize);
        const bool matchesTargetSize =
            sourceImage.cols == tileSize && sourceImage.rows == tileSize;
        if (matchesTargetSize && sourceImageType == ImageType::Jpeg)
        {
            return CopyFileReplacing(sourcePath, outputPath);
        }

        if (matchesTargetSize && HasTransparentPixels(sourceImage))
        {
            return CopyFileReplacing(sourcePath, outputPath);
        }

        return WriteXyzTileImage(
            outputPath,
            ResizeImageToSize(sourceImage, tileSize, tileSize));
    }

    const int tileSize = static_cast<int>(TileSize);
    return WriteXyzTileImage(
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

DownTool::TileHttpRequest DownTool::DownToolBase::BuildTileHttpRequest(const Tile& tile)
{
	TileHttpRequest request;
	request.url = MakeUri(tile);
	request.headers = GetRequestHeaders(tile);
	request.proxy = Proxy;
	return request;
}

bool DownTool::DownToolBase::IsTileDownloadUnavailable(
	const std::filesystem::path& tilePath,
	const std::map<std::string, std::string>& responseHeaders,
	const Tile& tile) const
{
	return IsUnavailableTileResponseHeader(responseHeaders, tile) ||
		IsUnavailableTileFile(tilePath, tile);
}

void DownTool::DownToolBase::CalculateDownRange()
{
	const std::optional<GeographicBounds> bounds =
		DownloadPolygonsWgs84.empty() ?
			CalculateGeographicBounds(GetCenterLonLat(), GetRadius()) :
			CalculateGeographicBounds(DownloadPolygonsWgs84);
	if (!bounds.has_value())
	{
		return;
	}
	MinLonLat = {bounds->west, bounds->south};
	MaxLonLat = {bounds->east, bounds->north};
}

bool DownTool::DownToolBase::TryProjectToSourceTile(
	const LonLat& lonLat,
	uint32_t zoom,
	Tile& tile,
	double& pixelX,
	double& pixelY) const
{
	const LonLat sourceLonLat = ConvertTargetLonLatToSourceLonLat(lonLat);
	return TryGetSourceTilePixelForLonLat(sourceLonLat, zoom, tile, pixelX, pixelY);
}

bool DownTool::DownToolBase::TryUnprojectFromSourceTile(
	const Tile& tile,
	double pixelX,
	double pixelY,
	LonLat& lonLat) const
{
	LonLat sourceLonLat;
	if (!TryGetSourceLonLatForTilePixel(tile, pixelX, pixelY, sourceLonLat))
	{
		return false;
	}

	lonLat = ConvertSourceLonLatToTargetLonLat(sourceLonLat);
	return true;
}

bool DownTool::DownToolBase::TryGetSourceTileMatrixSize(
	uint32_t zoom,
	uint32_t& columnCount,
	uint32_t& rowCount) const
{
	if (zoom >= 32)
	{
		return false;
	}

	columnCount = uint32_t{ 1 } << zoom;
	rowCount = columnCount;
	return columnCount != 0;
}
